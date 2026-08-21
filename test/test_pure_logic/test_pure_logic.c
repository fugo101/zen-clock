// SPDX-License-Identifier: MIT
// Host-side unit tests for the pure logic that doesn't need the ESP-IDF toolchain — see
// CLAUDE.md's "Non-Architectural Notes" (Build tooling: [env:native]/test_build_src). Runs via
// `pio test -e native`.

#include <stddef.h>
#include <string.h>
#include <unity.h>
#include "ui_utils.h"
#include "timezone_fmt.h"
#include "backoff.h"
#include "input_policy.h"
#include "battery_view.h"
#include "settings_table.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_ui_circ_next_wraps(void)
{
  TEST_ASSERT_EQUAL_INT(1, ui_circ_next(0, 3));
  TEST_ASSERT_EQUAL_INT(0, ui_circ_next(2, 3)); // wraps past the end
}

static void test_ui_circ_prev_wraps(void)
{
  TEST_ASSERT_EQUAL_INT(2, ui_circ_prev(0, 3)); // wraps before the start
  TEST_ASSERT_EQUAL_INT(0, ui_circ_prev(1, 3));
}

static void test_timezone_fmt_positive_offset_sign_inverted(void)
{
  char buf[12];
  timezone_fmt(buf, sizeof(buf), 7); // UTC+7 -> POSIX "UTC-7" (sign-inversion convention)
  TEST_ASSERT_EQUAL_STRING("UTC-7", buf);
}

static void test_timezone_fmt_negative_offset_sign_inverted(void)
{
  char buf[12];
  timezone_fmt(buf, sizeof(buf), -5);
  TEST_ASSERT_EQUAL_STRING("UTC+5", buf);
}

static void test_timezone_fmt_zero(void)
{
  char buf[12];
  timezone_fmt(buf, sizeof(buf), 0);
  TEST_ASSERT_EQUAL_STRING("UTC+0", buf); // offset > 0 is false at 0, takes the '+' branch
}

// --- backoff ---------------------------------------------------------------

static void test_backoff_doubles_when_armed(void)
{
  TEST_ASSERT_EQUAL_UINT32(60, backoff_next_s(BACKOFF_START_S, true));
  TEST_ASSERT_EQUAL_UINT32(120, backoff_next_s(60, true));
}

static void test_backoff_saturates_at_max(void)
{
  uint32_t s = BACKOFF_START_S;
  for (int i = 0; i < 20; i++)
  {
    s = backoff_next_s(s, true); // 30 -> 60 -> 120 -> 240 -> 300 (480 would overshoot), then stays
  }
  TEST_ASSERT_EQUAL_UINT32(BACKOFF_MAX_S, s);
}

static void test_backoff_full_armed_sequence(void)
{
  // The exact delay sequence both callers must produce from a cold start.
  const uint32_t expected[] = {30, 60, 120, 240, 300, 300, 300};
  uint32_t s = BACKOFF_START_S;
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
  {
    TEST_ASSERT_EQUAL_UINT32(expected[i], s);
    s = backoff_next_s(s, true);
  }
}

static void test_backoff_unarmed_does_not_consume_a_step(void)
{
  // The whole point of the armed flag: a retry that could not be scheduled must not inflate the
  // delay, or a burst of failures leaves a long wait with nothing pending.
  uint32_t s = BACKOFF_START_S;
  s = backoff_next_s(s, false);
  s = backoff_next_s(s, false);
  TEST_ASSERT_EQUAL_UINT32(30, s);
  s = backoff_next_s(s, true);
  TEST_ASSERT_EQUAL_UINT32(60, s);
}

static void test_backoff_unarmed_holds_at_the_ceiling_too(void)
{
  TEST_ASSERT_EQUAL_UINT32(BACKOFF_MAX_S, backoff_next_s(BACKOFF_MAX_S, false));
}

static void test_backoff_reset_to_start_then_doubles_again(void)
{
  // Callers reset by assigning BACKOFF_START_S on success; the next failure must start over at 30.
  uint32_t s = backoff_next_s(backoff_next_s(BACKOFF_START_S, true), true); // 120
  TEST_ASSERT_EQUAL_UINT32(120, s);
  s = BACKOFF_START_S;
  TEST_ASSERT_EQUAL_UINT32(60, backoff_next_s(s, true));
}

