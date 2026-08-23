// SPDX-License-Identifier: MIT
// ZenClock — BLE Provisioning via espressif/network_provisioning ^1.2.4
//
// Security 2 (SRP6a), device name "PROV_ZenClock_XXYY".
// Password derived from last 4 MAC bytes (8 hex chars), shown on provisioning screen.
// Username is fixed: "wifiprov" (Espressif BLE Prov app hardcodes this). Salt+verifier generated at start, freed on
// PROV_END.
//
// ⚠️  IMPORTANT: Always call wifi_manager_stop() before ble_provisioning_start()
//     to avoid conflict with network_prov_mgr's internal esp_wifi_connect() calls.
//
// ⚠️  IMPORTANT: Verify NETWORK_PROV_WIFI_CRED_RECV event data type against the
//     installed network_provisioning/manager.h header before building.

#include "ble_provisioning.h"
#include "prov_session.h"

#include <string.h>
#include <esp_log.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// network_provisioning ^1.2.4 — IDF v6 replacement for wifi_provisioning
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

// SRP6a salt+verifier generation
#include "esp_srp.h"

static const char *const tag = "BLEProv";

#define SEC2_USERNAME     "wifiprov"
#define SEC2_USERNAME_LEN 8
#define SEC2_SALT_LEN     16

static ble_prov_cb_t s_callback = NULL;

// The session lifecycle — phase plus latched outcome — lives in prov_session.c, which is pure and
// host-tested (test/test_pure_logic/). It replaced four booleans (s_active, s_stopping, s_cred_ok,
// s_mem_freed) whose combinations were the highest-stakes untested algebra in the firmware: the
// outcome it computes is what permits ble_provisioning_release_memory()'s one-way 110 KB free.
//
// Guarded by a spinlock rather than made atomic, because it is a struct: the manager's events
// arrive on the default event loop while do_provisioning() reads the phase from btn_worker. The
// booleans this replaced were shared across those same two tasks with no synchronization at all.
static portMUX_TYPE s_session_mux = portMUX_INITIALIZER_UNLOCKED;
static prov_session_t s_session;

// Deliberately NOT part of the session phase: this tracks whether network_prov_mgr_deinit() is
// owed, which cycles within a single session rather than being a phase of one. NETWORK_PROV_END
// clears the phase before invoking the app callback, so gating teardown on the phase would mean
// ble_provisioning_stop() returned early and deinit() never ran at all.
static bool s_initialized = false;

// The action enum mirrors ble_prov_event_t with PROV_ACT_NONE occupying 0, so each emitting action
// sits one past its event. session_emit() does not rely on that arithmetic — it maps explicitly —
// so these assert the intended 1:1 correspondence rather than guarding a computation: adding an
// event to one enum without the other fails the build here instead of in review.
_Static_assert((int) PROV_ACT_EMIT_STARTED == (int) BLE_PROV_STARTED + 1, "prov_session actions drifted");
_Static_assert((int) PROV_ACT_EMIT_CRED_RECEIVED == (int) BLE_PROV_CRED_RECEIVED + 1, "prov_session actions drifted");
_Static_assert((int) PROV_ACT_EMIT_SUCCESS == (int) BLE_PROV_SUCCESS + 1, "prov_session actions drifted");
_Static_assert((int) PROV_ACT_EMIT_FAILED == (int) BLE_PROV_FAILED + 1, "prov_session actions drifted");
_Static_assert((int) PROV_ACT_EMIT_STOPPED == (int) BLE_PROV_STOPPED + 1, "prov_session actions drifted");

// Advance the session and hand back what to do about it. The callback is deliberately NOT invoked
// in here: it runs app code (which stops WiFi, repaints, frees the BT controller) and must never
// run inside the spinlock.
static prov_action_t session_feed(const prov_input_t input)
{
  portENTER_CRITICAL(&s_session_mux);
  const prov_step_t r = prov_session_step(s_session, input);
  s_session = r.next;
  portEXIT_CRITICAL(&s_session_mux);
  return r.action;
}

static prov_phase_t session_phase(void)
{
  portENTER_CRITICAL(&s_session_mux);
  const prov_phase_t phase = s_session.phase;
  portEXIT_CRITICAL(&s_session_mux);
  return phase;
}

