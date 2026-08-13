// SPDX-License-Identifier: MIT
// ZenClock — Status bar (SNTP indicator + WiFi indicator + battery icon + percentage)

#include "status_bar.h"
#include "bsp.h"

#include <stdio.h>
#include <esp_log.h>

static const char *const tag = "StatusBar";

// ============================================================
// Private state
// ============================================================
static lv_obj_t *s_bat_icon = NULL;
static lv_obj_t *s_bat_pct = NULL;
static lv_obj_t *s_wifi_icon = NULL;
static lv_obj_t *s_sntp_icon = NULL;
static lv_obj_t *s_ts_icon = NULL;
static lv_timer_t *s_bat_timer = NULL;
static lv_timer_t *s_bat_blink_timer = NULL;
static status_bar_battery_cb_t s_battery_cb = NULL;

#define BATT_BLINK_PERIOD_MS 500

// Persist status across screen switches so icons restore correctly on recreate
static wifi_status_t s_last_wifi_status = WIFI_STATUS_DISCONNECTED;
static sntp_status_t s_last_sntp_status = SNTP_STATUS_IDLE;
static ts_status_t s_last_ts_status = TS_STATUS_IDLE;

// ============================================================
// Re-align the status bar chain (right-to-left)
//
// Layout: [TS] [SNTP] [WiFi] [BatIcon] [BatPct]  ← screen edge
// ============================================================
static void realign_chain(void)
{
  // Battery icon stays left of battery percentage
  lv_obj_align_to(s_bat_icon, s_bat_pct, LV_ALIGN_OUT_LEFT_MID, -4, 0);

  // WiFi icon stays left of battery icon
  if (s_wifi_icon)
  {
    lv_obj_align_to(s_wifi_icon, s_bat_icon, LV_ALIGN_OUT_LEFT_MID, -6, 0);
  }

  // SNTP icon stays left of WiFi (only when visible)
  if (s_sntp_icon && s_wifi_icon && !lv_obj_has_flag(s_sntp_icon, LV_OBJ_FLAG_HIDDEN))
  {
    lv_obj_align_to(s_sntp_icon, s_wifi_icon, LV_ALIGN_OUT_LEFT_MID, -6, 0);
  }

  // TS icon: left of SNTP when visible, otherwise left of WiFi
  if (s_ts_icon)
  {
    lv_obj_t *anchor = (s_sntp_icon && !lv_obj_has_flag(s_sntp_icon, LV_OBJ_FLAG_HIDDEN)) ? s_sntp_icon : s_wifi_icon;
    if (anchor)
    {
      lv_obj_align_to(s_ts_icon, anchor, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    }
  }
}

// ============================================================
// Battery blink — separate fast timer, only alive while critical
// ============================================================
static void battery_blink_cb(lv_timer_t *timer) // NOLINT(readability-non-const-parameter)
{
  (void) timer;
  if (lv_obj_has_flag(s_bat_icon, LV_OBJ_FLAG_HIDDEN))
  {
    lv_obj_remove_flag(s_bat_icon, LV_OBJ_FLAG_HIDDEN);
  }
  else
  {
    lv_obj_add_flag(s_bat_icon, LV_OBJ_FLAG_HIDDEN);
  }
}

static void set_battery_blink(bool enabled)
{
  if (enabled && !s_bat_blink_timer)
  {
    s_bat_blink_timer = lv_timer_create(battery_blink_cb, BATT_BLINK_PERIOD_MS, NULL);
  }
  else if (!enabled && s_bat_blink_timer)
  {
    lv_timer_delete(s_bat_blink_timer);
    s_bat_blink_timer = NULL;
    lv_obj_remove_flag(s_bat_icon, LV_OBJ_FLAG_HIDDEN); // don't leave it stuck invisible
  }
}

// ============================================================
// Battery timer callback — runs inside lv_timer_handler()
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void battery_timer_cb(lv_timer_t *timer) // NOLINT(readability-non-const-parameter)
{
  (void) timer;
  char buf[16];

  int pct;
  bool usb;
  bsp_battery_read(NULL, &pct, &usb); // one ADC conversion instead of two
  // Low-battery state never applies on USB power — it is about running out, not about charge
  // level while plugged in.
  const bool low = pct >= 0 && !usb && pct < BATT_LOW_PCT;
  const bool critical = pct >= 0 && !usb && pct < BATT_CRIT_PCT;

  if (pct >= 0)
  {
    // Update percentage text (number only)
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(s_bat_pct, buf);

    // Update icon based on USB / charge level
    if (usb)
    {
      lv_label_set_text(s_bat_icon, LV_SYMBOL_CHARGE);
    }
    else if (pct > 75)
    {
      lv_label_set_text(s_bat_icon, LV_SYMBOL_BATTERY_FULL);
    }
    else if (pct > 50)
    {
      lv_label_set_text(s_bat_icon, LV_SYMBOL_BATTERY_3);
    }
    else if (pct > 25)
    {
      lv_label_set_text(s_bat_icon, LV_SYMBOL_BATTERY_2);
    }
    else if (pct > 5)
    {
      lv_label_set_text(s_bat_icon, LV_SYMBOL_BATTERY_1);
    }
    else
    {
      lv_label_set_text(s_bat_icon, LV_SYMBOL_BATTERY_EMPTY);
    }

    if (low)
    {
      lv_obj_set_style_text_color(s_bat_icon, lv_palette_main(LV_PALETTE_RED), 0);
    }
    else
    {
      lv_obj_remove_local_style_prop(s_bat_icon, LV_STYLE_TEXT_COLOR, 0);
    }
    set_battery_blink(critical);

    ESP_LOGI(tag, "Battery: %d%% (%s)", pct, usb ? "USB" : "BATT");
  }
  else
  {
    // No battery connected or ADC error
    lv_label_set_text(s_bat_pct, "N/A");
    lv_label_set_text(s_bat_icon, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_remove_local_style_prop(s_bat_icon, LV_STYLE_TEXT_COLOR, 0);
    set_battery_blink(false);
  }

  if (s_battery_cb)
  {
    s_battery_cb(pct, usb);
  }

  // Re-align entire chain (text width may have changed)
  realign_chain();
}

// ============================================================
// Public API
// ============================================================
void status_bar_create(lv_obj_t *parent)
{
  // --- Battery percentage (top-right corner) ---
  s_bat_pct = lv_label_create(parent);
  lv_obj_set_width(s_bat_pct, LV_SIZE_CONTENT);
  lv_obj_set_height(s_bat_pct, LV_SIZE_CONTENT);
  lv_obj_align(s_bat_pct, LV_ALIGN_TOP_RIGHT, -8, 4);
  lv_label_set_text(s_bat_pct, "--%");

  // --- Battery icon (left of percentage, with gap) ---
  s_bat_icon = lv_label_create(parent);
  lv_obj_set_width(s_bat_icon, LV_SIZE_CONTENT);
  lv_obj_set_height(s_bat_icon, LV_SIZE_CONTENT);
  lv_obj_align_to(s_bat_icon, s_bat_pct, LV_ALIGN_OUT_LEFT_MID, -4, 0);
  lv_label_set_text(s_bat_icon, LV_SYMBOL_BATTERY_FULL);

  // --- WiFi icon (left of battery icon) ---
  s_wifi_icon = lv_label_create(parent);
  lv_obj_set_width(s_wifi_icon, LV_SIZE_CONTENT);
  lv_obj_set_height(s_wifi_icon, LV_SIZE_CONTENT);
  lv_obj_align_to(s_wifi_icon, s_bat_icon, LV_ALIGN_OUT_LEFT_MID, -6, 0);
  lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_40, 0); // Dim initially
  lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);

  // --- SNTP icon (left of WiFi icon) ---
  s_sntp_icon = lv_label_create(parent);
  lv_obj_set_width(s_sntp_icon, LV_SIZE_CONTENT);
  lv_obj_set_height(s_sntp_icon, LV_SIZE_CONTENT);
  lv_obj_align_to(s_sntp_icon, s_wifi_icon, LV_ALIGN_OUT_LEFT_MID, -6, 0);
  lv_obj_set_style_text_opa(s_sntp_icon, LV_OPA_40, 0); // Dim initially
  lv_label_set_text(s_sntp_icon, LV_SYMBOL_REFRESH);

  // --- TS (Tailscale) icon (left of SNTP icon) ---
  s_ts_icon = lv_label_create(parent);
  lv_obj_set_width(s_ts_icon, LV_SIZE_CONTENT);
  lv_obj_set_height(s_ts_icon, LV_SIZE_CONTENT);
  lv_obj_align_to(s_ts_icon, s_sntp_icon, LV_ALIGN_OUT_LEFT_MID, -6, 0);
  lv_obj_set_style_text_opa(s_ts_icon, LV_OPA_40, 0); // Dim initially
  lv_label_set_text(s_ts_icon, LV_SYMBOL_SHUFFLE);

  // --- LVGL timer: update battery every 30 seconds ---
  s_bat_timer = lv_timer_create(battery_timer_cb, 30000, NULL);
  lv_timer_ready(s_bat_timer); // Fire immediately on first tick

  // Restore last known status (survives screen transitions)
  status_bar_set_wifi_status(s_last_wifi_status);
  status_bar_set_sntp_status(s_last_sntp_status);
  status_bar_set_ts_status(s_last_ts_status);
}

