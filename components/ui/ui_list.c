// SPDX-License-Identifier: MIT
// ZenClock — Shared LVGL helpers for scrollable label-value lists

#include "ui_list.h"

void ui_apply_scroll(
    lv_obj_t **name_labels, lv_obj_t **value_labels, const int count, const int visible, const int scroll)
{
  for (int i = 0; i < count; i++)
  {
    const bool visible_row = (i >= scroll && i < scroll + visible);
    const int y = LIST_Y_START + (i - scroll) * LIST_ITEM_H;

    if (name_labels[i])
    {
      lv_obj_set_y(name_labels[i], y);
      if (visible_row)
      {
        lv_obj_remove_flag(name_labels[i], LV_OBJ_FLAG_HIDDEN);
      }
      else
      {
        lv_obj_add_flag(name_labels[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (value_labels[i])
    {
      lv_obj_set_y(value_labels[i], y);
      if (visible_row)
      {
        lv_obj_remove_flag(value_labels[i], LV_OBJ_FLAG_HIDDEN);
      }
      else
      {
        lv_obj_add_flag(value_labels[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
}

void ui_timer_delete(lv_timer_t **timer)
{
  if (*timer)
  {
    lv_timer_delete(*timer);
    *timer = NULL;
  }
}
