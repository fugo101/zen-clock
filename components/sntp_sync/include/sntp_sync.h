// SPDX-License-Identifier: MIT
// ZenClock — SNTP time synchronization

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"
#include <time.h>

// Default NTP server (Vietnam)
// Alternatives: "2.ntp.vnix.vn", "vn.pool.ntp.org", "pool.ntp.org", "time.google.com"
#define SNTP_PRIMARY_SERVER "1.ntp.vnix.vn"

  /**
   * @brief SNTP sync event passed to callback.
   */
  typedef enum
  {
    SNTP_EVENT_SYNCING, // Sync in progress (initial or re-sync)
    SNTP_EVENT_SYNCED,  // Time successfully synchronized
    SNTP_EVENT_FAILED,  // Sync failed / timed out
  } sntp_sync_event_t;

  /**
   * @brief Callback for SNTP sync state changes.
   * @param event  Current sync state
   */
  typedef void (*sntp_sync_cb_t)(sntp_sync_event_t event);

  /**
   * @brief Start SNTP sync (non-blocking).
   *
   * Configures SNTP client and spawns a persistent task that performs
   * initial sync and periodic re-sync. Calls the callback on each
   * state transition (syncing → synced/failed).
   *
   * @param on_sync  Callback (called from sntp task context)
   */
  esp_err_t sntp_sync_start(sntp_sync_cb_t on_sync);

  /**
   * @brief Notify SNTP that WiFi just reconnected.
   *
   * If last sync is stale (>1h) or has never occurred, wakes the
   * periodic-resync task immediately instead of waiting for the
   * next scheduled interval. Safe to call multiple times.
   */
  void sntp_sync_notify_connected(void);

  /**
   * @brief Force an immediate NTP resync regardless of last sync time.
   * Use for manual user-triggered resyncs from the Settings screen.
   */
  void sntp_sync_force_resync(void);

  /**
   * @brief Return timestamp of last successful NTP sync.
   * Returns 0 if never synced. Persists across deep sleep (RTC memory).
   */
  time_t sntp_sync_get_last_sync_time(void);

#ifdef __cplusplus
}
#endif
