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
    const auto ssid = (const char *) cfg->ssid;
    const auto pass = (const char *) cfg->password;
    ESP_LOGI(tag, "Credentials received: SSID=\"%s\"", ssid);
    if (s_callback)
    {
      s_callback(BLE_PROV_CRED_RECEIVED, ssid, pass);
    }
    break;
  }

  case NETWORK_PROV_WIFI_CRED_SUCCESS:
    ESP_LOGI(tag, "Credential verification succeeded");
    break;

  case NETWORK_PROV_WIFI_CRED_FAIL:
  {
    const auto reason = (network_prov_wifi_sta_fail_reason_t *) data;
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
      s_callback(BLE_PROV_SUCCESS, NULL, NULL);
    }
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
    network_prov_mgr_stop_provisioning();
    s_active = false;
  }

  if (s_initialized)
  {
    network_prov_mgr_deinit();
    s_initialized = false;
  }

  free_sec2_credentials();
  ESP_LOGI(tag, "BLE provisioning stopped");
  return ESP_OK;
}

bool ble_provisioning_is_active(void)
{
  return s_active;
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