// --- input_policy ----------------------------------------------------------
//
// The four guards that used to be fused into on_button_press(). PIN_RELEASED/PIN_PRESSED mirror
// the active-low levels the BSP reads off the *other* button's pad.

#define PIN_PRESSED  0
#define PIN_RELEASED 1

// Runs both stages the way on_button_press() does. Applying the overlay guard unconditionally is
// always correct — it only ever rewrites a NAV outcome — so the tests take the simple path.
static input_outcome_t decide(const input_btn_t btn, const input_event_t event, const int other_pin_level,
                              const bool prov_visible)
{
  return input_policy_apply_overlay(input_policy_decide(btn, event, other_pin_level), prov_visible);
}

static void assert_nav(const input_outcome_t outcome, const nav_action_t expected)
{
  TEST_ASSERT_EQUAL_INT(INPUT_OUTCOME_NAV, outcome.kind);
  TEST_ASSERT_EQUAL_INT(expected, outcome.action);
}

static void test_input_policy_maps_all_four_button_events(void)
{
  assert_nav(decide(INPUT_BTN_BOOT, INPUT_EVENT_SHORT, PIN_RELEASED, false), NAV_ACTION_UP);
  assert_nav(decide(INPUT_BTN_BOOT, INPUT_EVENT_LONG, PIN_RELEASED, false), NAV_ACTION_SELECT);
  assert_nav(decide(INPUT_BTN_IO14, INPUT_EVENT_SHORT, PIN_RELEASED, false), NAV_ACTION_DOWN);
  assert_nav(decide(INPUT_BTN_IO14, INPUT_EVENT_LONG, PIN_RELEASED, false), NAV_ACTION_BACK);
}

static void test_input_policy_emergency_io14_resets_wifi(void)
{
  const input_outcome_t o = decide(INPUT_BTN_IO14, INPUT_EVENT_EMERGENCY, PIN_RELEASED, false);
  TEST_ASSERT_EQUAL_INT(INPUT_OUTCOME_RESET_WIFI, o.kind);
}

static void test_input_policy_emergency_outranks_the_qr_overlay(void)
{
  // The escape hatch has to work from the provisioning screen too — that is where a user with a
  // half-provisioned device is most likely to be holding the button.
  const input_outcome_t o = decide(INPUT_BTN_IO14, INPUT_EVENT_EMERGENCY, PIN_PRESSED, true);
  TEST_ASSERT_EQUAL_INT(INPUT_OUTCOME_RESET_WIFI, o.kind);
}

static void test_input_policy_emergency_on_boot_falls_back_to_select(void)
{
  // The BSP only emits EMERGENCY for IO14; if that ever changes, a BOOT hold must still navigate
  // rather than vanish.
  assert_nav(decide(INPUT_BTN_BOOT, INPUT_EVENT_EMERGENCY, PIN_RELEASED, false), NAV_ACTION_SELECT);
}

static void test_input_policy_deep_sleep_combo_from_either_button(void)
{
  // Whichever button reports LONG first wins the race; the other is still physically down.
  TEST_ASSERT_EQUAL_INT(INPUT_OUTCOME_DEEP_SLEEP,
                        decide(INPUT_BTN_BOOT, INPUT_EVENT_LONG, PIN_PRESSED, false).kind);
  TEST_ASSERT_EQUAL_INT(INPUT_OUTCOME_DEEP_SLEEP,
                        decide(INPUT_BTN_IO14, INPUT_EVENT_LONG, PIN_PRESSED, false).kind);
}

static void test_input_policy_short_press_never_sleeps(void)
{
  // Holding one button while tapping the other must not sleep the device.
  assert_nav(decide(INPUT_BTN_BOOT, INPUT_EVENT_SHORT, PIN_PRESSED, false), NAV_ACTION_UP);
  assert_nav(decide(INPUT_BTN_IO14, INPUT_EVENT_SHORT, PIN_PRESSED, false), NAV_ACTION_DOWN);
}

static void test_input_policy_deep_sleep_combo_outranks_the_qr_overlay(void)
{
  // The policy still reports the combo while the QR is up; declining it is deep_sleep's own
  // inhibit callback's job, not this module's.
  TEST_ASSERT_EQUAL_INT(INPUT_OUTCOME_DEEP_SLEEP,
                        decide(INPUT_BTN_BOOT, INPUT_EVENT_LONG, PIN_PRESSED, true).kind);
}

