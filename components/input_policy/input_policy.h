// SPDX-License-Identifier: MIT
// ZenClock — button input policy: the pure decision behind on_button_press()

#pragma once

#include <stdbool.h>

#include "nav.h"

#ifdef __cplusplus
extern "C"
{
#endif

  // Mirrors of the BSP button vocabulary. Duplicated rather than included because bsp.h pulls in
  // esp_lvgl_port.h, and this module must stay buildable for the host-side [env:native] tests.
  // src/app_handlers.c static-asserts the values against BSP's so the two cannot drift silently.
  typedef enum
  {
    INPUT_BTN_BOOT = 0,
    INPUT_BTN_IO14 = 1,
  } input_btn_t;

  typedef enum
  {
    INPUT_EVENT_SHORT = 0,     // released < 800ms
    INPUT_EVENT_LONG = 1,      // held >= 800ms
    INPUT_EVENT_EMERGENCY = 2, // held >= 3000ms (IO14 only)
  } input_event_t;

  typedef enum
  {
    INPUT_OUTCOME_NAV,          // run .action through the nav state machine
    INPUT_OUTCOME_RESET_WIFI,   // emergency IO14 hold — reset WiFi, bypassing nav
    INPUT_OUTCOME_DEEP_SLEEP,   // both buttons held — trigger deep sleep
    INPUT_OUTCOME_DISMISS_PROV, // BACK while the QR overlay is up — hide it, keep advertising
    INPUT_OUTCOME_SWALLOW,      // QR overlay is up — drop the press so no transition deletes it
  } input_outcome_kind_t;

  typedef struct
  {
    input_outcome_kind_t kind;
    nav_action_t action; // only meaningful when kind == INPUT_OUTCOME_NAV
  } input_outcome_t;

  /**
   * @brief Decide what a button press means. Pure: no side effects, no hardware, no LVGL.
   *
   * Resolves, in priority order, the four guards that used to be fused into on_button_press():
   *   1. Emergency IO14 hold          -> INPUT_OUTCOME_RESET_WIFI
   *   2. Both buttons held long       -> INPUT_OUTCOME_DEEP_SLEEP
   *   3. QR overlay up                -> INPUT_OUTCOME_DISMISS_PROV (BACK) or INPUT_OUTCOME_SWALLOW
   *   4. Otherwise                    -> INPUT_OUTCOME_NAV
   *
   * The emergency and deep-sleep outcomes deliberately outrank the overlay guard, matching the
   * order the guards were written in: the emergency hold is an escape hatch that must stay
   * reachable from any screen, and the sleep combo is declined by deep_sleep's own inhibit
   * callback while the QR is up rather than by this policy.
   *
   * @param btn             which button fired
   * @param event           short / long / emergency
   * @param other_pin_level raw level of the *other* button's pin, active-low (0 = pressed)
   * @param prov_visible    whether the provisioning QR overlay is currently on screen
   */
  input_outcome_t input_policy_decide(input_btn_t btn, input_event_t event, int other_pin_level, bool prov_visible);

#ifdef __cplusplus
}
#endif
