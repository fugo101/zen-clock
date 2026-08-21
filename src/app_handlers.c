#include "app_handlers.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "backoff.h"
#include "bsp.h"
#include "deep_sleep.h"
#include "wifi_manager.h"
#include "sntp_sync.h"
#include "settings.h"
#include "status_bar.h"
#include "ble_provisioning.h"
#include "prov_screen.h"
#include "nav.h"
#include "input_policy.h"
#include "microlink.h"
#include "device_info_screen.h"
#include <freertos/queue.h>

static const char *const tag = "ZenClock";

// ============================================================
// WiFi reconnect — exponential backoff timer
// ============================================================

static esp_timer_handle_t s_reconnect_timer = NULL;
static esp_timer_handle_t s_ts_poll_timer = NULL;

// Reconnect backoff policy lives in components/backoff (shared with the NTP re-sync loop).
// RECONNECT_BUSY_RETRY_S is the short re-arm used when wifi_manager_start() refuses because the
// previous attempt has not unwound yet — that is not a failed attempt, so it must not consume a
// backoff step, and it deliberately bypasses the policy.
#define RECONNECT_BUSY_RETRY_S 5

static uint32_t s_reconnect_backoff_s = BACKOFF_START_S;

// How long ts_poll_cb waits for the LVGL lock before giving up on this tick.
#define TS_POLL_LOCK_TIMEOUT_MS 50
#define TS_POLL_PERIOD_US       (10ULL * 1000000ULL)

// How long on_wifi_event() waits for the LVGL lock before skipping a paint. Same 50ms value as
// TS_POLL_LOCK_TIMEOUT_MS, kept as its own constant since the two calls have different reasons
// for existing (see wifi_event_lvgl_lock() below).
#define WIFI_EVENT_LOCK_TIMEOUT_MS 50
static bool s_sntp_started = false;

// True while WiFi is associated but the DNS probe failed. Kept here rather than queried from the
// status bar so the recovery rule lives next to the events that drive it.
static bool s_wifi_unverified = false;

// Written once by the wifi_mgr task; read by the esp_timer task in ts_poll_cb(). No lock:
// the ts_poll timer is only armed *after* the assignment below, so a reader can never
// observe the pre-init value, and the rebind path does not reassign it. volatile keeps the
// compiler from caching the pointer across the two task contexts.
static microlink_t *volatile s_ml = NULL;

static bool schedule_reconnect_in(uint32_t delay_s);

// The device must survive an indefinite outage: no WiFi, or out of range. Every path out of this
// callback has to leave exactly one retry armed, or the clock goes offline until it is rebooted.
// NOLINTNEXTLINE(readability-non-const-parameter)
static void reconnect_timer_cb(void *arg)
{
  (void) arg;
  const esp_err_t ret = wifi_manager_start();
  if (ret != ESP_OK)
  {
    // Almost always ESP_ERR_INVALID_STATE: the previous attempt is still unwinding. A failed
    // scan+connect+verify cycle can outlast the initial 30s backoff, so this is reached in
    // normal operation — and the timer is one-shot and has already fired, so returning here
    // without re-arming would end retrying altogether.
    ESP_LOGW(tag, "Reconnect skipped (%s) — retrying shortly", esp_err_to_name(ret));
    schedule_reconnect_in(RECONNECT_BUSY_RETRY_S);
  }
}