static void test_input_policy_qr_overlay_swallows_navigation(void)
{
  // Any transition would delete the overlay and nothing would put it back.
  TEST_ASSERT_EQUAL_INT(INPUT_OUTCOME_SWALLOW,
                        decide(INPUT_BTN_BOOT, INPUT_EVENT_SHORT, PIN_RELEASED, true).kind);
  TEST_ASSERT_EQUAL_INT(INPUT_OUTCOME_SWALLOW,
                        decide(INPUT_BTN_BOOT, INPUT_EVENT_LONG, PIN_RELEASED, true).kind);
  TEST_ASSERT_EQUAL_INT(INPUT_OUTCOME_SWALLOW,
                        decide(INPUT_BTN_IO14, INPUT_EVENT_SHORT, PIN_RELEASED, true).kind);
}

static void test_input_policy_qr_overlay_back_dismisses(void)
{
  // BACK (IO14 long) is the one action that gets through: an unprovisioned device sits here
  // indefinitely and still has to be usable as a clock.
  TEST_ASSERT_EQUAL_INT(INPUT_OUTCOME_DISMISS_PROV,
                        decide(INPUT_BTN_IO14, INPUT_EVENT_LONG, PIN_RELEASED, true).kind);
}

static void test_input_policy_overlay_guard_leaves_escape_hatches_alone(void)
{
  // on_button_press() skips this call for non-NAV outcomes to stay off the LVGL lock; that is
  // only safe because applying it would have changed nothing.
  const input_outcome_t reset = input_policy_decide(INPUT_BTN_IO14, INPUT_EVENT_EMERGENCY, PIN_RELEASED);
  const input_outcome_t sleep = input_policy_decide(INPUT_BTN_BOOT, INPUT_EVENT_LONG, PIN_PRESSED);
  TEST_ASSERT_EQUAL_INT(reset.kind, input_policy_apply_overlay(reset, true).kind);
  TEST_ASSERT_EQUAL_INT(sleep.kind, input_policy_apply_overlay(sleep, true).kind);
}

static void test_input_policy_boot_emergency_is_still_swallowed_by_the_overlay(void)
{
  // The BOOT-emergency fallback degrades to SELECT, so it must stay subject to the overlay guard
  // — letting it through would delete the QR and nothing would put it back.
  TEST_ASSERT_EQUAL_INT(INPUT_OUTCOME_SWALLOW,
                        decide(INPUT_BTN_BOOT, INPUT_EVENT_EMERGENCY, PIN_RELEASED, true).kind);
}

// ============================================================
// battery_view — one mapping, consumed by the status bar and the brightness clamp
// ============================================================

static void test_battery_view_symbol_ladder_boundaries(void)
{
  // The ladder is strictly-greater-than, so every boundary pct belongs to the rung below it.
  TEST_ASSERT_EQUAL_INT(BATTERY_SYMBOL_FULL, battery_view(76, false).symbol);
  TEST_ASSERT_EQUAL_INT(BATTERY_SYMBOL_3, battery_view(75, false).symbol);
  TEST_ASSERT_EQUAL_INT(BATTERY_SYMBOL_3, battery_view(51, false).symbol);
  TEST_ASSERT_EQUAL_INT(BATTERY_SYMBOL_2, battery_view(50, false).symbol);
  TEST_ASSERT_EQUAL_INT(BATTERY_SYMBOL_2, battery_view(26, false).symbol);
  TEST_ASSERT_EQUAL_INT(BATTERY_SYMBOL_1, battery_view(25, false).symbol);
  TEST_ASSERT_EQUAL_INT(BATTERY_SYMBOL_1, battery_view(6, false).symbol);
  TEST_ASSERT_EQUAL_INT(BATTERY_SYMBOL_EMPTY, battery_view(5, false).symbol);
}

static void test_battery_view_formats_the_percentage(void)
{
  TEST_ASSERT_EQUAL_STRING("42%", battery_view(42, false).text);
  TEST_ASSERT_EQUAL_STRING("100%", battery_view(100, false).text);
  TEST_ASSERT_EQUAL_STRING("0%", battery_view(0, false).text);
  TEST_ASSERT_EQUAL_STRING("127%", battery_view(127, false).text); // never truncated
}

