// SPDX-License-Identifier: MIT
// ZenClock — BLE Provisioning overlay screen
//
// Layout (320×170 landscape):
//   [QR 140×140, left, y-centered] | [Title / password / device name, right]
//
// QR payload format (Espressif BLE Prov app, Security 2):
//   {"ver":"v1","name":"<device>","username":"ZenClock","pop":"<password>","transport":"ble"}

#include "prov_screen.h"
#include "lvgl.h"

#include <freertos/FreeRTOS.h>

#include <stdio.h>
#include <string.h>

#define QR_SIZE      140
#define QR_X_PAD     12
#define TEXT_X_PAD   8
#define TEXT_X_START (QR_X_PAD + QR_SIZE + TEXT_X_PAD)
#define TEXT_WIDTH   (320 - TEXT_X_START - 8)

static lv_obj_t *s_overlay = NULL;

// ============================================================
// Published overlay intent
//
// prov_screen_show()/hide() are called from the BLE event loop, the button task and the button
// worker. None of them may take the LVGL lock: the BLE handler shares the default event loop with
// the WiFi driver's own events, and the button task parks bsp_buttons.c's held_ms while it waits.
// So they publish an intent — "the QR should be up, with this name and password" — and ui.c's
// reconcile tick builds or tears down the overlay on the LVGL task.
//
// This is also why the overlay is intent rather than a one-shot paint: prov_screen_show() cannot
// be skipped or lost. A dropped paint here means no QR at all and a device that looks dead during
// setup, which no "the next event will repaint it" argument can excuse.
//
// The three fields must be read as a set, so a spinlock guards them — held for two memcpys and
// nothing else. Formatting happens outside it: portENTER_CRITICAL disables interrupts on this
// core, snprintf is flash-resident newlib, and these publishers run on the BLE default event loop
// and on btn_worker, where added ISR latency is not free.
// ============================================================
static portMUX_TYPE s_intent_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_intent_on = false;
static char s_intent_name[32] = {0};
static char s_intent_pass[16] = {0};

// What the overlay currently on screen was built from, so a publish carrying different
// credentials rebuilds it instead of being silently dropped. LVGL side only.
static char s_built_name[sizeof(s_intent_name)] = {0};
static char s_built_pass[sizeof(s_intent_pass)] = {0};

static void build_overlay(const char *device_name, const char *password);

// The overlay is parented to whatever screen is active when provisioning starts, and nav.c
// deletes the old screen on every transition — which recursively deletes this overlay. Buttons
// stay live during provisioning, so a long-press on BOOT while the QR is up used to free the
// overlay behind our back: s_overlay kept pointing at released memory, prov_screen_hide() then
// deleted it a second time, and prov_screen_show() early-returned forever. Track the deletion
// instead of assuming we are the only one who can trigger it.
static void overlay_deleted_cb(lv_event_t *e)
{
  if (lv_event_get_target(e) == s_overlay)
  {
    s_overlay = NULL;
  }
}

void prov_screen_show(const char *device_name, const char *password)
{
  char name[sizeof(s_intent_name)];
  char pass[sizeof(s_intent_pass)];
  snprintf(name, sizeof(name), "%s", device_name ? device_name : "");
  snprintf(pass, sizeof(pass), "%s", password ? password : "");

  portENTER_CRITICAL(&s_intent_mux);
  memcpy(s_intent_name, name, sizeof(s_intent_name));
  memcpy(s_intent_pass, pass, sizeof(s_intent_pass));
  s_intent_on = true;
  portEXIT_CRITICAL(&s_intent_mux);
}

void prov_screen_hide(void)
{
  portENTER_CRITICAL(&s_intent_mux);
  s_intent_on = false;
  portEXIT_CRITICAL(&s_intent_mux);
}

bool prov_screen_is_visible(void)
{
  // Reports the published intent, not the widget. That is what every caller actually wants: the
  // deep-sleep inhibit and the nav-action guard are asking "is the user in the middle of setup",
  // and neither may take the LVGL lock to find out. It also closes an unsynchronized read — the
  // inhibit callback used to touch s_overlay from the deep-sleep timer with no lock at all.
  portENTER_CRITICAL(&s_intent_mux);
  const bool on = s_intent_on;
  portEXIT_CRITICAL(&s_intent_mux);
  return on;
}