// ssid/pass are only meaningful for PROV_ACT_EMIT_CRED_RECEIVED; every other action passes NULL,
// matching the contract on ble_prov_cb_t.
static void session_emit(const prov_action_t action, const char *ssid, const char *pass)
{
  if (!s_callback)
  {
    return;
  }

  switch (action)
  {
  case PROV_ACT_EMIT_STARTED:
    s_callback(BLE_PROV_STARTED, NULL, NULL);
    break;
  case PROV_ACT_EMIT_CRED_RECEIVED:
    s_callback(BLE_PROV_CRED_RECEIVED, ssid, pass);
    break;
  case PROV_ACT_EMIT_SUCCESS:
    s_callback(BLE_PROV_SUCCESS, NULL, NULL);
    break;
  case PROV_ACT_EMIT_FAILED:
    s_callback(BLE_PROV_FAILED, NULL, NULL);
    break;
  case PROV_ACT_EMIT_STOPPED:
    s_callback(BLE_PROV_STOPPED, NULL, NULL);
    break;
  case PROV_ACT_SUPPRESS:
  case PROV_ACT_NONE:
    break;
  }
}

// SRP6a credentials — heap-alloc'd in ble_provisioning_start(), freed on PROV_END
static char s_sec2_password[9]; // 8 hex chars from MAC + NUL
static char *s_sec2_salt = NULL;
static char *s_sec2_verifier = NULL;
static int s_sec2_verifier_len = 0;

// ============================================================
// Device name + password builders
// ============================================================

static void build_device_name(char *buf, size_t len)
{
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(buf, len, "PROV_ZenClock_%02X%02X", mac[4], mac[5]);
}

static void build_sec2_password(void)
{
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_sec2_password, sizeof(s_sec2_password), "%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);
}

static void free_sec2_credentials(void)
{
  free(s_sec2_salt);
  free(s_sec2_verifier);
  s_sec2_salt = NULL;
  s_sec2_verifier = NULL;
  s_sec2_verifier_len = 0;
}

// ============================================================
// Teardown heap probe (#73)
// ============================================================

