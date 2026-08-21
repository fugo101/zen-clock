// SPDX-License-Identifier: MIT
// ZenClock — Status bar widget interface

#pragma once

#include "lvgl.h"

#include "battery_view.h"

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
   * @brief Called from the battery timer with the derived view of the latest reading.
   *
   * Carries the view rather than the raw (pct, usb) pair so every reader consumes the same
   * derivation — the thresholds and the "never low on USB" rule live in components/battery_view/
   * and cannot be re-derived differently here. See ADR-0001.
   */
  typedef void (*status_bar_battery_cb_t)(battery_view_t view);

  /**
   * @brief Register a callback for battery readings, piggybacking on the existing 30s timer.
   *
   * status_bar owns the only ADC poll in the UI; this exists so a caller that needs to react to
   * the battery view (e.g. clamping brightness) does not have to start a second one. Not a general
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
   * @brief Publish the WiFi status indicator's desired state.
   *
   * Callable from any task, with or without the LVGL lock held — it writes a published value and
   * returns. The status bar repaints it on the LVGL task within one reconcile tick (250 ms), and
   * replays it whenever the bar is recreated on a screen change, so a publish can never be lost.
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
