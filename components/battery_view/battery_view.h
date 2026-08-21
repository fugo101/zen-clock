// SPDX-License-Identifier: MIT
// ZenClock — Battery view: the one mapping from a raw battery reading to everything that reacts
// to it.
//
// `bsp_battery_read()` yields a raw (pct, usb) pair. Two unrelated readers care about it: the
// status bar paints an icon, and src/app_handlers.c clamps display brightness. Both used to
// re-derive "is the battery low" from the raw pair, character-for-character, in two components —
// so a change to what "low" means had to land twice. They now share one derived view instead.
//
// Deliberately free of lvgl.h and every ESP-IDF header so it builds for the host-side
// `[env:native]` tests (test/test_pure_logic/). That is why the icon is an abstract enum rather
// than an LV_SYMBOL_* string: components/ui/status_bar.c maps it, the same way it already maps
// the WiFi / SNTP / Tailscale states to appearance.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /** Which battery glyph to show. status_bar.c maps these onto LV_SYMBOL_*. */
  typedef enum
  {
    BATTERY_SYMBOL_EMPTY = 0,
    BATTERY_SYMBOL_1,
    BATTERY_SYMBOL_2,
    BATTERY_SYMBOL_3,
    BATTERY_SYMBOL_FULL,
    BATTERY_SYMBOL_CHARGE, // on USB power — the glyph carries the meaning, the percentage is hidden
  } battery_symbol_t;

  /** How to colour the glyph. DEFAULT means "no local style" — inherit the active theme. */
  typedef enum
  {
    BATTERY_TINT_DEFAULT = 0,
    BATTERY_TINT_LOW,
  } battery_tint_t;

  /** What the brightness clamp should do on this reading. */
  typedef enum
  {
    BATTERY_CLAMP_NONE = 0, // no transition — leave the display alone
    BATTERY_CLAMP_ON,       // crossed into low — apply the ceiling
    BATTERY_CLAMP_OFF,      // left low — restore the user's chosen brightness
  } battery_clamp_action_t;

  /** Everything derived from one reading. Computed once, consumed by every reader. */
  typedef struct
  {
    battery_symbol_t symbol;
    battery_tint_t tint;
    bool blink; // critical — the status bar flashes the glyph on top of the tint
    // Percentage label: "42%", "" on USB, "N/A" when there is no reading. Sized 16 like the
    // buffer this replaced — wide enough that snprintf can never truncate any int, so the
    // mapping needs no clamping rule of its own to satisfy -Werror=format-truncation.
    char text[16];
    // Same bit as `tint == BATTERY_TINT_LOW` today, and kept separate on purpose: `tint` is an
    // appearance decision the status bar owns, `low` is the policy input the brightness clamp
    // owns. Collapsing them would make a power policy read a colour.
    bool low;
  } battery_view_t;

  /**
   * @brief Derive the full view from one reading. Pure.
   *
   * @param pct Battery percentage (0-100), or -1 when unavailable (no battery / ADC error).
   * @param usb True if USB power is present.
   *
   * USB outranks charge level entirely: the ADC reads the USB rail on this board, not the
   * battery, so `pct` is meaningless while plugged in (see docs/adr/0001). It is never low and
   * never critical on USB — the alarm is about running out, not about charge level while
   * plugged in. A `pct` of -1 renders as "N/A" and is likewise never low: unknown must not
   * dim the screen.
   */
  battery_view_t battery_view(int pct, bool usb);

  /**
   * @brief Edge-detect the low-battery transition. Pure; the caller owns the state.
   *
   * Edge-triggered, not level-triggered. The reading arrives every 30 s; clamping on every tick
   * would fight a user who deliberately raises brightness while low, which is confusing since
   * nothing they did caused it. Acting only on the transition means the clamp steps in once and
   * then leaves the display alone.
   *
   * @param was_low In/out: the caller's record of the previous reading's low state. Start false.
   * @param view    The view for the current reading.
   */
  battery_clamp_action_t battery_clamp_step(bool *was_low, battery_view_t view);

#ifdef __cplusplus
}
#endif
