// SPDX-License-Identifier: MIT
// ZenClock — BLE Provisioning via espressif/network_provisioning (ESP-IDF v6)

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"

  // ============================================================
  // Events fired via callback
  // ============================================================

  typedef enum
  {
    BLE_PROV_STARTED,       // BLE advertisement active, waiting for client
    BLE_PROV_CRED_RECEIVED, // Credentials received — ssid/pass args are valid
    BLE_PROV_SUCCESS,       // Provisioning complete — safe to stop and release memory
    BLE_PROV_FAILED,        // Provisioning failed (bad credentials or error)
    BLE_PROV_STOPPED,       // Service stopped without provisioning (cancelled) — do NOT release memory
  } ble_prov_event_t;

  // ssid and pass are only non-NULL for BLE_PROV_CRED_RECEIVED
  typedef void (*ble_prov_cb_t)(ble_prov_event_t event, const char *ssid, const char *pass);

  // ============================================================
  // API
  // ============================================================

  /**
   * @brief Register event callback and event loop handler.
   *        Must be called after wifi_manager_init() (which creates the default event loop).
   */
  esp_err_t ble_provisioning_init(ble_prov_cb_t callback);

  /**
   * @brief Start BLE advertisement as "PROV_ZenClock_XXYY".
   *        Caller must call wifi_manager_stop() first to avoid WiFi driver conflict.
   */
  esp_err_t ble_provisioning_start(void);

  /**
   * @brief Stop BLE provisioning and deinit the manager.
   *
   * Safe to use as a user-facing "cancel". NETWORK_PROV_END means "the service stopped", not
   * "provisioning succeeded", so the handler latches NETWORK_PROV_WIFI_CRED_SUCCESS and reports
   * BLE_PROV_SUCCESS only for a genuine completion; a cancel reports BLE_PROV_STOPPED instead.
   *
   * @warning That distinction is what keeps a cancel survivable: only the BLE_PROV_SUCCESS path
   *          may call ble_provisioning_release_memory(), which frees ~110 KB of BT RAM
   *          permanently. Handling BLE_PROV_STOPPED as if it were success would make the device
   *          unprovisionable until reflashed.
   */
  esp_err_t ble_provisioning_stop(void);

  /**
   * @brief Check whether BLE advertisement is currently active.
   */
  bool ble_provisioning_is_active(void);

  /**
   * @brief Get the BLE device name used for provisioning (e.g. "PROV_ZenClock_A1B2").
   *        Safe to call any time after ble_provisioning_init().
   */
  void ble_provisioning_get_device_name(char *buf, size_t len);

  /**
   * @brief Get the SRP6a password derived from MAC (8 hex chars, e.g. "A1B2C3D4").
   *        Valid after ble_provisioning_start(). Used to display on provisioning screen.
   */
  void ble_provisioning_get_password(char *buf, size_t len);

  /**
   * @brief Free ~110KB of BLE controller RAM.
   *        Must be called only after ble_provisioning_stop().
   */
  void ble_provisioning_release_memory(void);

#ifdef __cplusplus
}
#endif
