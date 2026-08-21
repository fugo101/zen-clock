// SPDX-License-Identifier: MIT
// ZenClock — button input policy: the pure decision behind on_button_press()

#include "input_policy.h"

// Active-low buttons: a pin reads 0 while pressed.
#define PIN_PRESSED 0

static nav_action_t map_action(const input_btn_t btn, const input_event_t event)
{
  if (btn == INPUT_BTN_BOOT)
  {
    return (event == INPUT_EVENT_SHORT) ? NAV_ACTION_UP : NAV_ACTION_SELECT;
  }
  return (event == INPUT_EVENT_SHORT) ? NAV_ACTION_DOWN : NAV_ACTION_BACK;
}

input_outcome_t input_policy_decide(const input_btn_t btn, const input_event_t event, const int other_pin_level)
{
  input_outcome_t outcome = {.kind = INPUT_OUTCOME_NAV, .action = NAV_ACTION_UP};

  // Emergency: IO14 held >= 3s -> reset WiFi + BLE provisioning. Bypasses nav entirely.
  if (event == INPUT_EVENT_EMERGENCY && btn == INPUT_BTN_IO14)
  {
    outcome.kind = INPUT_OUTCOME_RESET_WIFI;
    return outcome;
  }

  // Simultaneous hold: both buttons long-pressed -> deep sleep. Whichever button reports LONG
  // first wins the race; the other one is still physically down, which is what other_pin_level
  // reports. EMERGENCY deliberately does not qualify: only a LONG pairs with a held second button.
  if (event == INPUT_EVENT_LONG && other_pin_level == PIN_PRESSED)
  {
    outcome.kind = INPUT_OUTCOME_DEEP_SLEEP;
    return outcome;
  }

  // Anything else navigates — including a BOOT emergency hold, which the BSP never emits today
  // but which must degrade to its long-press action rather than vanish if that ever changes.
  outcome.action = map_action(btn, event);
  return outcome;
}

input_outcome_t input_policy_apply_overlay(const input_outcome_t outcome, const bool prov_visible)
{
  if (!prov_visible || outcome.kind != INPUT_OUTCOME_NAV)
  {
    return outcome;
  }

  input_outcome_t guarded = outcome;
  guarded.kind = (outcome.action == NAV_ACTION_BACK) ? INPUT_OUTCOME_DISMISS_PROV : INPUT_OUTCOME_SWALLOW;
  return guarded;
}