void status_bar_register_battery_cb(status_bar_battery_cb_t cb)
{
  s_battery_cb = cb;
}

void status_bar_set_wifi_status(wifi_status_t status)
{
  s_last_wifi_status = status;

  if (!s_wifi_icon)
  {
    return;
  }

  switch (status)
  {
  case WIFI_STATUS_DISCONNECTED:
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_40, 0);
    lv_obj_remove_local_style_prop(s_wifi_icon, LV_STYLE_TEXT_COLOR, 0);
    break;

  case WIFI_STATUS_SCANNING:
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_70, 0);
    lv_obj_remove_local_style_prop(s_wifi_icon, LV_STYLE_TEXT_COLOR, 0);
    break;

  case WIFI_STATUS_CONNECTING:
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_wifi_icon, lv_palette_main(LV_PALETTE_ORANGE), 0);
    break;

  case WIFI_STATUS_VERIFYING:
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_wifi_icon, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
    break;

  case WIFI_STATUS_CONNECTED:
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_wifi_icon, lv_palette_main(LV_PALETTE_GREEN), 0);
    break;

  case WIFI_STATUS_NO_INTERNET:
    // Yellow, not green: the association and the IP lease are real, so this is not a
    // disconnection, but the DNS probe failed and anything needing the internet — NTP above all
    // — will not work. Without this the device showed a plain green icon while displaying 1970.
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_wifi_icon, lv_palette_main(LV_PALETTE_YELLOW), 0);
    break;

  case WIFI_STATUS_PROVISIONING:
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_wifi_icon, lv_palette_main(LV_PALETTE_CYAN), 0);
    break;
  }

  // Re-align entire chain after icon change
  realign_chain();
}

