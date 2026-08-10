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

#include <stdio.h>
#include <string.h>

#define QR_SIZE      140
#define QR_X_PAD     12
#define TEXT_X_PAD   8
#define TEXT_X_START (QR_X_PAD + QR_SIZE + TEXT_X_PAD)
#define TEXT_WIDTH   (320 - TEXT_X_START - 8)

static lv_obj_t *s_overlay = NULL;

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
  if (s_overlay)
  {
    return;
  }

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

void prov_screen_hide(void)
{
  if (!s_overlay)
  {
    return;
  }

  lv_obj_delete(s_overlay);
  s_overlay = NULL;
}
