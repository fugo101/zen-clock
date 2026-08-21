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
   * @brief Decide what a button press means, ignoring the provisioning overlay.
   * Pure: no side effects, no hardware, no LVGL.
   *
   * Resolves the two guards that outrank the overlay, then falls back to the button -> action
   * mapping:
   *   1. Emergency IO14 hold    -> INPUT_OUTCOME_RESET_WIFI
   *   2. Both buttons held long -> INPUT_OUTCOME_DEEP_SLEEP
   *   3. Otherwise              -> INPUT_OUTCOME_NAV
   *
   * Split from the overlay guard so the caller can resolve these two *before* touching LVGL:
   * both are escape hatches, and reading prov_screen_is_visible() first would put an unbounded
   * lvgl_port_lock(0) on their path and stall bsp_buttons.c's held_ms behind a screen repaint.
   * The sleep combo is declined while the QR is up by deep_sleep's own inhibit callback, not by
   * this policy.
   *
   * @param btn             which button fired
   * @param event           short / long / emergency
   * @param other_pin_level raw level of the *other* button's pin, active-low (0 = pressed).
   *                        Sample it before any blocking call: the other button is still
   *                        physically down at LONG-fire time and may be released a moment later.
   */
  input_outcome_t input_policy_decide(input_btn_t btn, input_event_t event, int other_pin_level);

  /**
   * @brief Apply the QR-overlay guard to a decided outcome. Pure.
   *
   * Only INPUT_OUTCOME_NAV is affected — everything else passes through untouched, so calling
   * this unconditionally is always correct; the caller skips it purely to avoid the LVGL lock
   * that reading @p prov_visible costs.
   *
   * The overlay is a child of whatever screen is active, so any nav transition would delete it
   * and nothing would ever put it back — provisioning would keep advertising behind a blank UI.
   * Nav actions are swallowed while it is up, except BACK, which dismisses it: a device that has
   * never been provisioned stays in this state indefinitely and still has to be usable as a
   * clock.
   */
  input_outcome_t input_policy_apply_overlay(input_outcome_t outcome, bool prov_visible);

#ifdef __cplusplus
}
#endif
