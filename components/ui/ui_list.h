// SPDX-License-Identifier: MIT
// ZenClock — Shared LVGL helpers for scrollable label-value lists

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

  // Layout constants shared by every screen that renders a title + scrollable row list
  // (settings_screen.c, device_info_screen.c, menu_screen.c). Values are identical across all
  // three; menu_screen.c has no value column so it simply doesn't reference VALUE_X_RIGHT.
#define TITLE_Y       24
#define LIST_Y_START  50
#define LIST_ITEM_H   24
#define LIST_X_PAD    16
#define VALUE_X_RIGHT (-12)

  /**
   * @brief Show/hide and reposition a scrollable row list's name/value labels based on the
   * current scroll offset.
   *
   * Only handles rendering (which rows are visible and where). Scroll-position policy is each
   * caller's own responsibility — settings_screen.c clamps directly in its focus handlers,
   * device_info_screen.c wraps via modulus — this function doesn't need to know which.
   *
   * @param name_labels  Array of `count` name label pointers; entries may be NULL (e.g. a header
   *                      row's value label).
   * @param value_labels Array of `count` value label pointers; entries may be NULL.
   * @param count        Total row count (array length of both label arrays).
   * @param visible      Number of rows visible at once.
   * @param scroll       Index of the first visible row.
   */
  void ui_apply_scroll(lv_obj_t **name_labels, lv_obj_t **value_labels, int count, int visible, int scroll);

  /** Delete an lv_timer_t if non-NULL and null the caller's handle. Safe to call on a NULL handle. */
  void ui_timer_delete(lv_timer_t **timer);

#ifdef __cplusplus
}
#endif
