// SPDX-License-Identifier: MIT
// ZenClock — Settings screen (vertical list with inline edit mode)
//
// Layout (320×170):
//   Status bar (top, managed externally)
//   Title "Settings" (y=24)
//   Item list: label left, value right, starting at y=50
//   Scrollable: SETTINGS_VISIBLE items visible at a time
//
// Item types:
//   HEADER — non-focusable section separator (dimmed label, no value)
//   TOGGLE — cycles between string options (Theme: Dark/Light, etc.)
//   RANGE  — increments/decrements numeric value (Brightness, Sleep H/M/S, Timezone)
//   ACTION — executes on select, no edit mode (Sleep Now, NTP Resync, Reset Wi-Fi)
//
// Row layout — canonical source is `settings_row_t` in settings_screen.h. 4 headers, 12 items:
//   Display:  Theme, Brightness
//   Clock:    Time Format, Show Secs, Timezone (-12..+14)
//   Sleep:    Sleep H, Sleep M, Sleep S, Sleep Now
//   Network:  NTP Resync, Reset WiFi, Provisioning (re-enter provisioning; keeps the credential)

#include "settings_screen.h"
#include "ui_utils.h"
#include "settings.h"
#include "deep_sleep.h"
#include "bsp.h"
#include "ui.h"

#include <esp_log.h>
#include <stdio.h>

static const char *const tag = "settings_scr";

// ============================================================
// Item definitions
// ============================================================
typedef enum
{
  STYPE_HEADER,
  STYPE_TOGGLE,
  STYPE_RANGE,
  STYPE_ACTION,
} setting_type_t;

typedef struct
{
  const char *label;
  setting_type_t type;
  // Toggle fields
  const char **options;
  int option_count;
  // Range fields
  int min;
  int max;
  int step;
  const char *unit;
  // Current value (working copy)
  int value;
} setting_item_t;

static const char *s_theme_options[] = {"Dark", "Light"};
static const char *s_format_options[] = {"24H", "12H"};
static const char *s_secs_options[] = {"On", "Off"};

#define SETTINGS_VISIBLE 5 // items shown at once (5×24px = 120px <= 170-50=120px)

// Indexed by settings_row_t (settings_screen.h) — order must match the enum exactly.
static setting_item_t s_items[SETTINGS_ROW_COUNT] = {
    {.label = "- Display -", .type = STYPE_HEADER},
    {.label = "Theme", .type = STYPE_TOGGLE, .options = s_theme_options, .option_count = 2},
    {.label = "Brightness", .type = STYPE_RANGE, .min = SETTINGS_BRIGHTNESS_MIN, .max = 100, .step = 10, .unit = "%"},
    {.label = "- Clock -", .type = STYPE_HEADER},
    {.label = "Time Format", .type = STYPE_TOGGLE, .options = s_format_options, .option_count = 2},
    {.label = "Show Secs", .type = STYPE_TOGGLE, .options = s_secs_options, .option_count = 2},
    {.label = "Timezone", .type = STYPE_RANGE, .min = -12, .max = 14, .step = 1, .unit = ""},
    {.label = "- Sleep -", .type = STYPE_HEADER},
    {.label = "Sleep H", .type = STYPE_RANGE, .min = 0, .max = 23, .step = 1, .unit = ""},
    {.label = "Sleep M", .type = STYPE_RANGE, .min = 0, .max = 59, .step = 1, .unit = ""},
    {.label = "Sleep S", .type = STYPE_RANGE, .min = 0, .max = 59, .step = 1, .unit = ""},
    {.label = "Sleep Now", .type = STYPE_ACTION},
    {.label = "- Network -", .type = STYPE_HEADER},
    {.label = "NTP Resync", .type = STYPE_ACTION},
    {.label = "Reset WiFi", .type = STYPE_ACTION},
    {.label = "Provisioning", .type = STYPE_ACTION},
};

