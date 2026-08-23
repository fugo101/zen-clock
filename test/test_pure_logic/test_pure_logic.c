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
#include "prov_session.h"

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

// ============================================================
// prov_session — the provisioning lifecycle
//
// The machine that decides BLE_PROV_SUCCESS vs BLE_PROV_STOPPED, which is what permits the one-way
// release of ~110 KB of BT RAM. Two of these tests are properties over every input sequence rather
// than single transitions, because both directions are irreversible: reporting success without a
// verified credential bricks re-provisioning, and leaving the terminal phase would let a device
// try to start a session with no controller behind it.
// ============================================================

static const prov_input_t k_all_inputs[] = {
    PROV_IN_START_BEGUN, PROV_IN_START_OK,      PROV_IN_START_FAILED,
    PROV_IN_STOP_REQUESTED, PROV_IN_CRED_RECEIVED, PROV_IN_CRED_VERIFIED,
    PROV_IN_CRED_FAILED, PROV_IN_END,          PROV_IN_MEM_RELEASED,
};
static const prov_phase_t k_all_phases[] = {
    PROV_PHASE_IDLE, PROV_PHASE_STARTING, PROV_PHASE_ADVERTISING, PROV_PHASE_STOPPING,
    PROV_PHASE_UNAVAILABLE,
};
#define N_INPUTS (sizeof(k_all_inputs) / sizeof(k_all_inputs[0]))
#define N_PHASES (sizeof(k_all_phases) / sizeof(k_all_phases[0]))

static prov_session_t sess(prov_phase_t phase, bool verified)
{
  const prov_session_t s = {.phase = phase, .verified = verified};
  return s;
}

static void test_prov_session_start_reaches_advertising(void)
{
  prov_step_t r = prov_session_step(sess(PROV_PHASE_IDLE, false), PROV_IN_START_BEGUN);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_STARTING, r.next.phase);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_NONE, r.action);

  r = prov_session_step(r.next, PROV_IN_START_OK);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_ADVERTISING, r.next.phase);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_EMIT_STARTED, r.action);
}

static void test_prov_session_start_failure_returns_to_idle(void)
{
  const prov_step_t r = prov_session_step(sess(PROV_PHASE_STARTING, false), PROV_IN_START_FAILED);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_IDLE, r.next.phase);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_NONE, r.action);
}

// The reason START_OK is a guard and not a plain assignment: ble_provisioning.c writes it from both
// the NETWORK_PROV_START handler and the tail of start(), which run on different tasks. A second
// write arriving after the session already ended must not resurrect it.
static void test_prov_session_start_ok_never_resurrects_a_finished_session(void)
{
  prov_step_t r = prov_session_step(sess(PROV_PHASE_IDLE, false), PROV_IN_START_OK);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_IDLE, r.next.phase);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_NONE, r.action);

  // Idempotent while advertising — no second BLE_PROV_STARTED.
  r = prov_session_step(sess(PROV_PHASE_ADVERTISING, false), PROV_IN_START_OK);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_ADVERTISING, r.next.phase);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_NONE, r.action);
}

static void test_prov_session_start_clears_a_previous_outcome(void)
{
  // A previous session's success must not make this one's END look like a success too.
  const prov_step_t r = prov_session_step(sess(PROV_PHASE_IDLE, true), PROV_IN_START_BEGUN);
  TEST_ASSERT_FALSE(r.next.verified);
}

// Regression: a start landing while a previous session was still winding down used to advance
// nothing, leaving the phase at STOPPING with a live service behind it and no BLE_PROV_STARTED ever
// emitted — the user saw no QR. START_BEGUN is an explicit app action after a successful
// network_prov_mgr_init(), so it resets the session from any live phase.
static void test_prov_session_start_resets_any_live_phase(void)
{
  const prov_phase_t live[] = {PROV_PHASE_IDLE, PROV_PHASE_STARTING, PROV_PHASE_ADVERTISING,
                               PROV_PHASE_STOPPING};
  for (size_t i = 0; i < sizeof(live) / sizeof(live[0]); i++)
  {
    const prov_step_t r = prov_session_step(sess(live[i], true), PROV_IN_START_BEGUN);
    TEST_ASSERT_EQUAL_INT(PROV_PHASE_STARTING, r.next.phase);
    TEST_ASSERT_FALSE(r.next.verified); // a previous outcome must not carry into a new session
  }
}

