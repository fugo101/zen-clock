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

  // Covers the bounded paths: one scan round (SCAN_MAX_TIME_MS × ~13 channels ≈ 4s), the
  // stop-aware connect wait, and the 300ms post-disconnect settle. It does NOT bound VERIFYING —
  // see invariant 2 on wifi_manager_stop() in wifi_manager.h for why that leaks out to callers.
#define STOP_TIMEOUT_MS 6000
#define STOP_POLL_MS    20

  // ============================================================
  // DNS connectivity probe — verify internet after Got IP
  // ============================================================

#define DNS_PROBE_HOST        "pool.ntp.org"
#define DNS_PROBE_MAX         5    // Max probe attempts
#define DNS_PROBE_INTERVAL_MS 2000 // Between probes (ms)

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