// ============================================================
// Layout constants
// ============================================================
#define TITLE_Y       24
#define LIST_Y_START  50
#define LIST_ITEM_H   24
#define LIST_X_PAD    16
#define VALUE_X_RIGHT (-12)

// ============================================================
// Private state
// ============================================================
static int s_focus = 0;
static int s_scroll = 0;
static bool s_editing = false;

static lv_obj_t *s_name_labels[SETTINGS_ROW_COUNT] = {NULL};
static lv_obj_t *s_value_labels[SETTINGS_ROW_COUNT] = {NULL};
static lv_obj_t *s_focus_marker = NULL;
static lv_obj_t *s_edit_box = NULL;

// ============================================================
// Scroll helper
// ============================================================
static void apply_scroll(void)
{
  for (int i = 0; i < SETTINGS_ROW_COUNT; i++)
  {
    bool visible = (i >= s_scroll && i < s_scroll + SETTINGS_VISIBLE);
    int y = LIST_Y_START + (i - s_scroll) * LIST_ITEM_H;
    if (s_name_labels[i])
    {
      lv_obj_set_y(s_name_labels[i], y);
      if (visible)
      {
        lv_obj_remove_flag(s_name_labels[i], LV_OBJ_FLAG_HIDDEN);
      }
      else
      {
        lv_obj_add_flag(s_name_labels[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (s_value_labels[i])
    {
      lv_obj_set_y(s_value_labels[i], y);
      if (visible)
      {
        lv_obj_remove_flag(s_value_labels[i], LV_OBJ_FLAG_HIDDEN);
      }
      else
      {
        lv_obj_add_flag(s_value_labels[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
}

// ============================================================
// Sleep timeout — compute total seconds from working values
// ============================================================
static uint32_t compute_sleep_s(void)
{
  return (uint32_t) s_items[SETTINGS_ROW_SLEEP_H].value * 3600 + (uint32_t) s_items[SETTINGS_ROW_SLEEP_M].value * 60 +
         (uint32_t) s_items[SETTINGS_ROW_SLEEP_S].value;
}

// ============================================================
// Value display helpers
// ============================================================
static void update_value_text(int index)
{
  if (!s_value_labels[index])
  {
    return;
  }

  setting_item_t *item = &s_items[index];
  char buf[16];

  switch (item->type)
  {
  case STYPE_TOGGLE:
    if (item->value >= 0 && item->value < item->option_count)
    {
      lv_label_set_text(s_value_labels[index], item->options[item->value]);
    }
    break;
  case STYPE_RANGE:
    if (item->unit && item->unit[0] != '\0')
    {
      snprintf(buf, sizeof(buf), "%d%s", item->value, item->unit);
    }
    else
    {
      snprintf(buf, sizeof(buf), "%d", item->value);
    }
    lv_label_set_text(s_value_labels[index], buf);
    break;
  case STYPE_HEADER:
  case STYPE_ACTION:
    break;
  }
}

// ============================================================
// Focus visual
// ============================================================
static void update_focus_visual(void)
{
  if (s_focus_marker)
  {
    if (s_editing)
    {
      lv_obj_add_flag(s_focus_marker, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
      lv_obj_remove_flag(s_focus_marker, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_y(s_focus_marker, LIST_Y_START + (s_focus - s_scroll) * LIST_ITEM_H);
    }
  }

  for (int i = 0; i < SETTINGS_ROW_COUNT; i++)
  {
    if (!s_name_labels[i])
    {
      continue;
    }

    if (s_items[i].type == STYPE_HEADER)
    {
      lv_obj_set_style_text_opa(s_name_labels[i], LV_OPA_50, 0);
      continue;
    }

    if (i == s_focus)
    {
      lv_obj_set_style_text_opa(s_name_labels[i], LV_OPA_COVER, 0);
      lv_obj_set_style_text_color(s_name_labels[i], lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
    }
    else
    {
      lv_obj_set_style_text_opa(s_name_labels[i], LV_OPA_70, 0);
      lv_obj_remove_local_style_prop(s_name_labels[i], LV_STYLE_TEXT_COLOR, 0);
    }

    if (s_value_labels[i])
    {
      lv_obj_set_style_text_opa(s_value_labels[i], (i == s_focus) ? LV_OPA_COVER : LV_OPA_70, 0);
    }
  }
}

// ============================================================
// Edit mode visual
// ============================================================
static void show_edit_box(int index)
{
  if (!s_value_labels[index])
  {
    return;
  }

  if (s_edit_box)
  {
    lv_obj_delete(s_edit_box);
    s_edit_box = NULL;
  }

  lv_obj_t *parent = lv_obj_get_parent(s_value_labels[index]);
  s_edit_box = lv_obj_create(parent);
  lv_obj_remove_flag(s_edit_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_edit_box, 80, LIST_ITEM_H);
  lv_obj_set_style_bg_opa(s_edit_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_edit_box, 1, 0);
  lv_obj_set_style_border_color(s_edit_box, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
  lv_obj_set_style_radius(s_edit_box, 4, 0);
  lv_obj_set_style_pad_all(s_edit_box, 0, 0);
  lv_obj_align(s_edit_box, LV_ALIGN_TOP_RIGHT, VALUE_X_RIGHT + 4, LIST_Y_START + (index - s_scroll) * LIST_ITEM_H - 2);

  lv_obj_set_style_text_color(s_value_labels[index], lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
}

static void hide_edit_box(void)
{
  if (s_edit_box)
  {
    lv_obj_delete(s_edit_box);
    s_edit_box = NULL;
  }
}

// ============================================================
// Apply value changes — live preview now, NVS write coalesced
// ============================================================
// Each settings_set_*() is a complete nvs_open/set/commit/close cycle, i.e. a blocking flash
// erase-write. Holding UP on Brightness for two seconds used to trigger about eleven of them,
// all on the LVGL task. The live effect still fires on every single press — only the write is
// deferred, so the screen stays as responsive as before while the flash sees one write instead.
#define NVS_FLUSH_DELAY_MS 1000

static lv_timer_t *s_flush_timer = NULL;
static bool s_dirty[SETTINGS_ROW_COUNT] = {false};

// Everything the user must see immediately. Cheap: RAM, LEDC, or a tzset().
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void apply_live(const int index)
{
  const setting_item_t *item = &s_items[index];

  switch (index)
  {
  case SETTINGS_ROW_THEME:
    ui_set_theme(item->value == 1);
    break;
  case SETTINGS_ROW_BRIGHTNESS:
    bsp_display_set_brightness((uint8_t) item->value, 0);
    break;
  case SETTINGS_ROW_TIMEZONE:
    settings_apply_timezone((int8_t) item->value);
    break;
  case SETTINGS_ROW_SLEEP_H:
  case SETTINGS_ROW_SLEEP_M:
  case SETTINGS_ROW_SLEEP_S:
    deep_sleep_update_timeout(compute_sleep_s());
    break;
  default:
    // Time Format and Show Seconds have no live effect — the clock face reads them when
    // nav re-creates the clock screen, which is exactly what it did before this change.
    break;
  }
}

// The flash side. Only ever called from flush_pending().
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void persist_item(const int index)
{
  const setting_item_t *item = &s_items[index];

  switch (index)
  {
  case SETTINGS_ROW_THEME:
    settings_set_theme_light(item->value == 1);
    ESP_LOGI(tag, "Theme -> %s", item->value == 1 ? "Light" : "Dark");
    break;
  case SETTINGS_ROW_BRIGHTNESS:
    settings_set_brightness((uint8_t) item->value);
    ESP_LOGI(tag, "Brightness -> %d%%", item->value);
    break;
  case SETTINGS_ROW_TIME_FORMAT:
    settings_set_time_format_24h(item->value == 0);
    ESP_LOGI(tag, "Time Format -> %s", item->value == 0 ? "24H" : "12H");
    break;
  case SETTINGS_ROW_SHOW_SECS:
    settings_set_show_seconds(item->value == 0);
    ESP_LOGI(tag, "Show Seconds -> %s", item->value == 0 ? "On" : "Off");
    break;
  case SETTINGS_ROW_TIMEZONE:
    settings_set_timezone_offset((int8_t) item->value);
    ESP_LOGI(tag, "Timezone -> UTC%+d", item->value);
    break;
  case SETTINGS_ROW_SLEEP_H:
    settings_set_sleep_h((uint8_t) item->value);
    ESP_LOGI(tag, "Sleep H -> %d", item->value);
    break;
  case SETTINGS_ROW_SLEEP_M:
    settings_set_sleep_m((uint8_t) item->value);
    ESP_LOGI(tag, "Sleep M -> %d", item->value);
    break;
  case SETTINGS_ROW_SLEEP_S:
    settings_set_sleep_s((uint8_t) item->value);
    ESP_LOGI(tag, "Sleep S -> %d", item->value);
    break;
  default:
    break;
  }
}

static void cancel_flush_timer(void)
{
  if (s_flush_timer)
  {
    lv_timer_delete(s_flush_timer);
    s_flush_timer = NULL;
  }
}

static void flush_pending(void)
{
  cancel_flush_timer();

  for (int i = 0; i < SETTINGS_ROW_COUNT; i++)
  {
    if (s_dirty[i])
    {
      s_dirty[i] = false;
      persist_item(i);
    }
  }
}

static void flush_timer_cb(lv_timer_t *timer) // NOLINT(readability-non-const-parameter)
{
  (void) timer;
  // One-shot: LVGL deletes this timer itself once the repeat count runs out (lv_timer.c:369).
  // Drop our handle first so flush_pending() does not delete it a second time.
  s_flush_timer = NULL;
  flush_pending();
}

static void schedule_flush(void)
{
  if (s_flush_timer)
  {
    lv_timer_reset(s_flush_timer);
    return;
  }

  s_flush_timer = lv_timer_create(flush_timer_cb, NVS_FLUSH_DELAY_MS, NULL);
  if (!s_flush_timer)
  {
    // Out of memory for a timer: write straight through rather than silently losing the setting.
    ESP_LOGW(tag, "No flush timer available — writing settings immediately");
    flush_pending();
    return;
  }
  lv_timer_set_repeat_count(s_flush_timer, 1);
}

static void apply_change(const int index)
{
  apply_live(index);
  s_dirty[index] = true;
  schedule_flush();
}

// ============================================================
// Public API
// ============================================================
void settings_screen_create(lv_obj_t *parent)
{
  s_focus = SETTINGS_ROW_THEME; // first focusable item (skip header at idx 0)
  s_scroll = 0;
  s_editing = false;
  s_edit_box = NULL;

  // Land any deferred write before reading back, otherwise re-entering the screen quickly
  // would load the value the user just replaced. Normally a no-op: exit_edit() already flushed.
  flush_pending();

  // Load current values from NVS
  s_items[SETTINGS_ROW_THEME].value = settings_get_theme_light() ? 1 : 0;
  s_items[SETTINGS_ROW_BRIGHTNESS].value = (int) settings_get_brightness();
  s_items[SETTINGS_ROW_TIME_FORMAT].value = settings_get_time_format_24h() ? 0 : 1;
  s_items[SETTINGS_ROW_SHOW_SECS].value = settings_get_show_seconds() ? 0 : 1;
  s_items[SETTINGS_ROW_TIMEZONE].value = (int) settings_get_timezone_offset();
  s_items[SETTINGS_ROW_SLEEP_H].value = (int) settings_get_sleep_h();
  s_items[SETTINGS_ROW_SLEEP_M].value = (int) settings_get_sleep_m();
  s_items[SETTINGS_ROW_SLEEP_S].value = (int) settings_get_sleep_s();

  // Title
  lv_obj_t *title = lv_label_create(parent);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_label_set_text(title, "Settings");
  lv_obj_set_pos(title, LIST_X_PAD, TITLE_Y);

  // Focus marker
  s_focus_marker = lv_label_create(parent);
  lv_obj_set_style_text_font(s_focus_marker, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_focus_marker, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
  lv_label_set_text(s_focus_marker, LV_SYMBOL_RIGHT);
  lv_obj_set_pos(s_focus_marker, LIST_X_PAD, LIST_Y_START);

  // All item rows
  for (int i = 0; i < SETTINGS_ROW_COUNT; i++)
  {
    int y = LIST_Y_START + i * LIST_ITEM_H;

    s_name_labels[i] = lv_label_create(parent);
    lv_obj_set_style_text_font(s_name_labels[i], &lv_font_montserrat_14, 0);
    lv_label_set_text(s_name_labels[i], s_items[i].label);

    if (s_items[i].type == STYPE_HEADER)
    {
      lv_obj_set_pos(s_name_labels[i], LIST_X_PAD, y);
      lv_obj_set_style_text_opa(s_name_labels[i], LV_OPA_50, 0);
      s_value_labels[i] = NULL;
    }
    else
    {
      lv_obj_set_pos(s_name_labels[i], LIST_X_PAD + 18, y);
      if (s_items[i].type != STYPE_ACTION)
      {
        s_value_labels[i] = lv_label_create(parent);
        lv_obj_set_style_text_font(s_value_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_align(s_value_labels[i], LV_ALIGN_TOP_RIGHT, VALUE_X_RIGHT, y);
        update_value_text(i);
      }
      else
      {
        s_value_labels[i] = NULL;
      }
    }
  }

  apply_scroll();
  update_focus_visual();
}

void settings_screen_destroy(void)
{
  // Land any pending NVS write before the widgets go away. In practice this is already
  // unreachable dead state — every exit from SCR_SETTINGS_EDIT goes through
  // settings_screen_exit_edit(), which already flushes — but that is an emergent invariant, not
  // one this function can rely on staying true. Making the flush explicit here means it stays
  // correct even if a future caller reaches this screen's teardown a different way.
  flush_pending();

  for (int i = 0; i < SETTINGS_ROW_COUNT; i++)
  {
    s_name_labels[i] = NULL;
    s_value_labels[i] = NULL;
  }
  s_focus_marker = NULL;
  hide_edit_box(); // also NULLs s_edit_box
}

void settings_screen_focus_prev(void)
{
  const int start = s_focus;
  // Bounded to SETTINGS_ROW_COUNT steps — ui_circ_prev() is unconditional modular arithmetic with
  // no stopping condition of its own, so a table edit that leaves zero non-header rows would spin
  // this loop forever on the LVGL task (wedged screen, watchdog trip). bugprone-infinite-loop is
  // disabled repo-wide, so static analysis will not flag a regression here — the bound is load-
  // bearing, not decorative.
  int steps = 0;
  do
  {
    s_focus = ui_circ_prev(s_focus, SETTINGS_ROW_COUNT);
    steps++;
  } while (s_items[s_focus].type == STYPE_HEADER && steps < SETTINGS_ROW_COUNT);

  if (s_items[s_focus].type == STYPE_HEADER)
  {
    // No focusable row anywhere in the table — stay put instead of leaving focus on a header.
    s_focus = start;
    return;
  }

  if (s_focus == SETTINGS_ROW_COUNT - 1)
  {
    s_scroll = SETTINGS_ROW_COUNT - SETTINGS_VISIBLE;
  }
  else if (s_focus < s_scroll)
  {
    s_scroll = s_focus;
  }
  apply_scroll();
  update_focus_visual();
}

void settings_screen_focus_next(void)
{
  const int start = s_focus;
  // See settings_screen_focus_prev() — same bounded-loop rationale.
  int steps = 0;
  do
  {
    s_focus = ui_circ_next(s_focus, SETTINGS_ROW_COUNT);
    steps++;
  } while (s_items[s_focus].type == STYPE_HEADER && steps < SETTINGS_ROW_COUNT);

  if (s_items[s_focus].type == STYPE_HEADER)
  {
    // No focusable row anywhere in the table — stay put instead of leaving focus on a header.
    s_focus = start;
    return;
  }

  if (s_focus == SETTINGS_ROW_THEME) // wrapped to first focusable item
  {
    s_scroll = 0;
  }
  else if (s_focus >= s_scroll + SETTINGS_VISIBLE)
  {
    s_scroll = s_focus - SETTINGS_VISIBLE + 1;
  }
  apply_scroll();
  update_focus_visual();
}

int settings_screen_get_focus(void)
{
  return s_focus;
}

void settings_screen_set_focus(int index)
{
  if (index >= 0 && index < SETTINGS_ROW_COUNT && s_items[index].type != STYPE_HEADER)
  {
    s_focus = index;
    if (s_focus < s_scroll)
    {
      s_scroll = s_focus;
    }
    else if (s_focus >= s_scroll + SETTINGS_VISIBLE)
    {
      s_scroll = s_focus - SETTINGS_VISIBLE + 1;
    }
    apply_scroll();
    update_focus_visual();
  }
}

bool settings_screen_is_action_item(int index)
{
  if (index < 0 || index >= SETTINGS_ROW_COUNT)
  {
    return false;
  }
  return s_items[index].type == STYPE_ACTION;
}

nav_action_cb_t settings_screen_resolve_action(int index, nav_action_cb_t cb)
{
  if (index < 0 || index >= SETTINGS_ROW_COUNT)
  {
    return NULL;
  }
  if (s_items[index].type != STYPE_ACTION)
  {
    return NULL;
  }

  // Returned rather than called: the caller runs it once the LVGL lock is released.
  ESP_LOGW(tag, "Action selected: %s", s_items[index].label);
  return cb;
}

void settings_screen_enter_edit(int index)
{
  if (index < 0 || index >= SETTINGS_ROW_COUNT)
  {
    return;
  }
  if (s_items[index].type == STYPE_ACTION || s_items[index].type == STYPE_HEADER)
  {
    return;
  }

  s_editing = true;
  update_focus_visual();
  show_edit_box(index);
  ESP_LOGI(tag, "Edit: %s", s_items[index].label);
}

void settings_screen_exit_edit(void)
{
  s_editing = false;
  hide_edit_box();
  update_focus_visual();

  if (s_value_labels[s_focus])
  {
    lv_obj_remove_local_style_prop(s_value_labels[s_focus], LV_STYLE_TEXT_COLOR, 0);
  }

  // Leaving edit mode is the point at which the user is done adjusting, so don't make them
  // wait out the debounce. This is also the only exit from SCR_SETTINGS_EDIT (nav.c), which
  // makes it the reliable place to guarantee the value reaches flash.
  flush_pending();

  ESP_LOGI(tag, "Edit done");
}

void settings_screen_edit_increase(void)
{
  setting_item_t *item = &s_items[s_focus];

  switch (item->type)
  {
  case STYPE_TOGGLE:
    item->value = (item->value + 1) % item->option_count;
    break;
  case STYPE_RANGE:
    item->value = (item->value + item->step <= item->max) ? item->value + item->step : item->max;
    break;
  default:
    return;
  }

  update_value_text(s_focus);
  apply_change(s_focus);
}

void settings_screen_edit_decrease(void)
{
  setting_item_t *item = &s_items[s_focus];

  switch (item->type)
  {
  case STYPE_TOGGLE:
    item->value = (item->value - 1 + item->option_count) % item->option_count;
    break;
  case STYPE_RANGE:
    item->value = (item->value - item->step >= item->min) ? item->value - item->step : item->min;
    break;
  default:
    return;
  }

  update_value_text(s_focus);
  apply_change(s_focus);
}
