// SPDX-License-Identifier: MIT
// Host-side unit tests for the pure logic that doesn't need the ESP-IDF toolchain — see
// CLAUDE.md's "Non-Architectural Notes" (Build tooling: [env:native]/test_build_src). Runs via
// `pio test -e native`.

#include <stddef.h>
#include <unity.h>
#include "ui_utils.h"
#include "timezone_fmt.h"
#include "backoff.h"
#include "input_policy.h"

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
  return UNITY_END();
}
