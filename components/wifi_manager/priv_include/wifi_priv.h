// SPDX-License-Identifier: MIT
// ZenClock WiFi Manager — Internal declarations
// DO NOT include from outside this component.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wifi_manager.h" // WIFI_SSID_MAX_LEN / WIFI_PASS_MAX_LEN — defined there, not here

#ifdef __cplusplus
extern "C"
{
#endif

  // ============================================================
  // Scan — multiple rounds merged by BSSID
  // ============================================================

#define SCAN_ROUNDS          3   // Number of scan rounds to aggregate
#define SCAN_MIN_TIME_MS     100 // Min active dwell time per channel (ms)
#define SCAN_MAX_TIME_MS     300 // Max active dwell time per channel (ms)
#define SCAN_INTER_DELAY_MS  200 // Pause between scan rounds (ms)
#define MAX_UNIQUE_APS       64  // Max merged AP entries
#define FAST_SCAN_TIMEOUT_MS 500 // Single-channel targeted scan timeout (ms)

  // ============================================================
  // Connection — per-attempt timeout
  // ============================================================

#define CONNECT_TIMEOUT_MS 15000 // Connection attempt timeout (ms)

  // ============================================================
  // Stop — how long wifi_manager_stop() waits for the task to reach IDLE
  // ============================================================

  // Covers every path the task can be in: one scan round (SCAN_MAX_TIME_MS × ~13 channels ≈ 4s —
  // esp_wifi_scan_start() blocks, so do_aggregated_scan() honours a stop between rounds, not
  // during one), the stop-aware connect wait, and the 300ms post-disconnect settle. The margin
  // over the worst case is thin and the scan's duration is the driver's estimate rather than a
  // guarantee, which is why invariant 2 on wifi_manager_stop() in wifi_manager.h still makes
  // ESP_ERR_TIMEOUT a normal outcome for callers to handle.
#define STOP_TIMEOUT_MS 6000
#define STOP_POLL_MS    20

  // ============================================================
  // Single-credential loader (defined in wifi_credentials.c)
  // ============================================================

  bool wifi_cred_load(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len);

  // AP hint: cached BSSID + channel of last successful connection.
  // Enables fast single-channel scan on next boot (~0.5s vs ~8s full scan).
  void wifi_cred_save_ap_hint(const uint8_t *bssid, uint8_t channel);
  bool wifi_cred_load_ap_hint(uint8_t *bssid, uint8_t *channel);
  void wifi_cred_clear_ap_hint(void);

#ifdef __cplusplus
}
#endif