// Regression, and the worst bug this refactor briefly had: gating the credential on ADVERTISING
// meant a phone finishing provisioning while the user backed out had its credential dropped, while
// the verification behind it still latched an outcome — so END reported success, released 110 KB of
// BT RAM for good, and reconnected on the old credential. A received credential is never dropped.
static void test_prov_session_credential_is_never_dropped(void)
{
  const prov_phase_t live[] = {PROV_PHASE_STARTING, PROV_PHASE_ADVERTISING, PROV_PHASE_STOPPING};
  for (size_t i = 0; i < sizeof(live) / sizeof(live[0]); i++)
  {
    const prov_step_t r = prov_session_step(sess(live[i], false), PROV_IN_CRED_RECEIVED);
    TEST_ASSERT_EQUAL_INT(PROV_ACT_EMIT_CRED_RECEIVED, r.action);
    TEST_ASSERT_EQUAL_INT(live[i], r.next.phase); // reporting it moves nothing
  }
}

// The pairing that made the dropped credential unrecoverable: anything that can latch an outcome
// must also be able to report the credential that earned it.
static void test_prov_session_success_is_always_preceded_by_a_reportable_credential(void)
{
  const prov_phase_t live[] = {PROV_PHASE_STARTING, PROV_PHASE_ADVERTISING, PROV_PHASE_STOPPING};
  for (size_t i = 0; i < sizeof(live) / sizeof(live[0]); i++)
  {
    const prov_session_t base = sess(live[i], false);
    if (prov_session_step(base, PROV_IN_CRED_VERIFIED).next.verified)
    {
      TEST_ASSERT_EQUAL_INT_MESSAGE(PROV_ACT_EMIT_CRED_RECEIVED,
                                    prov_session_step(base, PROV_IN_CRED_RECEIVED).action,
                                    "a phase that latches an outcome must forward the credential");
    }
  }
}

static void test_prov_session_end_without_a_credential_is_cancelled(void)
{
  const prov_step_t r = prov_session_step(sess(PROV_PHASE_ADVERTISING, false), PROV_IN_END);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_IDLE, r.next.phase);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_EMIT_STOPPED, r.action);
}

static void test_prov_session_end_with_a_credential_is_success(void)
{
  prov_step_t r = prov_session_step(sess(PROV_PHASE_ADVERTISING, false), PROV_IN_CRED_VERIFIED);
  TEST_ASSERT_TRUE(r.next.verified);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_NONE, r.action); // latches quietly; END reports it

  r = prov_session_step(r.next, PROV_IN_END);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_IDLE, r.next.phase);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_EMIT_SUCCESS, r.action);
  TEST_ASSERT_FALSE(r.next.verified); // consumed, not left latched for the next session
}

// A cancel racing a successful verification: the credential still wins. Preserves the behaviour of
// the old s_cred_ok latch, which s_stopping never cleared.
static void test_prov_session_verified_outcome_survives_a_racing_stop(void)
{
  prov_step_t r = prov_session_step(sess(PROV_PHASE_ADVERTISING, true), PROV_IN_STOP_REQUESTED);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_STOPPING, r.next.phase);
  TEST_ASSERT_TRUE(r.next.verified);

  r = prov_session_step(r.next, PROV_IN_END);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_EMIT_SUCCESS, r.action);
}

static void test_prov_session_verification_still_latches_while_stopping(void)
{
  const prov_step_t r = prov_session_step(sess(PROV_PHASE_STOPPING, false), PROV_IN_CRED_VERIFIED);
  TEST_ASSERT_TRUE(r.next.verified);
}

// The bug this suppression exists for: the manager can raise CRED_FAIL as it tears a session down,
// and the app answers a failure by erasing the stored WiFi credential and re-showing the QR.
static void test_prov_session_credential_failure_is_suppressed_while_stopping(void)
{
  prov_step_t r = prov_session_step(sess(PROV_PHASE_ADVERTISING, false), PROV_IN_CRED_FAILED);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_EMIT_FAILED, r.action); // a real failure, reported

  r = prov_session_step(sess(PROV_PHASE_STOPPING, false), PROV_IN_CRED_FAILED);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_SUPPRESS, r.action);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_STOPPING, r.next.phase);
}

// A wrong-password retry keeps advertising so the phone can retry over the same BLE link.
static void test_prov_session_failure_keeps_advertising(void)
{
  const prov_step_t r = prov_session_step(sess(PROV_PHASE_ADVERTISING, false), PROV_IN_CRED_FAILED);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_ADVERTISING, r.next.phase);
}

