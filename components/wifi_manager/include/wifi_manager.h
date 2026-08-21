// SPDX-License-Identifier: MIT
// ZenClock WiFi Manager — Public API

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"

  // ============================================================
  // Buffer sizes — the single definition for this component, public and private alike
  // ============================================================

#define WIFI_SSID_MAX_LEN 33 // IEEE 802.11 max SSID (32) + NUL
#define WIFI_PASS_MAX_LEN 65 // WPA max passphrase (64) + NUL

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

  // Wakes the task out of IDLE and clears any latched stop request. Returns
  // ESP_ERR_INVALID_STATE if the machine is not IDLE — that is a "not yet", not a failure: the
  // task is still busy and no event will be fired for the refused call, so the caller has to
  // re-arm its own retry rather than wait for one.
  esp_err_t wifi_manager_start(void);

  // Requests a stop and, unless called from the wifi task, blocks until the task has parked in
  // IDLE with the radio disconnected.
  //
  // Five invariants a caller must know. They are numbered, and cited by number from wifi_priv.h
  // and src/app_handlers.c — renumber them and those citations go stale silently.
  //
  //  1. No event is fired for the stop, nor for anything it interrupts. A stop mid-scan or
  //     mid-connect deliberately suppresses NO_MATCH / ALL_FAILED / DISCONNECTED so the app
  //     cannot schedule a reconnect against its own deliberate stop. Consequence: **nothing
  //     brings WiFi back but the caller.** Whoever stops it owns restarting it.
  //  2. ESP_ERR_TIMEOUT is a normal outcome, not an error. The wait is bounded, but the DNS
  //     probe in VERIFYING is not — getaddrinfo() takes no timeout argument, and stopping
  //     disconnects the link out from under an in-flight lookup. On timeout the stop is still
  //     latched and the task will park shortly; only the radio's idleness is uncertain, which
  //     matters solely to callers about to hand the radio to something else (BLE provisioning).
  //     Proceed anyway — refusing to would leave the user with no way out.
  //  3. Called from the wifi task itself (i.e. from inside a wifi_event_cb_t), it returns ESP_OK
  //     immediately without waiting. Waiting would be waiting on ourselves. The radio is not idle
  //     yet when it returns.
  //  4. A stop requested while the task is already parked in IDLE stays latched until
  //     wifi_manager_start() clears it — the parked task is blocked on its notify and never
  //     reaches the loop top that would consume the bit. (A stop the running task does observe
  //     clears itself there.) Stopping twice is safe; forgetting to start is not.
  //  5. It calls esp_wifi_disconnect(), so a stop always also looks like a disconnect from the
  //     driver's side. Internally BIT_STOP is therefore always tested before BIT_DISCONNECTED.
  esp_err_t wifi_manager_stop(void);

  void wifi_manager_set_callback(wifi_event_cb_t cb);

  // Both return a pointer to a static internal buffer, or NULL when there is nothing to report.
  //
  // The buffers are written from other tasks — the SSID from the wifi task, the IP from the
  // esp_event task — and are not locked. They are always NUL-terminated, so a read is never
  // unbounded, but the contents may go stale or change between two calls. **Consume the string
  // within the calling function; never store the pointer.** Fine for painting a label (LVGL
  // copies), not for anything that must agree with the radio's actual state.
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