// Arms the one-shot retry timer. Returns whether a retry is now pending.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static bool schedule_reconnect_in(const uint32_t delay_s)
{
  if (!s_reconnect_timer)
  {
    const esp_timer_create_args_t args = {.callback = reconnect_timer_cb, .name = "wifi_rc"};
    const esp_err_t cret = esp_timer_create(&args, &s_reconnect_timer);
    if (cret != ESP_OK)
    {
      ESP_LOGE(tag, "esp_timer_create(wifi_rc) failed: %s — WiFi will not retry", esp_err_to_name(cret));
      s_reconnect_timer = NULL;
      return false;
    }
  }

  // start_once() on an already-armed timer returns ESP_ERR_INVALID_STATE and does nothing.
  esp_timer_stop(s_reconnect_timer);

  const esp_err_t ret = esp_timer_start_once(s_reconnect_timer, (uint64_t) delay_s * 1000000ULL);
  if (ret != ESP_OK)
  {
    ESP_LOGE(tag, "Failed to arm reconnect timer: %s", esp_err_to_name(ret));
    return false;
  }
  ESP_LOGI(tag, "WiFi offline — retry in %lus", (unsigned long) delay_s);
  return true;
}

static void schedule_reconnect(void)
{
  // Advance the backoff only when something was actually armed. Otherwise a burst of failure
  // events inflates the delay while leaving no retry pending at all.
  const bool armed = schedule_reconnect_in(s_reconnect_backoff_s);
  s_reconnect_backoff_s = backoff_next_s(s_reconnect_backoff_s, armed);
}

static void cancel_reconnect(void)
{
  if (s_reconnect_timer)
  {
    esp_timer_stop(s_reconnect_timer);
  }
  s_reconnect_backoff_s = BACKOFF_START_S;
}

// ============================================================
// Tailscale status poll — fires every 10s to update status bar icon
// ============================================================

// NOLINTNEXTLINE(readability-non-const-parameter)
static void ts_poll_cb(void *arg)
{
  (void) arg;
  if (!s_ml)
  {
    return;
  }
  ts_status_t ts;
  switch (microlink_get_state(s_ml))
  {
  case ML_STATE_CONNECTED:
    ts = TS_STATUS_CONNECTED;
    break;
  case ML_STATE_ERROR:
    ts = TS_STATUS_ERROR;
    break;
  case ML_STATE_IDLE:
    ts = TS_STATUS_IDLE;
    break;
  default:
    ts = TS_STATUS_CONNECTING;
    break;
  }
  // Bounded wait. lvgl_port_lock(0) means portMAX_DELAY, not try-lock, and this callback
  // runs on the shared esp_timer task — blocking here would also stall inactivity_cb
  // (deep sleep) and reconnect_timer_cb (WiFi retry). Status is re-polled in 10s, so
  // dropping a tick costs nothing. On failure the mutex is NOT held: do not unlock.
  if (!lvgl_port_lock(TS_POLL_LOCK_TIMEOUT_MS))
  {
    ESP_LOGD(tag, "LVGL busy — skipping Tailscale status tick");
    return;
  }
  status_bar_set_ts_status(ts);
  lvgl_port_unlock();
}

// ============================================================
// Battery policy — status_bar owns the ADC poll, this owns what to do with it
// ============================================================

// Ceiling applied while the battery is low. Not written to NVS: the stored brightness still
// holds what the user actually chose, and that is what gets restored once the battery recovers
// or USB is plugged back in.
#define BATT_LOW_BRIGHTNESS 30

// Edge detection lives in components/battery_view/ so the transition rule is host-testable; this
// is just the caller's record of the previous reading. Starts false, so a device booting on an
// already-low battery clamps on its first tick.
static bool s_low_batt_active = false;

// clang-tidy scores this at 56 purely from the ESP_LOG* macro expansions; the function itself is
// a three-case switch.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void on_battery_reading(const battery_view_t view)
{
  switch (battery_clamp_step(&s_low_batt_active, view))
  {
  case BATTERY_CLAMP_ON:
  {
    const uint8_t current = bsp_display_get_brightness();
    if (current > BATT_LOW_BRIGHTNESS)
    {
      ESP_LOGW(tag, "Battery low (%s) — clamping brightness to %d%%", view.text, BATT_LOW_BRIGHTNESS);
      bsp_display_set_brightness(BATT_LOW_BRIGHTNESS, 500);
    }
    break;
  }
  case BATTERY_CLAMP_OFF:
    ESP_LOGI(tag, "Battery recovered — restoring brightness");
    bsp_display_set_brightness((uint8_t) settings_get(SETTINGS_KEY_BRIGHTNESS), 500);
    break;
  case BATTERY_CLAMP_NONE:
    break; // no default: let -Wswitch flag a new action instead of silently ignoring it
  }
}

