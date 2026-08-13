// SPDX-License-Identifier: MIT
// ZenClock — Text-based clock face (DS-Digital 48, per-digit fixed-width containers)
//
// Colors follow the global theme (ui_is_light_theme):
//   light: digit #333333, date #666666, background handled by screen (nav.c)
//   dark:  digit #01ddff, date #007a99, background handled by screen (nav.c)
//
// Layout (with seconds):   H1(25)+H2(25)+:(12)+M1(25)+M2(25)+:(12)+S1(25)+S2(25) = 174px
// Layout (without seconds): H1(25)+H2(25)+:(12)+M1(25)+M2(25) = 112px
// Each digit is one character, right-aligned in its own 25px container — no jitter on change.
// DS-Digital adv_w: 391/16 = 24.4px → DIGIT_W=25 (0.6px margin per digit)
// Colon adv_w: 170/16 = 10.6px → COLON_W=12 (1.4px margin)
//
// AM/PM label: DS-Digital 16px, bottom-aligned right of time row (12H mode only).

#include "clock_face.h"
#include "ui.h"
#include "ui_list.h"
#include "settings.h"
#include "fonts/lv_font_ds_digital_48.h"
#include "fonts/lv_font_ds_digital_16.h"

#include <time.h>
#include <sys/time.h>

#define DIGIT_W 25 // single digit
#define COLON_W 12

static lv_obj_t *s_time_row = NULL;
static lv_obj_t *s_digits[6]; // [0]=H1 [1]=H2 [2]=M1 [3]=M2 [4]=S1 [5]=S2
static lv_obj_t *s_date_label = NULL;
static lv_obj_t *s_ampm_label = NULL;
static lv_timer_t *s_clock_timer = NULL;

// ============================================================
// Timer callback — runs inside lv_timer_handler() on LVGL task
// ============================================================
static void clock_timer_cb(lv_timer_t *timer) // NOLINT(readability-non-const-parameter)
{
  (void) timer;

  time_t now = time(NULL);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char buf[2] = {'\0', '\0'};

  // Derived from the layout rather than re-read from NVS: an nvs_open/get/close here cost a
  // flash read 86 400 times a day on the LVGL task. The AM/PM label exists only in 12H mode,
  // so it already carries the answer — the same trick s_digits[4] uses for show_seconds below.
  // Changing the format re-creates the clock screen (nav.c), which re-reads the setting.
  const bool is_24h = (s_ampm_label == NULL);
  int h = is_24h ? timeinfo.tm_hour : (timeinfo.tm_hour % 12 != 0 ? timeinfo.tm_hour % 12 : 12);

  buf[0] = (char) ('0' + h / 10);
  lv_label_set_text(s_digits[0], buf);
  buf[0] = (char) ('0' + h % 10);
  lv_label_set_text(s_digits[1], buf);

  buf[0] = (char) ('0' + timeinfo.tm_min / 10);
  lv_label_set_text(s_digits[2], buf);
  buf[0] = (char) ('0' + timeinfo.tm_min % 10);
  lv_label_set_text(s_digits[3], buf);

  if (s_digits[4])
  {
    buf[0] = (char) ('0' + timeinfo.tm_sec / 10);
    lv_label_set_text(s_digits[4], buf);
    buf[0] = (char) ('0' + timeinfo.tm_sec % 10);
    lv_label_set_text(s_digits[5], buf);
  }

  char datebuf[12];
  (void) strftime(datebuf, sizeof(datebuf), "%d/%m/%Y", &timeinfo);
  lv_label_set_text(s_date_label, datebuf);

  if (s_ampm_label)
  {
    (void) strftime(datebuf, sizeof(datebuf), "%p", &timeinfo);
    lv_label_set_text(s_ampm_label, datebuf);
  }
}

// ============================================================
// Helper — create one fixed-width digit or symbol label
// ============================================================
static lv_obj_t *make_segment(lv_obj_t *parent, int32_t w, const char *init, lv_color_t color)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_width(label, w);
  lv_obj_set_height(label, LV_SIZE_CONTENT);
  lv_obj_set_style_text_font(label, &lv_font_ds_digital_48, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_label_set_text(label, init);
  return label;
}

// ============================================================
// Public API
// ============================================================
void clock_face_create(lv_obj_t *parent)
{
  const bool show_secs = settings_get_show_seconds();
  const bool is_24h = settings_get_time_format_24h();

  bool light = ui_is_light_theme();
  lv_color_t digit_color = light ? lv_color_hex(0x333333) : lv_color_hex(0x01ddff);
  lv_color_t date_color = light ? lv_color_hex(0x666666) : lv_color_hex(0x007a99);

  int32_t row_w = show_secs ? (DIGIT_W * 6 + COLON_W * 2) : (DIGIT_W * 4 + COLON_W);

  s_time_row = lv_obj_create(parent);
  lv_obj_remove_style_all(s_time_row);
  lv_obj_set_size(s_time_row, row_w, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_time_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_time_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(s_time_row, 0, 0);
  lv_obj_set_style_pad_column(s_time_row, 0, 0);
  lv_obj_align(s_time_row, LV_ALIGN_CENTER, 0, -12);

  s_digits[0] = make_segment(s_time_row, DIGIT_W, "0", digit_color); // H1
  s_digits[1] = make_segment(s_time_row, DIGIT_W, "0", digit_color); // H2
  make_segment(s_time_row, COLON_W, ":", digit_color);
  s_digits[2] = make_segment(s_time_row, DIGIT_W, "0", digit_color); // M1
  s_digits[3] = make_segment(s_time_row, DIGIT_W, "0", digit_color); // M2

  if (show_secs)
  {
    make_segment(s_time_row, COLON_W, ":", digit_color);
    s_digits[4] = make_segment(s_time_row, DIGIT_W, "0", digit_color); // S1
    s_digits[5] = make_segment(s_time_row, DIGIT_W, "0", digit_color); // S2
  }
  else
  {
    s_digits[4] = NULL;
    s_digits[5] = NULL;
  }

  // Date label below the time row
  s_date_label = lv_label_create(parent);
  lv_obj_set_width(s_date_label, LV_PCT(100));
  lv_obj_set_height(s_date_label, LV_SIZE_CONTENT);
  lv_obj_set_style_text_align(s_date_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(s_date_label, date_color, 0);
  lv_obj_align_to(s_date_label, s_time_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_label_set_text(s_date_label, "--/--/----");

  // AM/PM label — only in 12H mode, right of time row on same baseline
  if (!is_24h)
  {
    s_ampm_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_ampm_label, &lv_font_ds_digital_16, 0);
    lv_obj_set_style_text_color(s_ampm_label, date_color, 0);
    lv_obj_align_to(s_ampm_label, s_time_row, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 0);
    lv_label_set_text(s_ampm_label, "");
  }
  else
  {
    s_ampm_label = NULL;
  }

  s_clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
  lv_timer_ready(s_clock_timer);
}

void clock_face_destroy(void)
{
  ui_timer_delete(&s_clock_timer);
  s_time_row = NULL;
  s_date_label = NULL;
  s_ampm_label = NULL;
  for (int i = 0; i < 6; i++)
  {
    s_digits[i] = NULL;
  }
}