static void test_prov_session_credentials_are_forwarded_while_advertising(void)
{
  const prov_step_t r = prov_session_step(sess(PROV_PHASE_ADVERTISING, false), PROV_IN_CRED_RECEIVED);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_EMIT_CRED_RECEIVED, r.action);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_ADVERTISING, r.next.phase);
}

// The manager can end a session that never advertised: a start that unwound, or the deinit that
// ble_provisioning_stop() issues for exactly that case. Without this the phase stuck at STARTING
// and no later input could clear it — the device would refuse to provision until reboot.
static void test_prov_session_end_while_starting_resolves_the_session(void)
{
  const prov_step_t r = prov_session_step(sess(PROV_PHASE_STARTING, false), PROV_IN_END);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_IDLE, r.next.phase);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_EMIT_STOPPED, r.action);
}

// Every phase that can be entered must have a route back to IDLE, or a session can wedge.
static void test_prov_session_every_live_phase_resolves_on_end(void)
{
  const prov_phase_t live[] = {PROV_PHASE_STARTING, PROV_PHASE_ADVERTISING, PROV_PHASE_STOPPING};
  for (size_t i = 0; i < sizeof(live) / sizeof(live[0]); i++)
  {
    const prov_step_t r = prov_session_step(sess(live[i], false), PROV_IN_END);
    TEST_ASSERT_EQUAL_INT(PROV_PHASE_IDLE, r.next.phase);
    TEST_ASSERT_EQUAL_INT(PROV_ACT_EMIT_STOPPED, r.action);
  }
}

static void test_prov_session_stop_from_starting_is_a_stop(void)
{
  // Narrow but real: a cancel landing between mgr_init() and the service coming up.
  prov_step_t r = prov_session_step(sess(PROV_PHASE_STARTING, false), PROV_IN_STOP_REQUESTED);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_STOPPING, r.next.phase);

  r = prov_session_step(r.next, PROV_IN_END);
  TEST_ASSERT_EQUAL_INT(PROV_ACT_EMIT_STOPPED, r.action);
}

// Property, not a case: nothing may report a completed session without an outcome latched first.
// Walks every reachable state against every input to depth 4.
static void walk_no_unearned_success(prov_session_t s, int depth)
{
  if (depth == 0)
  {
    return;
  }
  for (size_t i = 0; i < N_INPUTS; i++)
  {
    const prov_step_t r = prov_session_step(s, k_all_inputs[i]);
    if (r.action == PROV_ACT_EMIT_SUCCESS)
    {
      TEST_ASSERT_TRUE_MESSAGE(s.verified, "EMIT_SUCCESS from a session with no verified credential");
    }
    walk_no_unearned_success(r.next, depth - 1);
  }
}

static void test_prov_session_never_reports_success_without_a_verified_credential(void)
{
  walk_no_unearned_success(sess(PROV_PHASE_IDLE, false), 4);
}

// Mirror property: the terminal phase is terminal. Nothing may leave it, and nothing may emit from
// it — a device whose BLE controller memory is gone must not look startable.
static void test_prov_session_unavailable_is_terminal(void)
{
  for (size_t i = 0; i < N_INPUTS; i++)
  {
    const prov_step_t r = prov_session_step(sess(PROV_PHASE_UNAVAILABLE, false), k_all_inputs[i]);
    TEST_ASSERT_EQUAL_INT(PROV_PHASE_UNAVAILABLE, r.next.phase);
    TEST_ASSERT_EQUAL_INT(PROV_ACT_NONE, r.action);
  }
}

static void test_prov_session_memory_release_is_reachable_from_any_phase(void)
{
  for (size_t i = 0; i < N_PHASES; i++)
  {
    const prov_step_t r = prov_session_step(sess(k_all_phases[i], false), PROV_IN_MEM_RELEASED);
    TEST_ASSERT_EQUAL_INT(PROV_PHASE_UNAVAILABLE, r.next.phase);
  }
}

// Totality: every phase accepts every input without wandering off the enum, so adding a phase or
// an input cannot silently leave a hole in the table.
static void test_prov_session_is_total(void)
{
  for (size_t p = 0; p < N_PHASES; p++)
  {
    for (size_t v = 0; v < 2; v++)
    {
      for (size_t i = 0; i < N_INPUTS; i++)
      {
        const prov_step_t r = prov_session_step(sess(k_all_phases[p], v != 0), k_all_inputs[i]);
        TEST_ASSERT_TRUE(r.next.phase >= PROV_PHASE_IDLE && r.next.phase <= PROV_PHASE_UNAVAILABLE);
        TEST_ASSERT_TRUE(r.action >= PROV_ACT_NONE && r.action <= PROV_ACT_SUPPRESS);
      }
    }
  }
}