void status_bar_set_sntp_status(sntp_status_t status)
{
  s_last_sntp_status = status;

  if (!s_sntp_icon)
  {
    return;
  }

  // A switch, not an if/else: FAILED used to fall into the else and hide the icon, which is how a
  // device could sit there showing 01/01/1970 with a completely clean status bar. Enumerating the
  // cases means the compiler flags the next status someone adds instead of silently hiding it.
  switch (status)
  {
  case SNTP_STATUS_SYNCING:
    lv_obj_remove_flag(s_sntp_icon, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_sntp_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_opa(s_sntp_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_sntp_icon, lv_palette_main(LV_PALETTE_ORANGE), 0);
    break;

  case SNTP_STATUS_FAILED:
    // Red and visible. The time on screen is wrong and nothing else says so.
    lv_obj_remove_flag(s_sntp_icon, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_sntp_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_opa(s_sntp_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_sntp_icon, lv_palette_main(LV_PALETTE_RED), 0);
    break;

  case SNTP_STATUS_IDLE:
  case SNTP_STATUS_SYNCED:
    // Nothing to report — hidden, so the icon chain collapses around it.
    lv_obj_add_flag(s_sntp_icon, LV_OBJ_FLAG_HIDDEN);
    break;
  }

  // Re-align chain — TS anchors to WiFi when SNTP is hidden
  realign_chain();
}

void status_bar_set_ts_status(ts_status_t status)
{
  s_last_ts_status = status;

  if (!s_ts_icon)
  {
    return;
  }

  switch (status)
  {
  case TS_STATUS_IDLE:
    lv_label_set_text(s_ts_icon, LV_SYMBOL_SHUFFLE);
    lv_obj_set_style_text_opa(s_ts_icon, LV_OPA_40, 0);
    lv_obj_remove_local_style_prop(s_ts_icon, LV_STYLE_TEXT_COLOR, 0);
    break;

  case TS_STATUS_CONNECTING:
    lv_label_set_text(s_ts_icon, LV_SYMBOL_SHUFFLE);
    lv_obj_set_style_text_opa(s_ts_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_ts_icon, lv_palette_main(LV_PALETTE_ORANGE), 0);
    break;

  case TS_STATUS_CONNECTED:
    lv_label_set_text(s_ts_icon, LV_SYMBOL_SHUFFLE);
    lv_obj_set_style_text_opa(s_ts_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_ts_icon, lv_palette_main(LV_PALETTE_GREEN), 0);
    break;

  case TS_STATUS_ERROR:
    lv_label_set_text(s_ts_icon, LV_SYMBOL_SHUFFLE);
    lv_obj_set_style_text_opa(s_ts_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_ts_icon, lv_palette_main(LV_PALETTE_RED), 0);
    break;
  }

  realign_chain();
}

void status_bar_destroy(void)
{
  if (s_bat_timer)
  {
    lv_timer_delete(s_bat_timer);
    s_bat_timer = NULL;
  }
  if (s_bat_blink_timer)
  {
    lv_timer_delete(s_bat_blink_timer);
    s_bat_blink_timer = NULL;
  }
  s_bat_icon = NULL;
  s_bat_pct = NULL;
  s_wifi_icon = NULL;
  s_sntp_icon = NULL;
  s_ts_icon = NULL;
}
