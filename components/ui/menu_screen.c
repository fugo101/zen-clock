// SPDX-License-Identifier: MIT
// ZenClock — Menu screen (vertical list of features)
//
// Layout (320×170):
//   Status bar (top, managed externally)
//   Title "Menu" (y=24)
//   Item list starting at y=50, each item 24px tall

#include "menu_screen.h"
#include "ui_utils.h"
#include "ui_list.h"

// ============================================================
// Menu items — only implemented features appear here
// ============================================================
static const char *s_menu_labels[] = {
    "Settings",
    "System Info",
};
#define MENU_ITEM_COUNT (sizeof(s_menu_labels) / sizeof(s_menu_labels[0]))

// Layout constants (TITLE_Y, LIST_Y_START, LIST_ITEM_H, LIST_X_PAD) come from ui_list.h — shared
// with settings_screen.c and device_info_screen.c, which use identical values. Menu has no value
// column, so it doesn't reference VALUE_X_RIGHT.

// ============================================================
// Private state
// ============================================================
static int s_focus = 0;
static lv_obj_t *s_item_labels[MENU_ITEM_COUNT] = {NULL};
static lv_obj_t *s_focus_marker = NULL;

// ============================================================
// Visual helpers
// ============================================================
static void update_focus_visual(void)
{
  // Position the ▸ marker
  if (s_focus_marker)
  {
    lv_obj_set_y(s_focus_marker, LIST_Y_START + s_focus * LIST_ITEM_H);
  }

  // Highlight focused, dim others
  for (int i = 0; i < (int) MENU_ITEM_COUNT; i++)
  {
    if (!s_item_labels[i])
    {
      continue;
    }

    if (i == s_focus)
    {
      lv_obj_set_style_text_opa(s_item_labels[i], LV_OPA_COVER, 0);
      lv_obj_set_style_text_color(s_item_labels[i], lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
    }
    else
    {
      lv_obj_set_style_text_opa(s_item_labels[i], LV_OPA_70, 0);
      lv_obj_remove_local_style_prop(s_item_labels[i], LV_STYLE_TEXT_COLOR, 0);
    }
  }
}

// ============================================================
// Public API
// ============================================================
void menu_screen_create(lv_obj_t *parent)
{
  s_focus = 0;

  // Title
  lv_obj_t *title = lv_label_create(parent);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_label_set_text(title, "Menu");
  lv_obj_set_pos(title, LIST_X_PAD, TITLE_Y);

  // Focus marker ▸
  s_focus_marker = lv_label_create(parent);
  lv_obj_set_style_text_font(s_focus_marker, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_focus_marker, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
  lv_label_set_text(s_focus_marker, LV_SYMBOL_RIGHT);
  lv_obj_set_pos(s_focus_marker, LIST_X_PAD, LIST_Y_START);

  // Menu items
  for (int i = 0; i < (int) MENU_ITEM_COUNT; i++)
  {
    s_item_labels[i] = lv_label_create(parent);
    lv_obj_set_style_text_font(s_item_labels[i], &lv_font_montserrat_14, 0);
    lv_label_set_text(s_item_labels[i], s_menu_labels[i]);
    lv_obj_set_pos(s_item_labels[i], LIST_X_PAD + 18, LIST_Y_START + i * LIST_ITEM_H);
  }

  update_focus_visual();
}

void menu_screen_destroy(void)
{
  // Deliberately does NOT reset s_focus — nav.c owns persisting/restoring it across screen
  // switches via s_menu_focus, and calls menu_screen_set_focus() right after every
  // show_menu_screen(). Clearing it here would race with that restore.
  for (int i = 0; i < (int) MENU_ITEM_COUNT; i++)
  {
    s_item_labels[i] = NULL;
  }
  s_focus_marker = NULL;
}

void menu_screen_focus_prev(void)
{
  s_focus = ui_circ_prev(s_focus, (int) MENU_ITEM_COUNT);
  update_focus_visual();
}

void menu_screen_focus_next(void)
{
  s_focus = ui_circ_next(s_focus, (int) MENU_ITEM_COUNT);
  update_focus_visual();
}

int menu_screen_get_focus(void)
{
  return s_focus;
}

void menu_screen_set_focus(int index)
{
  if (index >= 0 && index < (int) MENU_ITEM_COUNT)
  {
    s_focus = index;
    update_focus_visual();
  }
}
