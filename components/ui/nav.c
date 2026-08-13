// SPDX-License-Identifier: MIT
// ZenClock — Navigation state machine
//
// Manages screen transitions: Clock ↔ Menu ↔ Settings (list/edit)
// All functions must be called while holding the LVGL port lock.
// Exception: the callback returned by nav_handle_action() must be run *without* it.

#include "nav.h"
#include "ui.h"
#include "clock_face.h"
#include "status_bar.h"
#include "menu_screen.h"
#include "settings_screen.h"
#include "device_info_screen.h"

#include <esp_log.h>

static const char *const tag = "nav";

// ============================================================
// State
// ============================================================
typedef enum
{
  SCR_CLOCK,
  SCR_MENU,
  SCR_SETTINGS_LIST,
  SCR_SETTINGS_EDIT,
  SCR_DEVICE_INFO,
} screen_state_t;

static screen_state_t s_state = SCR_CLOCK;
static int s_menu_focus = 0;
static int s_settings_focus = 0;

// Action callbacks (registered by app_handlers)
static nav_action_cb_t s_reset_wifi_cb = NULL;
static nav_action_cb_t s_sleep_cb = NULL;
static nav_action_cb_t s_ntp_resync_cb = NULL;
static nav_action_cb_t s_provisioning_cb = NULL;

// ============================================================
// Screen switching helpers
// ============================================================

static void destroy_current_screen(void)
{
  switch (s_state)
  {
  case SCR_CLOCK:
    clock_face_destroy();
    status_bar_destroy();
    break;
  case SCR_MENU:
    menu_screen_destroy();
    status_bar_destroy();
    break;
  case SCR_SETTINGS_LIST:
  case SCR_SETTINGS_EDIT:
    settings_screen_destroy();
    status_bar_destroy();
    break;
  case SCR_DEVICE_INFO:
    device_info_screen_destroy();
    status_bar_destroy();
    break;
  }
}

static void show_clock_screen(void)
{
  destroy_current_screen();

  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  ui_apply_screen_bg(scr);

  clock_face_create(scr);
  status_bar_create(scr);

  lv_obj_t *old = lv_screen_active();
  lv_screen_load(scr);
  if (old)
  {
    lv_obj_delete(old);
  }

  ESP_LOGI(tag, "Screen: Clock");
}

static void show_menu_screen(void)
{
  destroy_current_screen();

  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  ui_apply_screen_bg(scr);

  status_bar_create(scr);
  menu_screen_create(scr);

  lv_obj_t *old = lv_screen_active();
  lv_screen_load(scr);
  if (old)
  {
    lv_obj_delete(old);
  }

  ESP_LOGI(tag, "Screen: Menu");
}

static void show_settings_screen(void)
{
  destroy_current_screen();

  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  ui_apply_screen_bg(scr);

  status_bar_create(scr);
  settings_screen_create(scr);

  lv_obj_t *old = lv_screen_active();
  lv_screen_load(scr);
  if (old)
  {
    lv_obj_delete(old);
  }

  ESP_LOGI(tag, "Screen: Settings");
}

static void show_device_info_screen(void)
{
  destroy_current_screen();

  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  ui_apply_screen_bg(scr);

  status_bar_create(scr);
  device_info_screen_create(scr);

  lv_obj_t *old = lv_screen_active();
  lv_screen_load(scr);
  if (old)
  {
    lv_obj_delete(old);
  }

  ESP_LOGI(tag, "Screen: System Info");
}

// ============================================================
// Public API
// ============================================================

void nav_init(void)
{
  s_state = SCR_CLOCK;
  s_menu_focus = 0;
  s_settings_focus = 0;
  show_clock_screen();
  ESP_LOGI(tag, "Navigation initialized — Clock screen loaded");
}

void nav_register_reset_wifi_cb(nav_action_cb_t cb)
{
  s_reset_wifi_cb = cb;
}

void nav_register_sleep_cb(nav_action_cb_t cb)
{
  s_sleep_cb = cb;
}

void nav_register_ntp_resync_cb(nav_action_cb_t cb)
{
  s_ntp_resync_cb = cb;
}

