#include "app_handlers.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "bsp.h"
#include "deep_sleep.h"
#include "wifi_manager.h"
#include "sntp_sync.h"
#include "status_bar.h"
#include "ble_provisioning.h"
#include "prov_screen.h"
#include "nav.h"
#include "microlink.h"
#include "device_info_screen.h"

static const char *const tag = "ZenClock";

// ============================================================
// WiFi reconnect — exponential backoff timer
// ============================================================

static esp_timer_handle_t s_reconnect_timer = NULL;
static esp_timer_handle_t s_ts_poll_timer = NULL;
static int s_reconnect_backoff_s = 30;

// How long ts_poll_cb waits for the LVGL lock before giving up on this tick.
#define TS_POLL_LOCK_TIMEOUT_MS 50
static bool s_sntp_started = false;

// Written once by the wifi_mgr task; read by the esp_timer task in ts_poll_cb(). No lock:
// the ts_poll timer is only armed *after* the assignment below, so a reader can never
// observe the pre-init value, and the rebind path does not reassign it. volatile keeps the
// compiler from caching the pointer across the two task contexts.
static microlink_t *volatile s_ml = NULL;

// NOLINTNEXTLINE(readability-non-const-parameter)
static void reconnect_timer_cb(void *arg)
{
  (void) arg;
  wifi_manager_start();
}

static void schedule_reconnect(void)
{
  if (!s_reconnect_timer)
  {
    const esp_timer_create_args_t args = {.callback = reconnect_timer_cb, .name = "wifi_rc"};
    esp_timer_create(&args, &s_reconnect_timer);
  }
  esp_timer_start_once(s_reconnect_timer, (uint64_t) s_reconnect_backoff_s * 1000000ULL);
  ESP_LOGI(tag, "WiFi offline — retry in %ds", s_reconnect_backoff_s);
  s_reconnect_backoff_s = (s_reconnect_backoff_s * 2 > 300) ? 300 : s_reconnect_backoff_s * 2;
}

static void cancel_reconnect(void)
{
  if (s_reconnect_timer)
  {
    esp_timer_stop(s_reconnect_timer);
  }
  s_reconnect_backoff_s = 30;
}

// ============================================================
// Tailscale status poll — fires every 10s to update status bar icon
// ============================================================

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
// Wi-Fi reset action (shared by emergency button + nav settings)
// ============================================================

