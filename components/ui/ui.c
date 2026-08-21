// SPDX-License-Identifier: MIT
// ZenClock UI — screen + theme + module composition

#include "ui.h"
#include "nav.h"
#include "prov_screen.h"
#include "status_bar.h"
#include "lvgl.h"

#include <esp_log.h>

static const char *const tag = "UI";

static bool s_is_light = true;
static lv_style_t s_screen_bg_style;
static bool s_style_inited = false;
static lv_timer_t *s_reconcile_timer = NULL;

// ============================================================
// Reconcile tick
//
// The one place published UI state becomes pixels. Foreign tasks — the wifi task, the SNTP task,
// the BLE event loop, the Tailscale poll timer, the button worker — publish what they want on
// screen and return without touching LVGL. This timer runs inside lv_timer_handler(), so it holds
// the LVGL lock by construction, and repaints only what has actually changed.
//
// It lives here rather than inside each widget because the overlay and the status bar have
// different lifetimes: the status bar is destroyed and rebuilt on every screen change, and the
// overlay may need building when no screen owns it yet. One tick outliving both is what makes a
// publish impossible to lose. See docs/adr/0007-published-ui-state.md.
// ============================================================
#define UI_RECONCILE_PERIOD_MS 250

static void reconcile_cb(lv_timer_t *timer) // NOLINT(readability-non-const-parameter)
{
  (void) timer;
  status_bar_reconcile(false);
  prov_screen_reconcile();
}

void ui_init(const bool is_light)
{
  s_is_light = is_light;

  lv_style_init(&s_screen_bg_style);
  lv_style_set_bg_color(&s_screen_bg_style, is_light ? lv_color_hex(0xe6e6e6) : lv_color_hex(0x0d0d0d));
  lv_style_set_bg_opa(&s_screen_bg_style, LV_OPA_COVER);
  s_style_inited = true;

  lv_disp_t *dispp = lv_display_get_default();
  lv_theme_t *theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
                                            !is_light, LV_FONT_DEFAULT);
  lv_disp_set_theme(dispp, theme);

  nav_init();

  // After nav_init(): the first tick must find a status bar to paint.
  s_reconcile_timer = lv_timer_create(reconcile_cb, UI_RECONCILE_PERIOD_MS, NULL);
  if (!s_reconcile_timer)
  {
    ESP_LOGE(tag, "Reconcile timer creation failed — status icons fall back to the battery "
                  "timer's 30s refresh and the provisioning QR will not appear");
  }
}

void ui_set_theme(const bool is_light)
{
  s_is_light = is_light;

  lv_style_set_bg_color(&s_screen_bg_style, is_light ? lv_color_hex(0xe6e6e6) : lv_color_hex(0x0d0d0d));
  lv_obj_report_style_change(&s_screen_bg_style);

  lv_disp_t *dispp = lv_display_get_default();
  lv_theme_t *theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
                                            !is_light, LV_FONT_DEFAULT);
  lv_disp_set_theme(dispp, theme);
}

bool ui_is_light_theme(void)
{
  return s_is_light;
}

void ui_apply_screen_bg(lv_obj_t *scr)
{
  if (s_style_inited)
  {
    lv_obj_add_style(scr, &s_screen_bg_style, 0);
  }
}
