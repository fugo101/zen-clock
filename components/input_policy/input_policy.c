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

input_outcome_t input_policy_decide(const input_btn_t btn,
                                    const input_event_t event,
                                    const int other_pin_level,
                                    const bool prov_visible)
{
  input_outcome_t outcome = {.kind = INPUT_OUTCOME_NAV, .action = NAV_ACTION_UP};

  // Emergency: IO14 held >= 3s -> reset WiFi + BLE provisioning. Bypasses nav entirely, and
  // outranks the overlay guard so it stays reachable from the provisioning screen too.
  if (event == INPUT_EVENT_EMERGENCY)
  {
    if (btn == INPUT_BTN_IO14)
    {
      outcome.kind = INPUT_OUTCOME_RESET_WIFI;
      return outcome;
    }
    // BSP only emits EMERGENCY for IO14; treat anything else as a plain long press rather than
    // silently dropping it.
    outcome.action = map_action(btn, INPUT_EVENT_LONG);
    return outcome;
  }

  // Simultaneous hold: both buttons long-pressed -> deep sleep. Whichever button reports LONG
  // first wins the race; the other one is still physically down, which is what other_pin_level
  // reports.
  if (event == INPUT_EVENT_LONG && other_pin_level == PIN_PRESSED)
  {
    outcome.kind = INPUT_OUTCOME_DEEP_SLEEP;
    return outcome;
  }

  const nav_action_t action = map_action(btn, event);

  // The QR overlay is a child of whatever screen is active, so any nav transition would delete it
  // and nothing would ever put it back — provisioning would keep advertising behind a blank UI.
  // Swallow nav actions while it is up, but let BACK dismiss it: a device that has never been
  // provisioned stays in this state indefinitely, and it still has to be usable as a clock.
  // Provisioning continues in the background; Settings -> Network -> Provisioning brings it back.
  if (prov_visible)
  {
    outcome.kind = (action == NAV_ACTION_BACK) ? INPUT_OUTCOME_DISMISS_PROV : INPUT_OUTCOME_SWALLOW;
    return outcome;
  }

  outcome.action = action;
  return outcome;
}