// A cancelled provisioning cycle is repeatable, and every one of them re-runs
// network_prov_mgr_init()/deinit() plus a freshly heap-allocated SRP salt+verifier. That the pair
// balances rested on code review and the upstream example only; these probes make it measurable.
//
// Read them ACROSS cycles, never as one cycle's difference. "begin" is taken before this cycle
// has allocated anything, "end" at the tail of the NETWORK_PROV_END teardown; the ~19 KB between
// them is not BLE at all but microlink losing its DERP and coordination sockets, which the WiFi
// stop preceding this call tears down asynchronously — after "begin" has already been sampled.
// The signal is each probe compared with ITSELF from one cycle to the next: both are taken at a
// fixed point of an identical cycle, and "begin" in particular is the only sample where BLE holds
// nothing, so begin(N+1) - begin(N) is a whole cycle's net accumulation. Measured over 6 cancel
// cycles (#73): begin drifts -12 B/cycle and not monotonically, end -30 B/cycle in a sequence
// decaying to zero, largest_free_block identical in all 12 samples. No leak, no fragmentation.
//
// INTERNAL|8BIT is the pool the BT controller and protocomm draw from; the total free size alone
// would include PSRAM and answer nothing. largest_free_block is what separates a leak (free
// falls, largest holds) from fragmentation (free holds, largest falls).
static void log_heap(const char *const phase)
{
  ESP_LOGI(tag, "heapchk %s: free=%u largest=%u", phase,
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// ============================================================
// network_provisioning event handler
// ============================================================

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void prov_event_handler(void *arg, // NOLINT(readability-non-const-parameter)
                               const esp_event_base_t base,
                               const int32_t id,
                               void *data)
{
  (void) arg;
  if (base != NETWORK_PROV_EVENT)
  {
    return;
  }

  switch (id)
  {
  case NETWORK_PROV_START:
    ESP_LOGI(tag, "BLE advertisement started");
    // Emits at most once per session: whichever of this handler and ble_provisioning_start()'s
    // tail observes the STARTING -> ADVERTISING edge first reports it, and the other becomes a
    // no-op. They run on different tasks and either order is legal.
    session_emit(session_feed(PROV_IN_START_OK), NULL, NULL);
    break;

  case NETWORK_PROV_WIFI_CRED_RECV:
  {
    if (!data)
    {
      break;
    }
    const auto cfg = (wifi_sta_config_t *) data;
    // cfg->ssid is uint8_t[32] with no room for a NUL — esp_wifi reads it bounded at 32 bytes and
    // never requires termination (see network_provisioning's own handlers.c comment on this exact
    // struct). But casting it straight to const char* and running strlen/%s on it is NOT safe: an
    // exactly-32-char SSID has no terminator and ssid[32] sits immediately before password[64] in
    // wifi_sta_config_t, so strlen keeps reading into the password. That leaks the plaintext
    // password into this log line and into whatever ssid is stored under, and later corrupts the
    // saved credential. Copy bounded with strnlen instead, matching manager.c's own read-back.
    char ssid[sizeof(cfg->ssid) + 1];
    char pass[sizeof(cfg->password) + 1];
    const size_t ssid_len = strnlen((const char *) cfg->ssid, sizeof(cfg->ssid));
    memcpy(ssid, cfg->ssid, ssid_len);
    ssid[ssid_len] = '\0';
    const size_t pass_len = strnlen((const char *) cfg->password, sizeof(cfg->password));
    memcpy(pass, cfg->password, pass_len);
    pass[pass_len] = '\0';
    ESP_LOGI(tag, "Credentials received: SSID=\"%s\"", ssid);
    // Forwarded from every live phase on purpose — see prov_session.c. A credential that arrived
    // is stored even if a cancel is in flight, because the verification that may follow latches an
    // outcome regardless, and reporting a success whose credential was never saved is the one
    // failure here that cannot be undone.
    session_emit(session_feed(PROV_IN_CRED_RECEIVED), ssid, pass);
    break;
  }

  case NETWORK_PROV_WIFI_CRED_SUCCESS:
    ESP_LOGI(tag, "Credential verification succeeded");
    // Latches the outcome; NETWORK_PROV_END is what reports it. END carries no outcome of its own
    // — it only says the service stopped — so this latch is the sole thing separating "the phone
    // provisioned us" from "the user backed out", and only the former may release BT memory.
    session_feed(PROV_IN_CRED_VERIFIED);
    break;

  case NETWORK_PROV_WIFI_CRED_FAIL:
  {
    const auto reason = (network_prov_wifi_sta_fail_reason_t *) data;
    const prov_action_t action = session_feed(PROV_IN_CRED_FAILED);
    if (action == PROV_ACT_SUPPRESS)
    {
      // Raised while we are tearing the session down on purpose. Forwarding it would have the app
      // erase a perfectly good stored credential and put the QR back up.
      ESP_LOGI(tag, "Ignoring credential failure during deliberate stop (reason=%d)", reason ? (int) *reason : -1);
      break;
    }
    ESP_LOGW(tag, "Credential verification failed (reason=%d)", reason ? (int) *reason : -1);
    session_emit(action, NULL, NULL);
    break;
  }

  case NETWORK_PROV_END:
  {
    ESP_LOGI(tag, "Provisioning ended");
    // Resolves the outcome and returns the session to IDLE. Computed before teardown, reported
    // after it: the callback releases the BT controller and must not run while the manager still
    // holds protocomm and the BLE scheme.
    const prov_action_t action = session_feed(PROV_IN_END);

    // Deinit here, matching the upstream examples (network_provisioning
    // examples/wifi_prov/main/app_main.c:150). It has to happen before the app callback:
    // that callback releases the BT controller, which must not be torn down while the
    // provisioning manager still holds protocomm and the BLE scheme.
    if (s_initialized)
    {
      const esp_err_t err = network_prov_mgr_deinit();
      if (err != ESP_OK)
      {
        ESP_LOGE(tag, "network_prov_mgr_deinit failed: %s", esp_err_to_name(err));
      }
      s_initialized = false;
    }

    // Safe only now: protocomm shallow-copies sec2_params, so the salt/verifier buffers had
    // to stay alive until the manager was torn down.
    free_sec2_credentials();

    // Paired with the "begin" probe at the top of ble_provisioning_start() — see the note there
    // for how to read them. Must stay above the session_emit() below: on a cancel that callback
    // restarts WiFi, and a sample taken after it would be measuring the network stack coming back
    // up rather than the teardown that just finished.
    log_heap("end");

    // END fires for a cancel exactly as it does for a success. Reporting both as SUCCESS sent the
    // app down a path that calls ble_provisioning_release_memory(), freeing ~110 KB of BT RAM for
    // good — a cancelled session would have left the device unprovisionable. prov_session_step()
    // owns that distinction now, and a host test asserts no input sequence can reach SUCCESS
    // without a verified credential behind it.
    session_emit(action, NULL, NULL);
    break;
  }

  default:
    break;
  }
}

// ============================================================
// Public API
// ============================================================

esp_err_t ble_provisioning_init(ble_prov_cb_t callback)
{
  s_callback = callback;

  esp_err_t ret = esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event_handler, NULL);

  if (ret != ESP_OK)
  {
    ESP_LOGE(tag, "Failed to register prov event handler: %s", esp_err_to_name(ret));
  }
  return ret;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
esp_err_t ble_provisioning_start(void)
{
  if (session_phase() == PROV_PHASE_UNAVAILABLE)
  {
    ESP_LOGE(tag, "BLE memory already released — cannot start provisioning again");
    return ESP_ERR_INVALID_STATE;
  }

  // Before network_prov_mgr_init() and before the SRP salt+verifier: the baseline this cycle is
  // measured against. Paired with the "end" probe in the NETWORK_PROV_END handler.
  log_heap("begin");

  char device_name[32];
  build_device_name(device_name, sizeof(device_name));
  build_sec2_password();
  ESP_LOGI(tag, "Starting BLE provisioning: device_name=\"%s\"", device_name);

  network_prov_mgr_config_t config = {
      .scheme = network_prov_scheme_ble,
      .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
  };

  esp_err_t ret = network_prov_mgr_init(config);
  if (ret != ESP_OK)
  {
    ESP_LOGE(tag, "network_prov_mgr_init failed: %s", esp_err_to_name(ret));
    return ret;
  }
  s_initialized = true;
  // Enters STARTING and clears any previous outcome: a past session's success must not make this
  // one's END look like a success too, and a past cancel must not silence this one's failures.
  session_feed(PROV_IN_START_BEGUN);

  // Generate SRP6a salt+verifier from MAC-derived password.
  // Buffers are heap-alloc'd — freed in NETWORK_PROV_END handler (or on error below).
  ret = esp_srp_gen_salt_verifier(SEC2_USERNAME, SEC2_USERNAME_LEN, s_sec2_password, (int) strlen(s_sec2_password),
                                  &s_sec2_salt, SEC2_SALT_LEN, &s_sec2_verifier, &s_sec2_verifier_len);
  if (ret != ESP_OK)
  {
    ESP_LOGE(tag, "esp_srp_gen_salt_verifier failed: %s", esp_err_to_name(ret));
    // Before the deinit, not after: deinit() emits NETWORK_PROV_END itself, on the default event
    // loop. Resolving the session first means that END lands on an already-IDLE session and emits
    // nothing, instead of racing us and reporting a spurious BLE_PROV_STOPPED for a failure the
    // caller is about to handle itself.
    session_feed(PROV_IN_START_FAILED);
    network_prov_mgr_deinit();
    s_initialized = false;
    // Both buffers are out-params: a partial failure can leave one of them allocated, and the
    // next successful start() would overwrite the pointer and leak it.
    free_sec2_credentials();
    return ret;
  }

  // sec2_params is shallow-copied by protocomm; salt/verifier buffers must outlive PROV_END.
  network_prov_security2_params_t sec2_params = {
      .salt = s_sec2_salt,
      .salt_len = SEC2_SALT_LEN,
      .verifier = s_sec2_verifier,
      .verifier_len = (uint16_t) s_sec2_verifier_len,
  };

  ret = network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_2, (const void *) &sec2_params, device_name, NULL);
  if (ret != ESP_OK)
  {
    ESP_LOGE(tag, "network_prov_mgr_start_provisioning failed: %s", esp_err_to_name(ret));
    session_feed(PROV_IN_START_FAILED); // before the deinit that emits END — see above
    network_prov_mgr_deinit();
    s_initialized = false;
    free_sec2_credentials();
    return ret;
  }

  // Second observer of the same edge, alongside the NETWORK_PROV_START handler. Advancing here
  // rather than waiting for the event means do_provisioning() cannot see STARTING on a manager
  // that is already up and call network_prov_mgr_init() a second time; the step function makes it
  // monotonic, so whichever arrives second is a no-op and cannot revive a finished session.
  session_emit(session_feed(PROV_IN_START_OK), NULL, NULL);
  return ESP_OK;
}

esp_err_t ble_provisioning_stop(void) // NOLINT
{
  // Gate on s_initialized as well as the phase: on the success path NETWORK_PROV_END has already
  // returned the session to IDLE (and done the teardown itself), while an explicit cancel can
  // arrive with the manager initialized but not yet advertising. Both cases must be handled.
  const prov_phase_t phase = session_phase();
  if (phase == PROV_PHASE_UNAVAILABLE || (!s_initialized && phase == PROV_PHASE_IDLE))
  {
    // UNAVAILABLE included explicitly: after ble_provisioning_release_memory() the BT controller
    // is deinitialized and its RAM handed back, so the deinit below would run against a stack that
    // no longer exists. A BACK press queued on btn_worker before the overlay came down reaches
    // dismiss_provisioning() after exactly that, so this is a real arrival order, not a defensive
    // flourish.
    return ESP_OK;
  }

  // A stop is already in flight. This must return before the teardown below: the service is still
  // winding down, and handing protocomm's shallow copy of the SRP salt/verifier back to the
  // allocator mid-flight is precisely what panicked the device (see the comment further down).
  // NETWORK_PROV_END owns the teardown and will arrive on its own.
  if (phase == PROV_PHASE_STOPPING)
  {
    ESP_LOGI(tag, "Stop already in flight — teardown completes on PROV_END");
    return ESP_OK;
  }

  if (phase == PROV_PHASE_ADVERTISING)
  {
    // Request only — do NOT tear down here. network_prov_mgr_stop_provisioning() "will initiate
    // a process to stop the service and return" (manager.h:429-434); NETWORK_PROV_END arrives
    // later, after a cleanup delay that defaults to 1000 ms. Calling deinit() and
    // free_sec2_credentials() straight after handed protocomm's shallow copy of the SRP
    // salt/verifier back to the allocator while the service was still winding down — the device
    // panicked about a second afterwards. The END handler owns the whole teardown.
    ESP_LOGI(tag, "Stopping BLE provisioning (teardown completes on PROV_END)");
    session_feed(PROV_IN_STOP_REQUESTED);
    network_prov_mgr_stop_provisioning();
    return ESP_OK;
  }

  // Initialized but never advertised — a cancel landing between network_prov_mgr_init() and the
  // service coming up. network_prov_mgr_stop_provisioning() is not usable here: there is no
  // running service for it to stop. deinit() stops and emits NETWORK_PROV_END itself
  // (manager.h:436-438), so clear the flag first and let the handler skip its own deinit.
  //
  // The phase still moves to STOPPING before deinit, so a CRED_FAIL raised by this teardown is
  // suppressed rather than erasing the stored credential, and the END it triggers resolves the
  // session exactly as a normal cancel does.
  session_feed(PROV_IN_STOP_REQUESTED);
  s_initialized = false;
  network_prov_mgr_deinit();
  free_sec2_credentials();
  ESP_LOGI(tag, "BLE provisioning deinitialized");
  return ESP_OK;
}

prov_phase_t ble_provisioning_session_phase(void)
{
  return session_phase();
}

void ble_provisioning_get_device_name(char *buf, size_t len)
{
  build_device_name(buf, len);
}

void ble_provisioning_get_password(char *buf, const size_t len)
{
  snprintf(buf, len, "%s", s_sec2_password);
}

void ble_provisioning_release_memory(void)
{
  if (session_phase() == PROV_PHASE_UNAVAILABLE)
  {
    return;
  }

  // Allow BLE stack to fully settle after deinit before releasing memory.
  // Releasing too early causes heap corruption.
  vTaskDelay(pdMS_TO_TICKS(200));

  ESP_LOGI(tag, "Releasing BLE controller memory (~110KB)...");
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
  esp_bt_mem_release(ESP_BT_MODE_BLE);
  // The firmware's one irreversible transition, and it goes through the same step function as
  // everything else so a host test can assert nothing ever leaves the phase it lands in.
  session_feed(PROV_IN_MEM_RELEASED);
  ESP_LOGI(tag, "BLE memory released. Free heap: %lu bytes", (unsigned long) esp_get_free_heap_size());
}
