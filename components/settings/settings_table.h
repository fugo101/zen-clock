// SPDX-License-Identifier: MIT
// ZenClock — settings descriptor table
//
// The single owner of every persisted setting's NVS key, default, valid range and boolean
// polarity. Deliberately free of ESP-IDF and LVGL headers so it builds for the host-side
// `[env:native]` tests — it is symlinked into test/test_pure_logic/. Nothing here may include
// nvs.h, esp_log.h or lvgl.h.
//
// What this table does NOT own, on purpose:
//   - Display labels, edit step, unit and row order      -> settings_row_t / s_items[] (ui)
//   - The hardware effect of a change                    -> s_apply[] (ui) and app_main (boot)
//   - Flash access                                       -> settings.c
// A label is a display string and a step is a button-press granularity; neither is a domain
// rule, and putting them here would make this table un-testable for the sake of one less file.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Lowest brightness the device will ever apply or store, in percent.
 *
 * The backlight is the only way to read the settings screen, so 0% is not a valid setting —
 * it hides the control needed to undo it. Enforced on both read and write via the descriptor's
 * `.min`, and used as the `.min` of the Brightness item in the UI so the two cannot drift apart.
 */
#define SETTINGS_BRIGHTNESS_MIN 10

/**
 * @brief Inclusive bounds of the stored timezone offset, in whole hours from UTC.
 *
 * Spans the real-world range (UTC-12 at Baker Island through UTC+14 at Line Islands). Whole
 * hours only — see ADR-0004 for why half-hour zones are out of scope.
 */
#define SETTINGS_TZ_MIN (-12)
#define SETTINGS_TZ_MAX 14

  /**
   * @brief Identifies one persisted setting.
   *
   * Deliberately separate from `settings_row_t` (components/ui/settings_screen.h): 8 of that
   * enum's 16 entries are section headers and action rows with no key, default or range.
   * `settings_row_t` remains the single source of truth for row order (ADR-0004); the screen's
   * row table points into this enum with a `.skey` field rather than restating the list.
   *
   * `SETTINGS_KEY_NONE` is 0 so it is the natural value of an un-set `.skey`, and the real keys
   * start at 1 so the enum indexes the descriptor array directly. Order follows the visible row
   * order, so the two tables can be read side by side during review.
   */
  typedef enum
  {
    SETTINGS_KEY_NONE = 0,
    SETTINGS_KEY_THEME_LIGHT,
    SETTINGS_KEY_BRIGHTNESS,
    SETTINGS_KEY_TIME_FMT,
    SETTINGS_KEY_SHOW_SECS,
    SETTINGS_KEY_TZ_OFFSET,
    SETTINGS_KEY_SLEEP_H,
    SETTINGS_KEY_SLEEP_M,
    SETTINGS_KEY_SLEEP_S,
    SETTINGS_KEY_COUNT,
  } settings_key_t;

  /** Everything the persistence layer needs to know about one setting. */
  typedef struct
  {
    /** NVS key name. Max 15 characters — NVS rejects longer ones at runtime only. */
    const char *nvs_key;
    /** Value used when NVS has nothing stored, or cannot be opened. Always within min..max. */
    int8_t def;
    int8_t min;
    int8_t max;
    /**
     * @brief True when the UI's option index is the inverse of the stored boolean.
     *
     * Time Format's options are {24H, 12H} and Show Secs' are {On, Off}, so index 0 means the
     * stored bool is true; Theme's are {Dark, Light}, so index 0 means false. This flag replaces
     * three hand-written `== 0` comparisons per field that the compiler could not check.
     * Meaningless for non-boolean settings, where it is false.
     */
    bool invert;
  } settings_desc_t;

  /**
   * @brief Look up a setting's descriptor.
   * @return NULL for SETTINGS_KEY_NONE and for any value outside the enum.
   */
  const settings_desc_t *settings_desc(settings_key_t key);

  /**
   * @brief Clamp a value into its setting's valid range.
   *
   * Applied on read as well as on write: read-clamping is what recovers a device that already
   * has an out-of-range value on flash (ADR-0004).
   *
   * @return The clamped value, or 0 for an unknown key.
   */
  int8_t settings_clamp(settings_key_t key, int val);

  /**
   * @brief Convert between a stored boolean and the UI option index, in either direction.
   *
   * Involutive — applying it twice returns the original — which is why the load and persist
   * paths can share one mapping instead of two independently-written inversions.
   */
  int settings_option_index(settings_key_t key, int val);

  /** @brief Total auto-sleep timeout in seconds from its three components. */
  uint32_t settings_sleep_seconds(int hours, int minutes, int seconds);

#ifdef __cplusplus
}
#endif