// ============================================================
// Button worker — heavy nav actions run here, off the button task
// ============================================================

// do_reset_wifi() alone can hold the CPU for seconds: wifi_manager_stop() polls up to
// STOP_TIMEOUT_MS, plus NVS erase, plus bringing up BLE (esp_srp_gen_salt_verifier() on a
// 3072-bit MPI). Running that inline on the button task stalled bsp_buttons.c's held_ms counter
// — which is just BTN_POLL_MS accumulated in a loop, so a stalled task stops counting — skewing
// every long-press/emergency threshold measured afterwards, dropped presses queued during the
// stall (the drain loop discards them), and made the two-button sleep combo unreachable while a
// reset was in flight. Everything that can block now goes through this queue instead.
#define BTN_WORKER_STACK     6144
#define BTN_WORKER_PRIORITY  2 // below BTN_TASK_PRIORITY (3, bsp_buttons.c) so button polling wins
#define BTN_WORKER_QUEUE_LEN 4

static QueueHandle_t s_btn_worker_queue = NULL;
static TaskHandle_t s_btn_worker_task = NULL;

static void btn_worker_task(void *arg) // NOLINT(readability-non-const-parameter)
{
  (void) arg;
  nav_action_cb_t cb;
  for (;;) // NOLINT
  {
    if (xQueueReceive(s_btn_worker_queue, (void *) &cb, portMAX_DELAY) == pdTRUE && cb)
    {
      cb();
    }
  }
}

static void btn_worker_init(void)
{
  s_btn_worker_queue = xQueueCreate(BTN_WORKER_QUEUE_LEN, sizeof(nav_action_cb_t));
  if (!s_btn_worker_queue)
  {
    ESP_LOGE(tag, "Failed to create button worker queue — actions will run inline on the button task");
    return;
  }
  if (xTaskCreate(btn_worker_task, "btn_worker", BTN_WORKER_STACK, NULL, BTN_WORKER_PRIORITY, &s_btn_worker_task) !=
      pdPASS)
  {
    ESP_LOGE(tag, "Failed to create button worker task — actions will run inline on the button task");
    vQueueDelete(s_btn_worker_queue);
    s_btn_worker_queue = NULL;
  }
}

// Queues a deferred nav action; falls back to running it inline if the worker never came up,
// which is the pre-4E behaviour and better than silently dropping the action.
static void post_to_worker(const nav_action_cb_t cb)
{
  if (!cb)
  {
    return;
  }
  if (!s_btn_worker_queue)
  {
    cb();
    return;
  }
  // Never block: a full queue means four heavy actions are already backed up, which does not
  // happen in practice. Blocking here would recreate the exact stall this queue exists to avoid.
  if (xQueueSend(s_btn_worker_queue, (const void *) &cb, 0) != pdTRUE)
  {
    ESP_LOGW(tag, "Button worker queue full — dropping action");
  }
}

// ============================================================
// Wi-Fi reset action (shared by emergency button + nav settings)
// ============================================================

// Single entry point for bringing provisioning up, so the failure handling cannot drift between
// the button path and the NO_CRED path — it already had, and NO_CRED was the one left stranded.
static void start_provisioning_or_recover(void)
{
  const esp_err_t ret = ble_provisioning_start();
  if (ret == ESP_OK)
  {
    return;
  }

  if (ret == ESP_ERR_INVALID_STATE)
  {
    // Either the BLE controller memory was released for good, or the manager is already
    // initialized. Both need a restart to get back to a provisionable state, and neither is
    // specifically "memory released" — the old log here claimed that and misdiagnosed the rest.
    ESP_LOGW(tag, "Provisioning unavailable (%s) — rebooting into provisioning mode", esp_err_to_name(ret));
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
  }

  // Anything else: stay alive and keep trying WiFi rather than sitting offline with no QR.
  ESP_LOGE(tag, "ble_provisioning_start failed: %s — falling back to WiFi retry", esp_err_to_name(ret));
  schedule_reconnect();
}

