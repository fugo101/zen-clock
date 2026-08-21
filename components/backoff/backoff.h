// SPDX-License-Identifier: MIT
// ZenClock — Retry backoff policy: start at 30 s, double on each armed retry, cap at 5 minutes,
// reset to BACKOFF_START_S once the thing being retried succeeds.
//
// One policy, one place. Both the WiFi reconnect timer (src/app_handlers.c) and the NTP re-sync
// loop (components/sntp_sync/) pace from this. Deliberately free of esp_timer/FreeRTOS/esp_log so
// it builds for the host-side `[env:native]` tests (test/test_pure_logic/).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** Delay the first retry waits, and the value a caller resets to on success. */
#define BACKOFF_START_S 30U
/** Ceiling — the delay never exceeds this. */
#define BACKOFF_MAX_S 300U

  /** The whole policy: given the delay the current attempt used, returns the delay the next one
      should use. `armed` says whether that attempt was actually scheduled — a retry that could not
      be armed must not consume a step, or a burst of failures inflates the delay while leaving
      nothing pending. Pure: the caller owns the state. */
  uint32_t backoff_next_s(uint32_t current_s, bool armed);

#ifdef __cplusplus
}
#endif