static void test_battery_view_usb_overrides_charge_level(void)
{
  // The ADC reads the USB rail, not the battery, so pct is meaningless while plugged in: the
  // charge glyph carries the meaning and the number is hidden. Never low, never critical.
  const battery_view_t v = battery_view(2, true);
  TEST_ASSERT_EQUAL_INT(BATTERY_SYMBOL_CHARGE, v.symbol);
  TEST_ASSERT_EQUAL_STRING("", v.text);
  TEST_ASSERT_EQUAL_INT(BATTERY_TINT_DEFAULT, v.tint);
  TEST_ASSERT_FALSE(v.low);
  TEST_ASSERT_FALSE(v.blink);
}

static void test_battery_view_no_reading_is_na_and_not_low(void)
{
  // Unknown must not dim the screen, tint the icon, or blink it.
  const battery_view_t v = battery_view(-1, false);
  TEST_ASSERT_EQUAL_INT(BATTERY_SYMBOL_EMPTY, v.symbol);
  TEST_ASSERT_EQUAL_STRING("N/A", v.text);
  TEST_ASSERT_EQUAL_INT(BATTERY_TINT_DEFAULT, v.tint);
  TEST_ASSERT_FALSE(v.low);
  TEST_ASSERT_FALSE(v.blink);
}

static void test_battery_view_low_boundary(void)
{
  TEST_ASSERT_TRUE(battery_view(14, false).low);
  TEST_ASSERT_EQUAL_INT(BATTERY_TINT_LOW, battery_view(14, false).tint);
  TEST_ASSERT_FALSE(battery_view(15, false).low);
  TEST_ASSERT_EQUAL_INT(BATTERY_TINT_DEFAULT, battery_view(15, false).tint);
}

static void test_battery_view_critical_boundary(void)
{
  TEST_ASSERT_TRUE(battery_view(4, false).blink);
  TEST_ASSERT_FALSE(battery_view(5, false).blink);
  TEST_ASSERT_TRUE(battery_view(4, false).low); // critical is always also low
}

static void test_battery_clamp_fires_once_on_the_edge(void)
{
  bool was_low = false;
  TEST_ASSERT_EQUAL_INT(BATTERY_CLAMP_ON, battery_clamp_step(&was_low, battery_view(10, false)));
  // Level-triggered would clamp again here and fight a user who raised brightness deliberately.
  TEST_ASSERT_EQUAL_INT(BATTERY_CLAMP_NONE, battery_clamp_step(&was_low, battery_view(9, false)));
  TEST_ASSERT_TRUE(was_low);
}

static void test_battery_clamp_restores_on_recovery(void)
{
  bool was_low = true;
  TEST_ASSERT_EQUAL_INT(BATTERY_CLAMP_OFF, battery_clamp_step(&was_low, battery_view(50, false)));
  TEST_ASSERT_FALSE(was_low);
  TEST_ASSERT_EQUAL_INT(BATTERY_CLAMP_NONE, battery_clamp_step(&was_low, battery_view(50, false)));
}

static void test_battery_clamp_releases_on_usb(void)
{
  // Plugging in while low is a recovery, even though the charge level has not moved.
  bool was_low = true;
  TEST_ASSERT_EQUAL_INT(BATTERY_CLAMP_OFF, battery_clamp_step(&was_low, battery_view(10, true)));
  TEST_ASSERT_FALSE(was_low);
}

static void test_battery_clamp_releases_when_the_reading_is_lost(void)
{
  // Deliberate: an ADC failure while low restores full brightness rather than holding the clamp
  // on a reading nothing can confirm.
  bool was_low = true;
  TEST_ASSERT_EQUAL_INT(BATTERY_CLAMP_OFF, battery_clamp_step(&was_low, battery_view(-1, false)));
  TEST_ASSERT_FALSE(was_low);
}

static void test_battery_clamp_starts_quiet_on_a_healthy_battery(void)
{
  bool was_low = false;
  TEST_ASSERT_EQUAL_INT(BATTERY_CLAMP_NONE, battery_clamp_step(&was_low, battery_view(80, false)));
  TEST_ASSERT_FALSE(was_low);
}

// ---- settings_table ----------------------------------------------------------------------
// The descriptor table is the single owner of every persisted setting's key, default and range.
// These tests are the reason it is a pure translation unit with no nvs.h/esp_log.h.

