// SPDX-License-Identifier: MIT
// Host-side unit tests for the pure logic that doesn't need the ESP-IDF toolchain — see
// CLAUDE.md's "Non-Architectural Notes" (Build tooling: [env:native]/test_build_src). Runs via
// `pio test -e native`.

#include <stddef.h>
#include <unity.h>
#include "ui_utils.h"
#include "timezone_fmt.h"
#include "backoff.h"

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
  return UNITY_END();
}
