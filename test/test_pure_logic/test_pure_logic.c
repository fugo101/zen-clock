// SPDX-License-Identifier: MIT
// Host-side unit tests for the pure logic that doesn't need the ESP-IDF toolchain — see
// docs/DECISIONS.md ("Build, CI & tooling"). Runs via `pio test -e native`.

#include <unity.h>
#include "ui_utils.h"
#include "timezone_fmt.h"

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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_ui_circ_next_wraps);
  RUN_TEST(test_ui_circ_prev_wraps);
  RUN_TEST(test_timezone_fmt_positive_offset_sign_inverted);
  RUN_TEST(test_timezone_fmt_negative_offset_sign_inverted);
  RUN_TEST(test_timezone_fmt_zero);
  return UNITY_END();
}