static void test_settings_desc_covers_every_key(void)
{
  TEST_ASSERT_NULL(settings_desc(SETTINGS_KEY_NONE));
  TEST_ASSERT_NULL(settings_desc(SETTINGS_KEY_COUNT));
  for (settings_key_t k = SETTINGS_KEY_NONE + 1; k < SETTINGS_KEY_COUNT; k++)
  {
    const settings_desc_t *d = settings_desc(k);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_NOT_NULL(d->nvs_key);
    // NVS caps key names at 15 characters plus the NUL; a longer one fails at runtime only.
    TEST_ASSERT_TRUE(d->nvs_key[0] != '\0');
    TEST_ASSERT_TRUE(strlen(d->nvs_key) <= 15);
    TEST_ASSERT_TRUE(d->min <= d->max);
    // A default outside its own range would defeat clamp-on-read on a fresh device.
    TEST_ASSERT_TRUE(d->def >= d->min && d->def <= d->max);
  }
}

static void test_settings_clamp_brightness_floor_and_ceiling(void)
{
  // The ADR-0004 bug: a stored 0 left the panel unreadable with no way back to Settings.
  TEST_ASSERT_EQUAL_INT(10, settings_clamp(SETTINGS_KEY_BRIGHTNESS, 0));
  TEST_ASSERT_EQUAL_INT(100, settings_clamp(SETTINGS_KEY_BRIGHTNESS, 127));
  TEST_ASSERT_EQUAL_INT(50, settings_clamp(SETTINGS_KEY_BRIGHTNESS, 50));
}

static void test_settings_clamp_timezone_boundaries(void)
{
  TEST_ASSERT_EQUAL_INT(-12, settings_clamp(SETTINGS_KEY_TZ_OFFSET, -13));
  TEST_ASSERT_EQUAL_INT(-12, settings_clamp(SETTINGS_KEY_TZ_OFFSET, -12));
  TEST_ASSERT_EQUAL_INT(14, settings_clamp(SETTINGS_KEY_TZ_OFFSET, 14));
  TEST_ASSERT_EQUAL_INT(14, settings_clamp(SETTINGS_KEY_TZ_OFFSET, 15));
  TEST_ASSERT_EQUAL_INT(7, settings_clamp(SETTINGS_KEY_TZ_OFFSET, 7));
}

static void test_settings_clamp_sleep_components(void)
{
  TEST_ASSERT_EQUAL_INT(23, settings_clamp(SETTINGS_KEY_SLEEP_H, 24));
  TEST_ASSERT_EQUAL_INT(59, settings_clamp(SETTINGS_KEY_SLEEP_M, 60));
  TEST_ASSERT_EQUAL_INT(59, settings_clamp(SETTINGS_KEY_SLEEP_S, 99));
  TEST_ASSERT_EQUAL_INT(0, settings_clamp(SETTINGS_KEY_SLEEP_H, -1));
}

static void test_settings_clamp_booleans_are_zero_or_one(void)
{
  TEST_ASSERT_EQUAL_INT(1, settings_clamp(SETTINGS_KEY_THEME_LIGHT, 2));
  TEST_ASSERT_EQUAL_INT(0, settings_clamp(SETTINGS_KEY_SHOW_SECS, -5));
}

static void test_settings_clamp_unknown_key_is_zero(void)
{
  TEST_ASSERT_EQUAL_INT(0, settings_clamp(SETTINGS_KEY_NONE, 42));
  TEST_ASSERT_EQUAL_INT(0, settings_clamp(SETTINGS_KEY_COUNT, 42));
}

static void test_settings_option_index_respects_polarity(void)
{
  // Theme's option array is {Dark, Light}, so index == stored bool.
  TEST_ASSERT_EQUAL_INT(0, settings_option_index(SETTINGS_KEY_THEME_LIGHT, 0));
  TEST_ASSERT_EQUAL_INT(1, settings_option_index(SETTINGS_KEY_THEME_LIGHT, 1));
  // Time Format's is {24H, 12H} and Show Secs' is {On, Off}: index is the inverse.
  TEST_ASSERT_EQUAL_INT(0, settings_option_index(SETTINGS_KEY_TIME_FMT, 1));
  TEST_ASSERT_EQUAL_INT(1, settings_option_index(SETTINGS_KEY_TIME_FMT, 0));
  TEST_ASSERT_EQUAL_INT(0, settings_option_index(SETTINGS_KEY_SHOW_SECS, 1));
  TEST_ASSERT_EQUAL_INT(1, settings_option_index(SETTINGS_KEY_SHOW_SECS, 0));
}

