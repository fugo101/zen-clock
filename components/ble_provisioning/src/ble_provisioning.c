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
// ⚠️  IMPORTANT: Verify NETWORK_PROV_CRED_RECV event data type against the
//     installed network_provisioning/manager.h header before building.

#include "ble_provisioning.h"

#include <string.h>
#include <esp_log.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <esp_bt.h>
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

// Two distinct lifetimes, previously conflated into one flag:
//   s_active      — the provisioning service is advertising / running
//   s_initialized — network_prov_mgr_init() succeeded and deinit() is still owed
// NETWORK_PROV_END clears s_active before invoking the app callback, so gating teardown on
// s_active meant ble_provisioning_stop() returned early and deinit() never ran at all.
static bool s_active = false;
static bool s_initialized = false;
static bool s_mem_freed = false;

// Latched by NETWORK_PROV_WIFI_CRED_SUCCESS, read by NETWORK_PROV_END, cleared on every start.
// Tells a completed provisioning apart from a cancelled one — see the END case below.
static bool s_cred_ok = false;

// Set while a deliberate stop is winding down. The manager can emit WIFI_CRED_FAIL as it tears a
// session down, and the app answers BLE_PROV_FAILED by erasing the stored WiFi credential and
// re-showing the QR — so an unguarded cancel wiped the user's WiFi password. Same idea as
// wifi_manager's stop_requested(): a failure raised by our own stop is not a real failure.
static bool s_stopping = false;

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
    s_active = true;
    if (s_callback)
    {
      s_callback(BLE_PROV_STARTED, NULL, NULL);
    }
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
    if (s_callback)
    {
      s_callback(BLE_PROV_CRED_RECEIVED, ssid, pass);
    }
    break;
  }

  case NETWORK_PROV_WIFI_CRED_SUCCESS:
    ESP_LOGI(tag, "Credential verification succeeded");
    // Latched because NETWORK_PROV_END carries no outcome of its own — it only says the service
    // stopped. This flag is the sole thing separating "the phone provisioned us" from "the user
    // backed out", and only the former may release the BT controller memory.
    s_cred_ok = true;
    break;

  case NETWORK_PROV_WIFI_CRED_FAIL:
  {
    const auto reason = (network_prov_wifi_sta_fail_reason_t *) data;
    if (s_stopping)
    {
      // Raised while we are tearing the session down on purpose. Forwarding it would have the
      // app erase a perfectly good stored credential and put the QR back up.
      ESP_LOGI(tag, "Ignoring credential failure during deliberate stop (reason=%d)", reason ? (int) *reason : -1);
      break;
    }
    ESP_LOGW(tag, "Credential verification failed (reason=%d)", reason ? (int) *reason : -1);
    if (s_callback)
    {
      s_callback(BLE_PROV_FAILED, NULL, NULL);
    }
    break;
  }

  case NETWORK_PROV_END:
    ESP_LOGI(tag, "Provisioning ended");
    s_active = false;

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

    if (s_callback)
    {
      // END fires for a cancel exactly as it does for a success. Reporting both as SUCCESS sent
      // the app down a path that calls ble_provisioning_release_memory(), freeing ~110 KB of BT
      // RAM for good — a cancelled session would have left the device unprovisionable.
      s_callback(s_cred_ok ? BLE_PROV_SUCCESS : BLE_PROV_STOPPED, NULL, NULL);
    }
    s_cred_ok = false;
    s_stopping = false;
    break;

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
  if (s_mem_freed)
  {
    ESP_LOGE(tag, "BLE memory already released — cannot start provisioning again");
    return ESP_ERR_INVALID_STATE;
  }

  // Cleared per attempt: a previous session's success must not make this one's END look like a
  // success too, and a previous cancel must not silence this session's genuine failures.
  s_cred_ok = false;
  s_stopping = false;

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

  // Generate SRP6a salt+verifier from MAC-derived password.
  // Buffers are heap-alloc'd — freed in NETWORK_PROV_END handler (or on error below).
  ret = esp_srp_gen_salt_verifier(SEC2_USERNAME, SEC2_USERNAME_LEN, s_sec2_password, (int) strlen(s_sec2_password),
                                  &s_sec2_salt, SEC2_SALT_LEN, &s_sec2_verifier, &s_sec2_verifier_len);
  if (ret != ESP_OK)
  {
    ESP_LOGE(tag, "esp_srp_gen_salt_verifier failed: %s", esp_err_to_name(ret));
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
    network_prov_mgr_deinit();
    s_initialized = false;
    free_sec2_credentials();
    return ret;
  }

  s_active = true;
  return ESP_OK;
}

esp_err_t ble_provisioning_stop(void) // NOLINT
{
  // Gate on s_initialized, not s_active: on the success path NETWORK_PROV_END has already
  // cleared s_active (and done the teardown itself), while an explicit cancel can arrive with
  // the manager initialized but not yet advertising. Both cases must be handled.
  if (!s_initialized && !s_active)
  {
    return ESP_OK;
  }

  if (s_active)
  {
    // Request only — do NOT tear down here. network_prov_mgr_stop_provisioning() "will initiate
    // a process to stop the service and return" (manager.h:429-434); NETWORK_PROV_END arrives
    // later, after a cleanup delay that defaults to 1000 ms. Calling deinit() and
    // free_sec2_credentials() straight after handed protocomm's shallow copy of the SRP
    // salt/verifier back to the allocator while the service was still winding down — the device
    // panicked about a second afterwards. The END handler owns the whole teardown.
    ESP_LOGI(tag, "Stopping BLE provisioning (teardown completes on PROV_END)");
    s_stopping = true;
    network_prov_mgr_stop_provisioning();
    return ESP_OK;
  }

  // Initialized but never advertised — a start() that failed partway. deinit() stops the service
  // and emits NETWORK_PROV_END itself (manager.h:436-438), so clear the flag first and let the
  // handler skip its own deinit.
  s_initialized = false;
  network_prov_mgr_deinit();
  free_sec2_credentials();
  ESP_LOGI(tag, "BLE provisioning deinitialized");
  return ESP_OK;
}

bool ble_provisioning_is_active(void)
{
  // A stop that has been requested but not yet confirmed by NETWORK_PROV_END is already "not
  // active" as far as callers are concerned. Reporting it as active let do_provisioning() take
  // the "just re-show the QR" branch against a manager that was busy tearing itself down.
  return s_active && !s_stopping;
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
  if (s_mem_freed)
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
  s_mem_freed = true;
  ESP_LOGI(tag, "BLE memory released. Free heap: %lu bytes", (unsigned long) esp_get_free_heap_size());
}
