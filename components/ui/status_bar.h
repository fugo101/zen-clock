// SPDX-License-Identifier: MIT
// ZenClock — Status bar widget interface

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

  // ============================================================
  // Tailscale (MicroLink) status for UI display
  // ============================================================

  typedef enum
  {
    TS_STATUS_IDLE,       // no session / not started
    TS_STATUS_CONNECTING, // connecting / registering / reconnecting
    TS_STATUS_CONNECTED,  // tunnel up
    TS_STATUS_ERROR,      // error
  } ts_status_t;

  // ============================================================
  // WiFi status for UI display (WiFi layer only)
  // ============================================================

  typedef enum
  {
    WIFI_STATUS_DISCONNECTED,
    WIFI_STATUS_SCANNING, // Scan in progress (between retries)
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_VERIFYING,    // Got IP, checking internet
    WIFI_STATUS_CONNECTED,    // Verified online
    WIFI_STATUS_NO_INTERNET,  // Associated with an IP, but the internet check failed
    WIFI_STATUS_PROVISIONING, // BLE provisioning active — waiting for credentials
  } wifi_status_t;

  // ============================================================
  // SNTP status for UI display (NTP sync layer)
  // ============================================================

  typedef enum
  {
    SNTP_STATUS_IDLE,    // Not started yet
    SNTP_STATUS_SYNCING, // NTP sync in progress
    SNTP_STATUS_SYNCED,  // Time synchronized
    SNTP_STATUS_FAILED,  // Sync failed / timed out
  } sntp_status_t;

  /**
   * @brief Battery percentage below which the UI shows low-battery state.
   *
   * Shared with app_handlers.c, which clamps brightness on this same threshold — a single
   * constant so the icon color and the brightness policy can never disagree about what "low"
   * means.
   */
#define BATT_LOW_PCT 15

  /**
   * @brief Battery percentage below which the icon blinks in addition to being red.
   */
#define BATT_CRIT_PCT 5

  /**
   * @brief Called from the battery timer with the latest reading.
   *
   * @param pct Battery percentage (0-100), or -1 if unavailable.
   * @param usb True if USB power is present.
   */
  typedef void (*status_bar_battery_cb_t)(int pct, bool usb);

  /**
   * @brief Register a callback for battery readings, piggybacking on the existing 30s timer.
   *
   * status_bar owns the only ADC poll in the UI; this exists so a caller that needs to react to
   * battery level (e.g. clamping brightness) does not have to start a second one. Not a general
   * pub/sub — one callback, last writer wins. NULL clears it.
   */
  void status_bar_register_battery_cb(status_bar_battery_cb_t cb);

  /**
   * @brief Create the status bar on the given parent.
   *
   * Creates SNTP indicator + WiFi indicator + battery icon + percentage label
   * in the top-right corner with a 30-second LVGL timer for
   * automatic battery updates. Timer fires immediately on first tick.
   *
   * Must be called inside lvgl_port_lock()/unlock().
   */
  void status_bar_create(lv_obj_t *parent);

  /**
   * @brief Update WiFi status indicator.
   *
   * Must be called inside lvgl_port_lock()/unlock().
   */
  void status_bar_set_wifi_status(wifi_status_t status);

  /**
   * @brief Update SNTP status indicator.
   *
   * Must be called inside lvgl_port_lock()/unlock().
   */
  void status_bar_set_sntp_status(sntp_status_t status);

  /**
   * @brief Update Tailscale (MicroLink) status indicator.
   *
   * Must be called inside lvgl_port_lock()/unlock().
   */
  void status_bar_set_ts_status(ts_status_t status);

  /**
   * @brief Destroy the status bar and its battery timer.
   *
   * Must be called before deleting the parent screen to avoid
   * stale timer references. Safe to call if not created.
   * Must be called inside lvgl_port_lock()/unlock().
   */
  void status_bar_destroy(void);

#ifdef __cplusplus
}
#endif
