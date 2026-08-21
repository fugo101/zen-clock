// SPDX-License-Identifier: MIT
// ZenClock — Battery view (see battery_view.h).

#include "battery_view.h"

#include <stdio.h>

// Private on purpose. Once the predicate is a function, a threshold in a public header is an
// invitation to re-derive "low" at a call site — which is exactly how this logic came to be
// written twice. The host tests pin the behaviour at the boundaries instead.
#define BATT_LOW_PCT  15
#define BATT_CRIT_PCT 5

battery_view_t battery_view(const int pct, const bool usb)
{
  battery_view_t v = {
      .symbol = BATTERY_SYMBOL_EMPTY,
      .tint = BATTERY_TINT_DEFAULT,
      .blink = false,
      .text = "N/A",
      .low = false,
  };

  if (pct < 0)
  {
    return v; // no battery / ADC error — "N/A", and never low
  }

  if (usb)
  {
    v.symbol = BATTERY_SYMBOL_CHARGE;
    v.text[0] = '\0';
    return v; // never low, never critical while plugged in
  }

  snprintf(v.text, sizeof(v.text), "%d%%", pct);

  if (pct > 75)
  {
    v.symbol = BATTERY_SYMBOL_FULL;
  }
  else if (pct > 50)
  {
    v.symbol = BATTERY_SYMBOL_3;
  }
  else if (pct > 25)
  {
    v.symbol = BATTERY_SYMBOL_2;
  }
  else if (pct > 5)
  {
    v.symbol = BATTERY_SYMBOL_1;
  }

  v.low = pct < BATT_LOW_PCT;
  v.tint = v.low ? BATTERY_TINT_LOW : BATTERY_TINT_DEFAULT;
  v.blink = pct < BATT_CRIT_PCT;
  return v;
}

battery_clamp_action_t battery_clamp_step(bool *const was_low, const battery_view_t view)
{
  if (view.low == *was_low)
  {
    return BATTERY_CLAMP_NONE;
  }
  *was_low = view.low;
  return view.low ? BATTERY_CLAMP_ON : BATTERY_CLAMP_OFF;
}