void nav_register_provisioning_cb(nav_action_cb_t cb)
{
  s_provisioning_cb = cb;
}

nav_action_cb_t nav_handle_action(nav_action_t action)
{
  // Resolved here, run by the caller after the LVGL lock is released — see nav.h.
  nav_action_cb_t deferred = NULL;

  switch (s_state)
  {
  // ========== CLOCK ==========
  case SCR_CLOCK:
    if (action == NAV_ACTION_SELECT)
    {
      show_menu_screen();
      menu_screen_set_focus(s_menu_focus);
      s_state = SCR_MENU;
    }
    // Short presses + BACK: no-op on clock
    break;

  // ========== MENU ==========
  case SCR_MENU:
    if (action == NAV_ACTION_UP)
    {
      menu_screen_focus_prev();
    }
    else if (action == NAV_ACTION_DOWN)
    {
      menu_screen_focus_next();
    }
    else if (action == NAV_ACTION_SELECT)
    {
      s_menu_focus = menu_screen_get_focus();
      if (s_menu_focus == MENU_ROW_SETTINGS)
      {
        show_settings_screen();
        settings_screen_set_focus(s_settings_focus);
        s_state = SCR_SETTINGS_LIST;
      }
      else if (s_menu_focus == MENU_ROW_DEVICE_INFO)
      {
        show_device_info_screen();
        s_state = SCR_DEVICE_INFO;
      }
    }
    else if (action == NAV_ACTION_BACK)
    {
      s_menu_focus = menu_screen_get_focus();
      show_clock_screen();
      s_state = SCR_CLOCK;
    }
    break;

  // ========== SETTINGS LIST ==========
  case SCR_SETTINGS_LIST:
    if (action == NAV_ACTION_UP)
    {
      settings_screen_focus_prev();
    }
    else if (action == NAV_ACTION_DOWN)
    {
      settings_screen_focus_next();
    }
    else if (action == NAV_ACTION_SELECT)
    {
      s_settings_focus = settings_screen_get_focus();
      if (settings_screen_is_action_item(s_settings_focus))
      {
        nav_action_cb_t cb = NULL;
        if (s_settings_focus == SETTINGS_ROW_SLEEP_NOW)
        {
          cb = s_sleep_cb;
        }
        else if (s_settings_focus == SETTINGS_ROW_NTP_RESYNC)
        {
          cb = s_ntp_resync_cb;
        }
        else if (s_settings_focus == SETTINGS_ROW_RESET_WIFI)
        {
          cb = s_reset_wifi_cb;
        }
        else if (s_settings_focus == SETTINGS_ROW_PROVISIONING)
        {
          cb = s_provisioning_cb;
        }
        deferred = settings_screen_resolve_action(s_settings_focus, cb);
      }
      else
      {
        settings_screen_enter_edit(s_settings_focus);
        s_state = SCR_SETTINGS_EDIT;
      }
    }
    else if (action == NAV_ACTION_BACK)
    {
      s_settings_focus = settings_screen_get_focus();
      show_menu_screen();
      menu_screen_set_focus(s_menu_focus);
      s_state = SCR_MENU;
    }
    break;

  // ========== DEVICE INFO ==========
  case SCR_DEVICE_INFO:
    if (action == NAV_ACTION_UP)
    {
      device_info_screen_scroll_up();
    }
    else if (action == NAV_ACTION_DOWN)
    {
      device_info_screen_scroll_down();
    }
    else if (action == NAV_ACTION_BACK)
    {
      show_menu_screen();
      menu_screen_set_focus(s_menu_focus);
      s_state = SCR_MENU;
    }
    break;

  // ========== SETTINGS EDIT ==========
  case SCR_SETTINGS_EDIT:
    if (action == NAV_ACTION_UP)
    {
      settings_screen_edit_increase();
    }
    else if (action == NAV_ACTION_DOWN)
    {
      settings_screen_edit_decrease();
    }
    else if (action == NAV_ACTION_SELECT || action == NAV_ACTION_BACK)
    {
      settings_screen_exit_edit();
      s_state = SCR_SETTINGS_LIST;
    }
    break;
  }

  return deferred;
}