// ---- holds_radio ----------------------------------------------------------
//
// The predicate the WiFi reconnect timer checks before starting the radio (#98). It is a property
// of the session model, not of app_handlers: deriving it at the call site is how UNAVAILABLE got
// read as "BLE still owns the radio", which is the exact inversion of what it means.

static void test_prov_session_holds_radio_only_while_a_session_is_live(void)
{
  TEST_ASSERT_TRUE(prov_session_holds_radio(PROV_PHASE_STARTING));
  TEST_ASSERT_TRUE(prov_session_holds_radio(PROV_PHASE_ADVERTISING));
  TEST_ASSERT_TRUE(prov_session_holds_radio(PROV_PHASE_STOPPING));
  TEST_ASSERT_FALSE(prov_session_holds_radio(PROV_PHASE_IDLE));
}

// The case that carries the weight: UNAVAILABLE means the BLE controller memory has been released,
// so the radio belongs to WiFi and always will. Reporting true here strands the device offline
// after the first post-provisioning disconnect, with only a reboot to recover it.
static void test_prov_session_holds_radio_is_false_once_memory_is_released(void)
{
  TEST_ASSERT_FALSE(prov_session_holds_radio(PROV_PHASE_UNAVAILABLE));

  const prov_step_t r = prov_session_step(sess(PROV_PHASE_ADVERTISING, true), PROV_IN_MEM_RELEASED);
  TEST_ASSERT_EQUAL_INT(PROV_PHASE_UNAVAILABLE, r.next.phase);
  TEST_ASSERT_FALSE(prov_session_holds_radio(r.next.phase));
}

// Every phase a live session can be observed in must claim the radio, and every phase it cannot
// must not. Pinned against the machine itself so a new phase cannot be added without a verdict.
static void test_prov_session_holds_radio_agrees_with_the_live_phases(void)
{
  const prov_phase_t live[] = {PROV_PHASE_STARTING, PROV_PHASE_ADVERTISING, PROV_PHASE_STOPPING};
  for (size_t i = 0; i < sizeof(live) / sizeof(live[0]); i++)
  {
    // A live phase is exactly one a START_BEGUN can reset rather than one END has resolved.
    const prov_step_t r = prov_session_step(sess(live[i], false), PROV_IN_END);
    TEST_ASSERT_EQUAL_INT(PROV_PHASE_IDLE, r.next.phase);
    TEST_ASSERT_TRUE(prov_session_holds_radio(live[i]));
    TEST_ASSERT_FALSE(prov_session_holds_radio(r.next.phase));
  }
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
  RUN_TEST(test_prov_session_start_reaches_advertising);
  RUN_TEST(test_prov_session_start_failure_returns_to_idle);
  RUN_TEST(test_prov_session_start_ok_never_resurrects_a_finished_session);
  RUN_TEST(test_prov_session_start_clears_a_previous_outcome);
  RUN_TEST(test_prov_session_start_resets_any_live_phase);
  RUN_TEST(test_prov_session_credential_is_never_dropped);
  RUN_TEST(test_prov_session_success_is_always_preceded_by_a_reportable_credential);
  RUN_TEST(test_prov_session_end_without_a_credential_is_cancelled);
  RUN_TEST(test_prov_session_end_with_a_credential_is_success);
  RUN_TEST(test_prov_session_verified_outcome_survives_a_racing_stop);
  RUN_TEST(test_prov_session_verification_still_latches_while_stopping);
  RUN_TEST(test_prov_session_credential_failure_is_suppressed_while_stopping);
  RUN_TEST(test_prov_session_failure_keeps_advertising);
  RUN_TEST(test_prov_session_credentials_are_forwarded_while_advertising);
  RUN_TEST(test_prov_session_stop_from_starting_is_a_stop);
  RUN_TEST(test_prov_session_end_while_starting_resolves_the_session);
  RUN_TEST(test_prov_session_every_live_phase_resolves_on_end);
  RUN_TEST(test_prov_session_never_reports_success_without_a_verified_credential);
  RUN_TEST(test_prov_session_unavailable_is_terminal);
  RUN_TEST(test_prov_session_memory_release_is_reachable_from_any_phase);
  RUN_TEST(test_prov_session_is_total);
  RUN_TEST(test_prov_session_holds_radio_only_while_a_session_is_live);
  RUN_TEST(test_prov_session_holds_radio_is_false_once_memory_is_released);
  RUN_TEST(test_prov_session_holds_radio_agrees_with_the_live_phases);
  return UNITY_END();
}