static void do_reset_wifi(void)
{
  wifi_manager_stop();
  wifi_manager_clear_credential();
  const esp_err_t ret = ble_provisioning_start();
  if (ret == ESP_ERR_INVALID_STATE)
  {
    ESP_LOGW(tag, "BLE memory released — rebooting into provisioning mode");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
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

void app_handlers_register_nav_callbacks(void)
{
  nav_register_reset_wifi_cb(do_reset_wifi);
  nav_register_sleep_cb(do_sleep_now);
  nav_register_ntp_resync_cb(do_ntp_resync);
}

// ============================================================
// Button handler
// ============================================================

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void on_button_press(const int btn_id, const bsp_btn_event_t event)
{
  deep_sleep_reset_timer();

  // Emergency: IO14 held ≥ 3s → reset WiFi + BLE provisioning (bypasses nav)
  if (event == BSP_BTN_EMERGENCY && btn_id == BSP_BTN_IO14)
  {
    ESP_LOGW(tag, "Emergency: resetting WiFi → BLE provisioning");
    do_reset_wifi();
    return;
  }

  // Simultaneous hold: both buttons long-pressed → deep sleep
  if (event == BSP_BTN_LONG)
  {
    gpio_num_t other = (btn_id == BSP_BTN_BOOT) ? GPIO_NUM_14 : GPIO_NUM_0;
    if (gpio_get_level(other) == 0) // active-low: 0 = pressed
    {
      ESP_LOGI(tag, "Both buttons held — triggering deep sleep");
      deep_sleep_trigger();
      return;
    }
  }

  // Map button + event → nav action
  nav_action_t action;
  if (btn_id == BSP_BTN_BOOT)
  {
    action = (event == BSP_BTN_SHORT) ? NAV_ACTION_UP : NAV_ACTION_SELECT;
  }
  else
  {
    action = (event == BSP_BTN_SHORT) ? NAV_ACTION_DOWN : NAV_ACTION_BACK;
  }

  lvgl_port_lock(0);
  nav_action_cb_t deferred = nav_handle_action(action);
  lvgl_port_unlock();

  // Run action items only after the lock is released. do_reset_wifi() alone can hold the
  // CPU for seconds (wifi_manager_stop polls up to 6s, NVS erase, BLE bring-up + SRP), and
  // doing that under the lock froze the display and stalled the esp_event loop — which
  // BLE provisioning needs to deliver its own BLE_PROV_STARTED. nav_handle_action() touches
  // no LVGL after resolving this, and the emergency path above already runs it lock-free.
  if (deferred)
  {
    deferred();
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
    ESP_LOGI(tag, "BLE provisioning complete — starting WiFi");
    lvgl_port_lock(0);
    prov_screen_hide();
    lvgl_port_unlock();
    ble_provisioning_stop();
    ble_provisioning_release_memory();
    wifi_manager_start();
    break;

  case BLE_PROV_FAILED:
    ESP_LOGW(tag, "BLE provisioning failed — bad credentials, waiting for retry");
    wifi_manager_clear_credential();
    break;

  default:
    break;
  }
}

// ============================================================
// WiFi event callback
// ============================================================

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void on_wifi_event(const wifi_manager_event_t event)
{
  switch (event)
  {
  case WIFI_MGR_SCANNING:
    lvgl_port_lock(0);
    status_bar_set_wifi_status(WIFI_STATUS_SCANNING);
    lvgl_port_unlock();
    break;

  case WIFI_MGR_CONNECTING:
    lvgl_port_lock(0);
    status_bar_set_wifi_status(WIFI_STATUS_CONNECTING);
    lvgl_port_unlock();
    break;

  case WIFI_MGR_GOT_IP:
    ESP_LOGI(tag, "WiFi got IP — verifying internet...");
    lvgl_port_lock(0);
    status_bar_set_wifi_status(WIFI_STATUS_VERIFYING);
    lvgl_port_unlock();
    break;

  case WIFI_MGR_CONNECTED:
    cancel_reconnect();
    lvgl_port_lock(0);
    status_bar_set_wifi_status(WIFI_STATUS_CONNECTED);
    lvgl_port_unlock();
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
    if (CONFIG_ML_TAILSCALE_AUTH_KEY[0] == '\0' && !microlink_has_stored_credentials())
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
        // very same update_tailscale().
        lvgl_port_lock(0);
        device_info_screen_set_ml(s_ml);
        lvgl_port_unlock();
        if (!s_ts_poll_timer)
        {
          const esp_timer_create_args_t ts_args = {.callback = ts_poll_cb, .name = "ts_poll"};
          esp_timer_create(&ts_args, &s_ts_poll_timer);
        }
        esp_timer_start_periodic(s_ts_poll_timer, 10ULL * 1000000ULL);
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
      lvgl_port_lock(0);
      device_info_screen_set_ml(s_ml);
      lvgl_port_unlock();
    }
    break;

  case WIFI_MGR_SCAN_DONE:
    ESP_LOGI(tag, "WiFi scan complete");
    break;

  case WIFI_MGR_NO_CRED:
    ESP_LOGW(tag, "No WiFi credential stored — starting BLE provisioning");
    wifi_manager_stop();
    lvgl_port_lock(0);
    status_bar_set_wifi_status(WIFI_STATUS_PROVISIONING);
    lvgl_port_unlock();
    ble_provisioning_start();
    break;

  case WIFI_MGR_DISCONNECTED:
  case WIFI_MGR_NO_MATCH:
  case WIFI_MGR_ALL_FAILED:
    ESP_LOGW(tag, "WiFi unavailable (event=%d) — will retry with backoff", (int) event);
    lvgl_port_lock(0);
    status_bar_set_wifi_status(WIFI_STATUS_DISCONNECTED);
    lvgl_port_unlock();
    schedule_reconnect();
    break;
  }
}