static void do_reset_wifi(void)
{
  wifi_manager_stop();
  wifi_manager_clear_credential();
  start_provisioning_or_recover();
}

// Non-destructive counterpart to do_reset_wifi(): brings up (or returns to) provisioning without
// touching the stored credential, so backing out still leaves the device able to rejoin its AP.
static void do_provisioning(void)
{
  if (!ble_provisioning_is_active())
  {
    ESP_LOGI(tag, "Starting provisioning (credential kept)");
    wifi_manager_stop();
    start_provisioning_or_recover();
    return; // BLE_PROV_STARTED puts the QR up
  }

  char dev_name[32];
  char prov_pass[9];
  ble_provisioning_get_device_name(dev_name, sizeof(dev_name));
  ble_provisioning_get_password(prov_pass, sizeof(prov_pass));

  lvgl_port_lock(0);
  prov_screen_show(dev_name, prov_pass);
  lvgl_port_unlock();
}

// Dismissing the QR overlay on a device that already has a credential means "never mind" — and
// getting here cost the user their connection, because do_provisioning() stopped WiFi to hand the
// radio to network_prov_mgr. Per invariant 1 of wifi_manager_stop(), restarting it is ours to do.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void dismiss_provisioning(void)
{
  if (!wifi_manager_has_credential())
  {
    // Never provisioned: there is no connection to restore, and wifi_manager_start() would only
    // fire NO_CRED straight back. Leave BLE advertising so the phone can still finish the job —
    // the clock stays usable in the meantime, which is the whole point of a dismissible overlay.
    ESP_LOGI(tag, "QR dismissed with no stored credential — provisioning stays available");
    return;
  }

  // Only requests the stop; the radio is not free yet. WiFi is restarted from the
  // BLE_PROV_STOPPED handler, which runs once NETWORK_PROV_END confirms the service is down —
  // same shape as the BLE_PROV_SUCCESS path. Starting it here would race the teardown.
  ESP_LOGI(tag, "Provisioning dismissed — stopping BLE, WiFi resumes on PROV_END");
  const esp_err_t ret = ble_provisioning_stop();
  if (ret != ESP_OK)
  {
    ESP_LOGW(tag, "ble_provisioning_stop failed: %s", esp_err_to_name(ret));
  }
}

static void do_sleep_now(void)
{
  deep_sleep_trigger();
}

static void do_ntp_resync(void)
{
  sntp_sync_force_resync();
}

// ============================================================
// Nav callback registration
// ============================================================

// Sleeping means a full restart, which throws away the provisioning session the user is in the
// middle of — and they are holding a phone up to the QR code, not pressing buttons, so nothing
// resets the inactivity countdown.
//
// Keyed on the overlay being visible rather than on ble_provisioning_is_active(): a device that
// has never been provisioned keeps advertising indefinitely by design, so the latter would block
// auto-sleep forever and flatten the battery. Once the QR is dismissed the device is a clock
// again and may sleep; waking re-runs boot, hits NO_CRED and brings provisioning straight back.
static bool provisioning_in_progress(void)
{
  return prov_screen_is_visible();
}

void app_handlers_register_nav_callbacks(void)
{
  nav_register_reset_wifi_cb(do_reset_wifi);
  nav_register_sleep_cb(do_sleep_now);
  nav_register_ntp_resync_cb(do_ntp_resync);
  nav_register_provisioning_cb(do_provisioning);
  deep_sleep_register_inhibit_cb(provisioning_in_progress);
  status_bar_register_battery_cb(on_battery_reading);
  btn_worker_init();
}

