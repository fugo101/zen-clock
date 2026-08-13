// SPDX-License-Identifier: MIT
// ZenClock — Settings screen widget interface

#pragma once

#include "lvgl.h"
#include "nav.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Canonical settings row layout — the single source of truth for row indices.
   *
   * Previously this list existed as up to 6 independently-maintained encodings (named defines
   * covering only the action rows, two separate bare `case N:` ladders, a literal NVS-load list,
   * and a doc-comment table) — reordering the table meant hunting down every one of them.
   * `SETTINGS_ROW_COUNT` doubles as the row-array bound.
   */
  typedef enum
  {
    SETTINGS_ROW_DISPLAY_HEADER,
    SETTINGS_ROW_THEME,
    SETTINGS_ROW_BRIGHTNESS,
    SETTINGS_ROW_CLOCK_HEADER,
    SETTINGS_ROW_TIME_FORMAT,
    SETTINGS_ROW_SHOW_SECS,
    SETTINGS_ROW_TIMEZONE,
    SETTINGS_ROW_SLEEP_HEADER,
    SETTINGS_ROW_SLEEP_H,
    SETTINGS_ROW_SLEEP_M,
    SETTINGS_ROW_SLEEP_S,
    SETTINGS_ROW_SLEEP_NOW,
    SETTINGS_ROW_NETWORK_HEADER,
    SETTINGS_ROW_NTP_RESYNC,
    SETTINGS_ROW_RESET_WIFI,
    SETTINGS_ROW_PROVISIONING,
    SETTINGS_ROW_COUNT,
  } settings_row_t;

  /** Create settings screen content on the given parent. */
  void settings_screen_create(lv_obj_t *parent);

  /** Flush any pending NVS write and null out widget pointers. Call before the parent is deleted. */
  void settings_screen_destroy(void);

  void settings_screen_focus_prev(void);
  void settings_screen_focus_next(void);
  int settings_screen_get_focus(void);
  void settings_screen_set_focus(int index);

  /** Check if item at index is an action (not editable). */
  bool settings_screen_is_action_item(int index);

  /**
   * @brief Resolve an action item (e.g., Reset WiFi) to the callback that should run.
   *
   * Deliberately does *not* invoke the callback: these block for seconds and must run
   * outside the LVGL lock. See nav_handle_action().
   *
   * @param index   Item index
   * @param cb      External callback (from app_handlers via nav)
   * @return `cb` when `index` is a valid action item, NULL otherwise.
   */
  nav_action_cb_t settings_screen_resolve_action(int index, nav_action_cb_t cb);

  /** Enter edit mode for the focused item. */
  void settings_screen_enter_edit(int index);

  /** Exit edit mode. */
  void settings_screen_exit_edit(void);

  /** Increase/next value in edit mode. */
  void settings_screen_edit_increase(void);

  /** Decrease/prev value in edit mode. */
  void settings_screen_edit_decrease(void);

#ifdef __cplusplus
}
#endif