static void test_settings_option_index_is_involutive(void)
{
  // The same call converts both ways, which is why load and persist can share one mapping
  // instead of the two hand-written inversions that used to drift apart.
  for (settings_key_t k = SETTINGS_KEY_NONE + 1; k < SETTINGS_KEY_COUNT; k++)
  {
    for (int v = 0; v <= 1; v++)
    {
      TEST_ASSERT_EQUAL_INT(v, settings_option_index(k, settings_option_index(k, v)));
    }
  }
}

static void test_settings_sleep_seconds_sums_components(void)
{
  TEST_ASSERT_EQUAL_UINT32(0, settings_sleep_seconds(0, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(3600, settings_sleep_seconds(1, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(86399, settings_sleep_seconds(23, 59, 59));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_ui_circ_next_wraps);
  RUN_TEST(test_ui_circ_prev_wraps);
  RUN_TEST(test_timezone_fmt_positive_offset_sign_inverted);
  RUN_TEST(test_timezone_fmt_negative_offset_sign_inverted);
  RUN_TEST(test_timezone_fmt_zero);
  RUN_TEST(test_backoff_doubles_when_armed);
  RUN_TEST(test_backoff_saturates_at_max);
  RUN_TEST(test_backoff_full_armed_sequence);
  RUN_TEST(test_backoff_unarmed_does_not_consume_a_step);
  RUN_TEST(test_backoff_unarmed_holds_at_the_ceiling_too);
  RUN_TEST(test_backoff_reset_to_start_then_doubles_again);
  RUN_TEST(test_input_policy_maps_all_four_button_events);
  RUN_TEST(test_input_policy_emergency_io14_resets_wifi);
  RUN_TEST(test_input_policy_emergency_outranks_the_qr_overlay);
  RUN_TEST(test_input_policy_emergency_on_boot_falls_back_to_select);
  RUN_TEST(test_input_policy_deep_sleep_combo_from_either_button);
  RUN_TEST(test_input_policy_short_press_never_sleeps);
  RUN_TEST(test_input_policy_deep_sleep_combo_outranks_the_qr_overlay);
  RUN_TEST(test_input_policy_qr_overlay_swallows_navigation);
  RUN_TEST(test_input_policy_qr_overlay_back_dismisses);
  RUN_TEST(test_input_policy_overlay_guard_leaves_escape_hatches_alone);
  RUN_TEST(test_input_policy_boot_emergency_is_still_swallowed_by_the_overlay);
  RUN_TEST(test_battery_view_symbol_ladder_boundaries);
  RUN_TEST(test_battery_view_formats_the_percentage);
  RUN_TEST(test_battery_view_usb_overrides_charge_level);
  RUN_TEST(test_battery_view_no_reading_is_na_and_not_low);
  RUN_TEST(test_battery_view_low_boundary);
  RUN_TEST(test_battery_view_critical_boundary);
  RUN_TEST(test_battery_clamp_fires_once_on_the_edge);
  RUN_TEST(test_battery_clamp_restores_on_recovery);
  RUN_TEST(test_battery_clamp_releases_on_usb);
  RUN_TEST(test_battery_clamp_releases_when_the_reading_is_lost);
  RUN_TEST(test_battery_clamp_starts_quiet_on_a_healthy_battery);
  RUN_TEST(test_settings_desc_covers_every_key);
  RUN_TEST(test_settings_clamp_brightness_floor_and_ceiling);
  RUN_TEST(test_settings_clamp_timezone_boundaries);
  RUN_TEST(test_settings_clamp_sleep_components);
  RUN_TEST(test_settings_clamp_booleans_are_zero_or_one);
  RUN_TEST(test_settings_clamp_unknown_key_is_zero);
  RUN_TEST(test_settings_option_index_respects_polarity);
  RUN_TEST(test_settings_option_index_is_involutive);
  RUN_TEST(test_settings_sleep_seconds_sums_components);
  return UNITY_END();
}