// ============================================================
// Button handler
// ============================================================

// input_policy mirrors the BSP button vocabulary rather than including bsp.h, which pulls in
// esp_lvgl_port.h and would keep the policy off the host test bench. These assertions are what
// makes the duplication safe.
_Static_assert((int) INPUT_BTN_BOOT == BSP_BTN_BOOT, "input_policy button ids drifted from BSP");
_Static_assert((int) INPUT_BTN_IO14 == BSP_BTN_IO14, "input_policy button ids drifted from BSP");
_Static_assert((int) INPUT_EVENT_SHORT == BSP_BTN_SHORT, "input_policy events drifted from BSP");
_Static_assert((int) INPUT_EVENT_LONG == BSP_BTN_LONG, "input_policy events drifted from BSP");
_Static_assert((int) INPUT_EVENT_EMERGENCY == BSP_BTN_EMERGENCY, "input_policy events drifted from BSP");

void on_button_press(const int btn_id, const bsp_btn_event_t event)
{
  deep_sleep_reset_timer();
  // Calls off a sleep already fading out. Resetting the timer alone does not help there: the
  // request has left the timer behind and the sleep task is committed to it.
  deep_sleep_cancel();

  // Sampled before anything that can block. The sleep combo means "the other button is still
  // down as this one fires LONG" — reading the pad after an lvgl_port_lock(0) would let a
  // release during a screen repaint turn the combo into a stray SELECT.
  const gpio_num_t other = (btn_id == BSP_BTN_BOOT) ? GPIO_NUM_14 : GPIO_NUM_0;
  const int other_level = gpio_get_level(other); // active-low: 0 = pressed

  input_outcome_t outcome = input_policy_decide((input_btn_t) btn_id, (input_event_t) event, other_level);

  // The overlay guard only ever rewrites a NAV outcome, so the emergency hold and the sleep
  // combo resolve without touching LVGL at all — lvgl_port_lock(0) waits portMAX_DELAY, and
  // parking the button task behind a repaint also stalls bsp_buttons.c's held_ms, which is what
  // makes the combo unreachable in the first place. Applying it unconditionally would be
  // correct, just needlessly blocking.
  if (outcome.kind == INPUT_OUTCOME_NAV)
  {
    lvgl_port_lock(0);
    const bool prov_visible = prov_screen_is_visible();
    lvgl_port_unlock();
    outcome = input_policy_apply_overlay(outcome, prov_visible);
  }

  switch (outcome.kind)
  {
  case INPUT_OUTCOME_RESET_WIFI:
    ESP_LOGW(tag, "Emergency: resetting WiFi → BLE provisioning");
    post_to_worker(do_reset_wifi);
    break;

  case INPUT_OUTCOME_DEEP_SLEEP:
    ESP_LOGI(tag, "Both buttons held — triggering deep sleep");
    deep_sleep_trigger();
    break;

  case INPUT_OUTCOME_DISMISS_PROV:
    lvgl_port_lock(0);
    prov_screen_hide();
    lvgl_port_unlock();
    // Posted, not called: ble_provisioning_stop() tears down the manager and wifi_manager_start()
    // wakes the WiFi task — both block long enough to freeze the display and stall the event
    // loop, which is exactly what Phase 2 moved the nav action callbacks off the lock to avoid,
    // and what the button worker exists to keep off this task entirely.
    post_to_worker(dismiss_provisioning);
    break;

  case INPUT_OUTCOME_SWALLOW:
    break;

  case INPUT_OUTCOME_NAV:
  {
    lvgl_port_lock(0);
    nav_action_cb_t deferred = nav_handle_action(outcome.action);
    lvgl_port_unlock();

    // Posted after the lock is released, not called inline. nav_handle_action() touches no LVGL
    // after resolving this, so the lock is already clear either way — but running action items
    // (do_reset_wifi() above all) synchronously here still parked the button task for seconds,
    // which stalls held_ms accounting in bsp_buttons.c and makes the two-button sleep combo
    // unreachable mid-reset. The worker runs them instead.
    post_to_worker(deferred);
    break;
  }
  }
}