void prov_screen_reconcile(void)
{
  bool want = false;
  char name[sizeof(s_intent_name)];
  char pass[sizeof(s_intent_pass)];

  portENTER_CRITICAL(&s_intent_mux);
  want = s_intent_on;
  memcpy(name, s_intent_name, sizeof(name));
  memcpy(pass, s_intent_pass, sizeof(pass));
  portEXIT_CRITICAL(&s_intent_mux);

  // Rebuild on changed credentials, not just on a missing overlay. The header promises the
  // published name and password are what ends up on screen, and do_provisioning()'s "already
  // active" branch re-publishes freshly fetched values on BLE_PROV_FAILED. They happen to be
  // identical today because the password is MAC-derived, but that is ble_provisioning.c's
  // business, not something this file may assume.
  const bool stale = s_overlay && (strcmp(name, s_built_name) != 0 || strcmp(pass, s_built_pass) != 0);

  // s_overlay, not the intent, decides whether to build: nav.c deletes the active screen on every
  // transition and takes this overlay with it (see overlay_deleted_cb). If that happens while the
  // intent is still on, rebuilding is the correct answer, not leaving a blank clock behind a
  // swallowed-input guard.
  if (stale || (!want && s_overlay))
  {
    lv_obj_delete(s_overlay);
    s_overlay = NULL;
  }

  if (want && !s_overlay)
  {
    build_overlay(name, pass);
    memcpy(s_built_name, name, sizeof(s_built_name));
    memcpy(s_built_pass, pass, sizeof(s_built_pass));
  }
}

static void build_overlay(const char *device_name, const char *password)
{
  // Full-screen black overlay on top of the current screen
  s_overlay = lv_obj_create(lv_screen_active());
  lv_obj_add_event_cb(s_overlay, overlay_deleted_cb, LV_EVENT_DELETE, NULL);
  lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_pos(s_overlay, 0, 0);
  lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_overlay, 0, 0);
  lv_obj_set_style_pad_all(s_overlay, 0, 0);
  lv_obj_set_style_radius(s_overlay, 0, 0);
  lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

  // Build QR payload — Security 2 format includes username + password
  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"ver\":\"v1\",\"name\":\"%s\",\"username\":\"wifiprov\",\"pop\":\"%s\",\"transport\":\"ble\"}",
           device_name ? device_name : "", password ? password : "");

  // QR code — dark=black on white for maximum contrast
  lv_obj_t *qr = lv_qrcode_create(s_overlay);
  lv_qrcode_set_size(qr, QR_SIZE);
  lv_qrcode_set_dark_color(qr, lv_color_black());
  lv_qrcode_set_light_color(qr, lv_color_white());
  lv_qrcode_set_quiet_zone(qr, true);
  lv_qrcode_set_data(qr, payload);
  lv_obj_align(qr, LV_ALIGN_LEFT_MID, QR_X_PAD, 0);

  // Title
  lv_obj_t *title = lv_label_create(s_overlay);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_label_set_text(title, "WiFi Setup");
  lv_obj_set_pos(title, TEXT_X_START, 10);

  // Instructions
  lv_obj_t *instr = lv_label_create(s_overlay);
  lv_obj_set_style_text_color(instr, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
  lv_obj_set_style_text_font(instr, &lv_font_montserrat_12, 0);
  lv_label_set_long_mode(instr, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(instr, TEXT_WIDTH);
  lv_label_set_text(instr, "Scan QR with\nEspressif BLE Prov\n(iOS / Android)");
  lv_obj_set_pos(instr, TEXT_X_START, 34);

  // Password label
  lv_obj_t *pass_lbl = lv_label_create(s_overlay);
  lv_obj_set_style_text_color(pass_lbl, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
  lv_obj_set_style_text_font(pass_lbl, &lv_font_montserrat_12, 0);
  lv_label_set_text(pass_lbl, "Password:");
  lv_obj_set_pos(pass_lbl, TEXT_X_START, 100);

  // Password value — shown prominently so user can type it in the app
  char pass_buf[16];
  snprintf(pass_buf, sizeof(pass_buf), "%s", password ? password : "");
  lv_obj_t *pass_val = lv_label_create(s_overlay);
  lv_obj_set_style_text_color(pass_val, lv_color_white(), 0);
  lv_obj_set_style_text_font(pass_val, &lv_font_montserrat_14, 0);
  lv_label_set_text(pass_val, pass_buf);
  lv_obj_set_pos(pass_val, TEXT_X_START, 116);

  // Device name (bottom — fallback for manual entry in app)
  lv_obj_t *devlabel = lv_label_create(s_overlay);
  lv_obj_set_style_text_color(devlabel, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_set_style_text_font(devlabel, &lv_font_montserrat_12, 0);
  lv_label_set_text(devlabel, device_name ? device_name : "");
  lv_obj_set_pos(devlabel, TEXT_X_START, 148);
}
