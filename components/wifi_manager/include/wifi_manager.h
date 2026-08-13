// SPDX-License-Identifier: MIT
// ZenClock WiFi Manager — Public API

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"

#define WIFI_SSID_MAX_LEN 33
#define WIFI_PASS_MAX_LEN 65

  // ============================================================
  // State Machine
  // ============================================================

  typedef enum
  {
    WIFI_ST_IDLE,       // Initialized, not running. Waiting for start()
    WIFI_ST_SCANNING,   // Aggregated scan in progress
    WIFI_ST_CONNECTING, // Trying stored AP credential
    WIFI_ST_VERIFYING,  // Got IP, checking internet connectivity
    WIFI_ST_CONNECTED,  // Verified online, operational
  } wifi_state_t;

  // ============================================================
  // Events
  // ============================================================

  typedef enum
  {
    WIFI_MGR_SCANNING,     // Started scanning for APs
    WIFI_MGR_CONNECTING,   // Trying to connect
    WIFI_MGR_GOT_IP,       // Got IP but not yet verified online
    WIFI_MGR_CONNECTED,    // Verified online (DNS probe OK)
    WIFI_MGR_NO_INTERNET,  // Fired right after CONNECTED when the DNS probe failed. Informational:
                           // the state machine still enters CONNECTED, because the association and
                           // the IP lease are real and a LAN-only network is usable. Exists so the
                           // UI can say so instead of showing a plain "online".
    WIFI_MGR_DISCONNECTED, // Lost connection — caller should schedule a backoff reconnect (no BLE)
    WIFI_MGR_SCAN_DONE,    // WiFi scan complete
    WIFI_MGR_NO_CRED,      // No credential in NVS — caller should start BLE provisioning
    WIFI_MGR_NO_MATCH,     // Stored AP not found in scan — caller should schedule a backoff reconnect (no BLE)
    WIFI_MGR_ALL_FAILED,   // Connection attempt failed — caller should schedule a backoff reconnect (no BLE)
  } wifi_manager_event_t;

  typedef void (*wifi_event_cb_t)(wifi_manager_event_t event);

  // ============================================================
  // Lifecycle
  // ============================================================

  esp_err_t wifi_manager_init(void);
  esp_err_t wifi_manager_start(void);
  esp_err_t wifi_manager_stop(void);
  void wifi_manager_set_callback(wifi_event_cb_t cb);
  const char *wifi_manager_get_ssid(void);
  const char *wifi_manager_get_ip_str(void);

  // ============================================================
  // Single-credential API
  // ============================================================

  bool wifi_manager_has_credential(void);
  esp_err_t wifi_manager_set_credential(const char *ssid, const char *pass);
  esp_err_t wifi_manager_clear_credential(void);

#ifdef __cplusplus
}
#endif
