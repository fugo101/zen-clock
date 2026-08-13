// SPDX-License-Identifier: MIT
// ZenClock — Menu screen widget interface

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Menu row indices. The single source of truth for which row does what — previously
   * `nav.c` matched `s_menu_focus` against raw literals `0`/`1` with no name tying them to
   * `s_menu_labels[]` in menu_screen.c.
   */
  typedef enum
  {
    MENU_ROW_SETTINGS,
    MENU_ROW_DEVICE_INFO,
  } menu_row_t;

  /** Create menu screen content on the given parent (below status bar). */
  void menu_screen_create(lv_obj_t *parent);

  /** Null out widget pointers. Call before the parent is deleted. */
  void menu_screen_destroy(void);

  /** Move focus to previous item. Stops at first. */
  void menu_screen_focus_prev(void);

  /** Move focus to next item. Stops at last. */
  void menu_screen_focus_next(void);

  /** Get current focus index. */
  int menu_screen_get_focus(void);

  /** Set focus to a specific index (for restoring position). */
  void menu_screen_set_focus(int index);

#ifdef __cplusplus
}
#endif
