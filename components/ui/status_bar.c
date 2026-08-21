// SPDX-License-Identifier: MIT
// ZenClock — Status bar (SNTP indicator + WiFi indicator + battery icon + percentage)

#include "status_bar.h"
#include "ui_list.h"
#include "bsp.h"

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
static lv_timer_t *s_reconcile_timer = NULL;
static status_bar_battery_cb_t s_battery_cb = NULL;

#define BATT_BLINK_PERIOD_MS 500

// Published WiFi status: written by whatever task learns it (wifi task, SNTP task, the BLE event
// loop), painted only on the LVGL task by the reconcile section below. Persisting it across screen
// switches is what lets the icon restore on recreate.
static volatile wifi_status_t s_pub_wifi = WIFI_STATUS_DISCONNECTED;

// What the icon currently shows. LVGL task only — never written by a publisher.
static wifi_status_t s_painted_wifi = WIFI_STATUS_DISCONNECTED;

// Defined with the reconcile section below; declared here for battery_timer_cb()'s backstop call.
static void reconcile_wifi(void);

// Persist status across screen switches so icons restore correctly on recreate
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
static const char *symbol_text(const battery_symbol_t symbol)
{
  switch (symbol)
  {
  case BATTERY_SYMBOL_CHARGE:
    return LV_SYMBOL_CHARGE;
  case BATTERY_SYMBOL_FULL:
    return LV_SYMBOL_BATTERY_FULL;
  case BATTERY_SYMBOL_3:
    return LV_SYMBOL_BATTERY_3;
  case BATTERY_SYMBOL_2:
    return LV_SYMBOL_BATTERY_2;
  case BATTERY_SYMBOL_1:
    return LV_SYMBOL_BATTERY_1;
  case BATTERY_SYMBOL_EMPTY:
    return LV_SYMBOL_BATTERY_EMPTY;
  }
  return LV_SYMBOL_BATTERY_EMPTY; // unreachable — the switch is exhaustive, so -Wswitch still bites
}

static void battery_timer_cb(lv_timer_t *timer) // NOLINT(readability-non-const-parameter)
{
  (void) timer;

  int pct;
  bool usb;
  bsp_battery_read(&pct, &usb);
  const battery_view_t view = battery_view(pct, usb);

  lv_label_set_text(s_bat_pct, view.text);
  lv_label_set_text(s_bat_icon, symbol_text(view.symbol));

  switch (view.tint)
  {
  case BATTERY_TINT_LOW:
    lv_obj_set_style_text_color(s_bat_icon, lv_palette_main(LV_PALETTE_RED), 0);
    break;
  case BATTERY_TINT_DEFAULT:
    lv_obj_remove_local_style_prop(s_bat_icon, LV_STYLE_TEXT_COLOR, 0);
    break;
  }

  set_battery_blink(view.blink);

  if (pct >= 0)
  {
    ESP_LOGI(tag, "Battery: %d%% (%s)", pct, usb ? "USB" : "BATT");
  }

  if (s_battery_cb)
  {
    s_battery_cb(view);
  }

  // Backstop: if lv_timer_create() failed for the reconcile timer, this is the only thing left
  // that can move the WiFi icon. Costs one enum compare every 30s.
  reconcile_wifi();

  // Re-align entire chain (text width may have changed)
  realign_chain();
}

// ============================================================
// Published WiFi status -> reconcile
//
// Foreign tasks publish a desired status and never paint. This timer and status_bar_create() are
// the only things that reconcile it onto the icon, and both run on the LVGL task — so no foreign
// task ever takes the LVGL lock to move this icon, and no paint can be silently skipped.
//
// Reconciling is a comparison against what is on screen, deliberately, not a dirty flag. The two
// cores give `volatile` no ordering guarantee, so a flag could become visible before the value it
// refers to; the reconcile would then clear the flag, paint the stale value, and — CONNECTED and
// NO_INTERNET being terminal states with no follow-up event — never repaint. Comparing instead
// makes a torn read cost one 250 ms tick and nothing else: the next tick still sees a difference
// and corrects it.
// ============================================================
#define RECONCILE_PERIOD_MS 250

static void paint_wifi_status(wifi_status_t status)
{
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

// Paints unconditionally. Used by status_bar_create(), where the icon is a brand-new object and
// nothing can be inferred from s_painted_wifi.
static void reconcile_wifi_force(void)
{
  if (!s_wifi_icon)
  {
    return;
  }
  const wifi_status_t want = s_pub_wifi; // read once: a publisher may write it at any moment
  paint_wifi_status(want);
  s_painted_wifi = want;
}

static void reconcile_wifi(void)
{
  if (!s_wifi_icon || s_pub_wifi == s_painted_wifi)
  {
    return;
  }
  reconcile_wifi_force();
}

static void reconcile_timer_cb(lv_timer_t *timer) // NOLINT(readability-non-const-parameter)
{
  (void) timer;
  reconcile_wifi();
}

// ============================================================
// Public API
// ============================================================
// clang-tidy scores this at 51 almost entirely from the two ESP_LOGE macro expansions; the
// function itself is a flat sequence of widget creations with two timer null-checks.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
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
  if (s_bat_timer)
  {
    lv_timer_ready(s_bat_timer); // Fire immediately on first tick
  }
  else
  {
    ESP_LOGE(tag, "Battery timer creation failed — battery indicator will not update");
  }

  // --- LVGL timer: reconcile published status onto the icons ---
  s_reconcile_timer = lv_timer_create(reconcile_timer_cb, RECONCILE_PERIOD_MS, NULL);
  if (!s_reconcile_timer)
  {
    ESP_LOGE(tag, "Reconcile timer creation failed — WiFi indicator will not update");
  }

  // Reconcile immediately: this is also the restore-on-recreate path, which is why it paints
  // unconditionally rather than comparing against what the previous icon showed.
  reconcile_wifi_force();
  status_bar_set_sntp_status(s_last_sntp_status);
  status_bar_set_ts_status(s_last_ts_status);
}

void status_bar_register_battery_cb(status_bar_battery_cb_t cb)
{
  s_battery_cb = cb;
}

// Publish only — callable from any task with no LVGL lock held. One store; nothing here touches
// an LVGL object. The paint happens on the LVGL task, within one reconcile tick.
void status_bar_set_wifi_status(wifi_status_t status)
{
  s_pub_wifi = status;
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
  ui_timer_delete(&s_bat_timer);
  ui_timer_delete(&s_reconcile_timer);
  ui_timer_delete(&s_bat_blink_timer);
  s_bat_icon = NULL;
  s_bat_pct = NULL;
  s_wifi_icon = NULL;
  s_sntp_icon = NULL;
  s_ts_icon = NULL;
}