// ============================================================
// SNTP sync callback
// ============================================================

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void on_sntp_sync(const sntp_sync_event_t event)
{
  switch (event)
  {
  case SNTP_EVENT_SYNCING:
    ESP_LOGI(tag, "NTP syncing...");
    lvgl_port_lock(0);
    status_bar_set_sntp_status(SNTP_STATUS_SYNCING);
    lvgl_port_unlock();
    break;

  case SNTP_EVENT_SYNCED:
    ESP_LOGI(tag, "NTP time synchronized!");
    lvgl_port_lock(0);
    status_bar_set_sntp_status(SNTP_STATUS_SYNCED);
    // Reaching an NTP server proves the internet works, so this doubles as the recovery signal
    // for a connection that failed its DNS probe earlier — the captive portal has been signed
    // into, or the upstream came back. Cheaper and safer than re-probing from the WiFi task,
    // which would mean an unbounded getaddrinfo() inside a state wifi_manager_stop() must be
    // able to interrupt.
    if (s_wifi_unverified)
    {
      ESP_LOGI(tag, "Internet confirmed by NTP — clearing no-internet state");
      s_wifi_unverified = false;
      status_bar_set_wifi_status(WIFI_STATUS_CONNECTED);
    }
    lvgl_port_unlock();
    break;

  case SNTP_EVENT_FAILED:
    ESP_LOGW(tag, "NTP sync failed — clock may show wrong time");
    lvgl_port_lock(0);
    status_bar_set_sntp_status(SNTP_STATUS_FAILED);
    lvgl_port_unlock();
    break;
  }
}

// ============================================================
// BLE provisioning callback
// ============================================================

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void on_ble_prov_event(const ble_prov_event_t event, const char *ssid, const char *pass)
{
  switch (event)
  {
  case BLE_PROV_STARTED:
  {
    char dev_name[32];
    char prov_pass[9];
    ble_provisioning_get_device_name(dev_name, sizeof(dev_name));
    ble_provisioning_get_password(prov_pass, sizeof(prov_pass));
    ESP_LOGI(tag, "BLE provisioning active: %s", dev_name);
    lvgl_port_lock(0);
    status_bar_set_wifi_status(WIFI_STATUS_PROVISIONING);
    prov_screen_show(dev_name, prov_pass);
    lvgl_port_unlock();
    break;
  }

  case BLE_PROV_CRED_RECEIVED:
    ESP_LOGI(tag, "BLE credentials received: SSID=\"%s\"", ssid ? ssid : "");
    wifi_manager_set_credential(ssid, pass);
    break;

  case BLE_PROV_SUCCESS:
  {
    ESP_LOGI(tag, "BLE provisioning complete — starting WiFi");
    lvgl_port_lock(0);
    prov_screen_hide();
    lvgl_port_unlock();
    ble_provisioning_stop();
    // Only reached on a genuine completion — this frees ~110 KB of BT RAM for good.
    ble_provisioning_release_memory();
    const esp_err_t ret = wifi_manager_start();
    if (ret != ESP_OK)
    {
      ESP_LOGW(tag, "wifi_manager_start after provisioning: %s", esp_err_to_name(ret));
    }
    break;
  }

  case BLE_PROV_STOPPED:
  {
    // Cancelled, not completed. Deliberately no ble_provisioning_release_memory() here — that
    // frees the BT controller for good, and the user has to be able to come back via
    // Settings → Network → Provisioning.
    ESP_LOGI(tag, "BLE provisioning stopped (cancelled) — resuming WiFi");
    lvgl_port_lock(0);
    prov_screen_hide();
    lvgl_port_unlock();
    // Safe now, and only now: the manager has released the radio. do_provisioning() stopped WiFi
    // on the way in, and per invariant 1 nothing but this call ever brings it back.
    const esp_err_t ret = wifi_manager_start();
    if (ret != ESP_OK)
    {
      ESP_LOGW(tag, "wifi_manager_start after cancel: %s", esp_err_to_name(ret));
    }
    break;
  }

  case BLE_PROV_FAILED:
    ESP_LOGW(tag, "BLE provisioning failed — bad credentials, waiting for retry");
    wifi_manager_clear_credential();
    // The manager keeps advertising so the phone can retry over the same BLE link. Put the QR
    // back up: without this the only feedback was a log line, and if the user had dismissed the
    // overlay there was nothing on screen to say a retry was expected.
    do_provisioning();
    break;

  default:
    break;
  }
}

// ============================================================
// WiFi event callback
// ============================================================

// MicroLink runs if a key was configured at build time, or if a previous session left credentials
// in NVS. Written as one predicate rather than inline: CONFIG_ML_TAILSCALE_AUTH_KEY is empty in
// the checked-in sdkconfig, so an inline `KEY[0] == '\0' && ...` folds to a constant and reads as
// a redundant operand — yet it is the decisive one in any build that sets a key via menuconfig,
// where it must short-circuit so MicroLink starts.
static bool microlink_configured(void)
{
  const char *const auth_key = CONFIG_ML_TAILSCALE_AUTH_KEY;
  return auth_key[0] != '\0' || microlink_has_stored_credentials();
}

// Bounded wait — see ts_poll_cb() above for the same reasoning: on_wifi_event() runs
// synchronously on the wifi task from inside fire_event(), including calls made deep inside
// try_connect_candidate(); an unbounded lvgl_port_lock(0) there could stall the wifi task for as
// long as the LVGL task is busy, invisible to wifi_manager_stop()'s STOP_TIMEOUT_MS polling.
//
// Only survives for device_info_screen_set_ml(), where a skipped update is genuinely free: the
// System Info screen re-reads the same handle on its own 10s timer. It must never be used for
// anything a later event will not repeat — the WiFi status icon used to be exactly that, and a
// timeout on entering a terminal state (CONNECTED / NO_INTERNET) left the icon stale for as long
// as the connection lasted, because there is no next event. That icon is published now.
//
// On failure the mutex is NOT held: callers must not call lvgl_port_unlock() when this returns
// false.
static bool wifi_event_lvgl_lock(void)
{
  if (!lvgl_port_lock(WIFI_EVENT_LOCK_TIMEOUT_MS))
  {
    ESP_LOGD(tag, "LVGL busy — skipping a WiFi-task paint");
    return false;
  }
  return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void on_wifi_event(const wifi_manager_event_t event)
{
  switch (event)
  {
  case WIFI_MGR_SCANNING:
    status_bar_set_wifi_status(WIFI_STATUS_SCANNING);
    break;

  case WIFI_MGR_CONNECTING:
    status_bar_set_wifi_status(WIFI_STATUS_CONNECTING);
    break;

  case WIFI_MGR_GOT_IP:
    ESP_LOGI(tag, "WiFi got IP — verifying internet...");
    status_bar_set_wifi_status(WIFI_STATUS_VERIFYING);
    break;

  case WIFI_MGR_CONNECTED:
    cancel_reconnect();
    // Cleared before publishing: a later connection to a working network must not inherit the
    // flag from an earlier one. WIFI_MGR_NO_INTERNET, if it comes, arrives immediately after
    // this and publishes over the green.
    s_wifi_unverified = false;
    status_bar_set_wifi_status(WIFI_STATUS_CONNECTED);
    if (!s_sntp_started)
    {
      ESP_LOGI(tag, "WiFi verified online — starting NTP sync...");
      sntp_sync_start(on_sntp_sync);
      s_sntp_started = true;
    }
    else
    {
      ESP_LOGI(tag, "WiFi reconnected — notifying SNTP");
      sntp_sync_notify_connected();
    }
    if (!microlink_configured())
    {
      ESP_LOGI(tag, "No Tailscale auth key and no stored session — skipping MicroLink");
    }
    else if (s_ml == NULL)
    {
      const char *dev_name =
          (CONFIG_ML_DEVICE_NAME[0] != '\0') ? CONFIG_ML_DEVICE_NAME : microlink_default_device_name();
      microlink_config_t ml_cfg = {
          .auth_key = CONFIG_ML_TAILSCALE_AUTH_KEY,
          .device_name = dev_name,
          .enable_derp = true,
          .enable_stun = true,
          .enable_disco = true,
          .max_peers = CONFIG_ML_MAX_PEERS,
      };
      s_ml = microlink_init(&ml_cfg);
      if (s_ml)
      {
        microlink_start(s_ml);
        // device_info_screen_set_ml() writes a static handle and then updates labels via
        // lv_label_set_text(). This runs on the wifi_mgr task, so it has to be serialized
        // against the LVGL task — including the screen's own 10s timer, which calls the
        // very same update_tailscale(). Bounded: if the lock isn't free the screen's own
        // 10s timer will pick up the same handle on its next tick.
        if (wifi_event_lvgl_lock())
        {
          device_info_screen_set_ml(s_ml);
          lvgl_port_unlock();
        }
        if (!s_ts_poll_timer)
        {
          const esp_timer_create_args_t ts_args = {.callback = ts_poll_cb, .name = "ts_poll"};
          const esp_err_t tret = esp_timer_create(&ts_args, &s_ts_poll_timer);
          if (tret != ESP_OK)
          {
            ESP_LOGE(tag, "esp_timer_create(ts_poll) failed: %s — Tailscale icon will not update",
                     esp_err_to_name(tret));
            s_ts_poll_timer = NULL;
          }
        }
        if (s_ts_poll_timer)
        {
          esp_timer_start_periodic(s_ts_poll_timer, TS_POLL_PERIOD_US);
        }
      }
      else
      {
        ESP_LOGE(tag, "microlink_init failed");
      }
    }
    else
    {
      // Kept outside the lock — microlink_rebind() vTaskDelays ~300ms reopening sockets.
      microlink_rebind(s_ml);
      if (wifi_event_lvgl_lock())
      {
        device_info_screen_set_ml(s_ml);
        lvgl_port_unlock();
      }
    }
    break;

  case WIFI_MGR_NO_INTERNET:
    // Arrives right after CONNECTED, deliberately, so it paints over the green the handler above
    // just set. The device stays fully operational on the LAN; only the icon disagrees with
    // "online", which is the honest reading when NTP is about to fail.
    ESP_LOGW(tag, "Associated but no internet — clock time may be wrong");
    s_wifi_unverified = true;
    status_bar_set_wifi_status(WIFI_STATUS_NO_INTERNET);
    break;

  case WIFI_MGR_SCAN_DONE:
    ESP_LOGI(tag, "WiFi scan complete");
    break;

  case WIFI_MGR_NO_CRED:
    ESP_LOGW(tag, "No WiFi credential stored — starting BLE provisioning");
    // Invariant 3: this runs on the wifi task, so the stop is only requested, not awaited.
    wifi_manager_stop();
    status_bar_set_wifi_status(WIFI_STATUS_PROVISIONING);
    start_provisioning_or_recover();
    break;

  case WIFI_MGR_DISCONNECTED:
  case WIFI_MGR_NO_MATCH:
  case WIFI_MGR_ALL_FAILED:
    ESP_LOGW(tag, "WiFi unavailable (event=%d) — will retry with backoff", (int) event);
    s_wifi_unverified = false; // no connection at all now; the yellow state no longer applies
    status_bar_set_wifi_status(WIFI_STATUS_DISCONNECTED);
    schedule_reconnect();
    break;
  }
}
