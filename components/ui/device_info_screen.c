// SPDX-License-Identifier: MIT
// ZenClock — System Info screen (read-only, scrollable)
//
// Layout (320×170):
//   Status bar (top, managed externally)
//   Title "System Info" (y=24)
//   Info rows starting at y=50, each 24px tall — 5 rows visible at a time
//
// Refresh schedule:
//   Static (once on create): Chip, Total Heap, Firmware, MAC
//   Every 1s:  Uptime
//   Every 10s: Free Heap, SSID, IP, Last NTP, TS Status, TS IP
//   Every 30s: Battery

#include "device_info_screen.h"
#include "ui_utils.h"
#include "bsp.h"
#include "wifi_manager.h"
#include "microlink.h"
#include "sntp_sync.h"

#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <stdio.h>
#include <time.h>

// ============================================================
// Layout constants (match settings_screen.c)
// ============================================================
#define TITLE_Y       24
#define LIST_Y_START  50
#define LIST_ITEM_H   24
#define LIST_X_PAD    16
#define VALUE_X_RIGHT (-12)

// ============================================================
// Row indices
// ============================================================
#define ROW_COUNT   12
#define ROW_VISIBLE 5

#define ROW_CHIP       0
#define ROW_FIRMWARE   1
#define ROW_MAC        2
#define ROW_FREE_HEAP  3
#define ROW_TOTAL_HEAP 4
#define ROW_UPTIME     5
#define ROW_SSID       6
#define ROW_IP         7
#define ROW_NTP_SYNC   8
#define ROW_TS_STATUS  9
#define ROW_TS_IP      10
#define ROW_BATTERY    11

static const char *const s_row_labels[ROW_COUNT] = {
    "Chip", "Firmware", "MAC",      "Free Heap", "Total Heap", "Uptime",
    "SSID", "IP",       "Last NTP", "TS Status", "TS IP",      "Battery",
};

// ============================================================
// Private state
// ============================================================
static int s_scroll = 0;
static lv_obj_t *s_name_labels[ROW_COUNT] = {NULL};
static lv_obj_t *s_value_labels[ROW_COUNT] = {NULL};
static microlink_t *s_ml_handle = NULL;

static lv_timer_t *s_timer_1s = NULL;
static lv_timer_t *s_timer_10s = NULL;
static lv_timer_t *s_timer_30s = NULL;

// ============================================================
// Scroll helper — show/hide rows based on s_scroll
// ============================================================
static void apply_scroll(void)
{
  for (int i = 0; i < ROW_COUNT; i++)
  {
    const bool visible = (i >= s_scroll && i < s_scroll + ROW_VISIBLE);
    const int y = LIST_Y_START + (i - s_scroll) * LIST_ITEM_H;

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
// Dynamic value updaters
// ============================================================
static void update_uptime(void)
{
  if (!s_value_labels[ROW_UPTIME])
  {
    return;
  }
  const int64_t total_s = esp_timer_get_time() / 1000000;
  const int h = (int) (total_s / 3600);
  const int m = (int) ((total_s % 3600) / 60);
  const int s = (int) (total_s % 60);
  char buf[16];
  snprintf(buf, sizeof(buf), "%dh %02dm %02ds", h, m, s);
  lv_label_set_text(s_value_labels[ROW_UPTIME], buf);
}

static void update_network(void)
{
  if (s_value_labels[ROW_FREE_HEAP])
  {
    char buf[16];
    fmt_bytes(buf, sizeof(buf), esp_get_free_heap_size());
    lv_label_set_text(s_value_labels[ROW_FREE_HEAP], buf);
  }
  if (s_value_labels[ROW_SSID])
  {
    const char *ssid = wifi_manager_get_ssid();
    lv_label_set_text(s_value_labels[ROW_SSID], ssid ? ssid : "Not connected");
  }
  if (s_value_labels[ROW_IP])
  {
    const char *ip = wifi_manager_get_ip_str();
    lv_label_set_text(s_value_labels[ROW_IP], ip ? ip : "N/A");
  }
}

static void update_ntp_sync(void)
{
  if (!s_value_labels[ROW_NTP_SYNC])
  {
    return;
  }
  const time_t t = sntp_sync_get_last_sync_time();
  if (t == 0)
  {
    lv_label_set_text(s_value_labels[ROW_NTP_SYNC], "Never");
    return;
  }
  struct tm tm_info;
  localtime_r(&t, &tm_info);
  char buf[20];
  strftime(buf, sizeof(buf), "%H:%M %d/%m/%y", &tm_info);
  lv_label_set_text(s_value_labels[ROW_NTP_SYNC], buf);
}

static const char *ts_state_str(const microlink_state_t state)
{
  switch (state)
  {
  case ML_STATE_IDLE:
    return "Idle";
  case ML_STATE_WIFI_WAIT:
    return "Waiting";
  case ML_STATE_CONNECTING:
    return "Connecting";
  case ML_STATE_REGISTERING:
    return "Registering";
  case ML_STATE_CONNECTED:
    return "Connected";
  case ML_STATE_RECONNECTING:
    return "Reconnecting";
  case ML_STATE_ERROR:
    return "Error";
  default:
    return "Unknown";
  }
}

static void update_tailscale(void)
{
  if (!s_ml_handle)
  {
    if (s_value_labels[ROW_TS_STATUS])
    {
      lv_label_set_text(s_value_labels[ROW_TS_STATUS], "N/A");
    }
    if (s_value_labels[ROW_TS_IP])
    {
      lv_label_set_text(s_value_labels[ROW_TS_IP], "N/A");
    }
    return;
  }

  const microlink_state_t state = microlink_get_state(s_ml_handle);

  if (s_value_labels[ROW_TS_STATUS])
  {
    lv_label_set_text(s_value_labels[ROW_TS_STATUS], ts_state_str(state));
  }
  if (s_value_labels[ROW_TS_IP])
  {
    if (state == ML_STATE_CONNECTED)
    {
      char ip[16];
      microlink_ip_to_str(microlink_get_vpn_ip(s_ml_handle), ip);
      lv_label_set_text(s_value_labels[ROW_TS_IP], ip);
    }
    else
    {
      lv_label_set_text(s_value_labels[ROW_TS_IP], "N/A");
    }
  }
}

static void update_battery(void)
{
  if (!s_value_labels[ROW_BATTERY])
  {
    return;
  }
  char buf[20];
  snprintf(buf, sizeof(buf), "%d%% (%dmV)", bsp_battery_get_percentage(), bsp_battery_get_voltage());
  lv_label_set_text(s_value_labels[ROW_BATTERY], buf);
}

// ============================================================
// Timer callbacks
// ============================================================
static void timer_1s_cb(lv_timer_t *t) // NOLINT(readability-non-const-parameter)
{
  (void) t;
  update_uptime();
}

static void timer_10s_cb(lv_timer_t *t) // NOLINT(readability-non-const-parameter)
{
  (void) t;
  update_network();
  update_ntp_sync();
  update_tailscale();
}

static void timer_30s_cb(lv_timer_t *t) // NOLINT(readability-non-const-parameter)
{
  (void) t;
  update_battery();
}

// ============================================================
// Static value helpers
// ============================================================
static const char *chip_model_str(const esp_chip_model_t model)
{
  switch (model)
  {
  case CHIP_ESP32:
    return "ESP32";
  case CHIP_ESP32S2:
    return "ESP32-S2";
  case CHIP_ESP32S3:
    return "ESP32-S3";
  case CHIP_ESP32C3:
    return "ESP32-C3";
  case CHIP_ESP32H2:
    return "ESP32-H2";
  default:
    return "ESP32-??";
  }
}

// ============================================================
// Row creation helper
// ============================================================
static void create_row(lv_obj_t *parent, const int idx)
{
  const int y = LIST_Y_START + idx * LIST_ITEM_H;

  s_name_labels[idx] = lv_label_create(parent);
  lv_obj_set_style_text_font(s_name_labels[idx], &lv_font_montserrat_14, 0);
  lv_label_set_text(s_name_labels[idx], s_row_labels[idx]);
  lv_obj_set_pos(s_name_labels[idx], LIST_X_PAD, y);

  s_value_labels[idx] = lv_label_create(parent);
  lv_obj_set_style_text_font(s_value_labels[idx], &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_opa(s_value_labels[idx], LV_OPA_70, 0);
  lv_label_set_long_mode(s_value_labels[idx], LV_LABEL_LONG_CLIP);
  lv_obj_set_width(s_value_labels[idx], 160);
  lv_obj_align(s_value_labels[idx], LV_ALIGN_TOP_RIGHT, VALUE_X_RIGHT, y);
}

// ============================================================
// Public API
// ============================================================
void device_info_screen_create(lv_obj_t *parent)
{
  s_scroll = 0;
  for (int i = 0; i < ROW_COUNT; i++)
  {
    s_name_labels[i] = NULL;
    s_value_labels[i] = NULL;
  }

  // Title
  lv_obj_t *title = lv_label_create(parent);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_label_set_text(title, "System Info");
  lv_obj_set_pos(title, LIST_X_PAD, TITLE_Y);

  for (int i = 0; i < ROW_COUNT; i++)
  {
    create_row(parent, i);
  }

  // Populate static rows
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  char chip_buf[32];
  snprintf(chip_buf, sizeof(chip_buf), "%s r%d %dc", chip_model_str(chip.model), chip.revision, chip.cores);
  lv_label_set_text(s_value_labels[ROW_CHIP], chip_buf);

  char heap_buf[16];
  fmt_bytes(heap_buf, sizeof(heap_buf), heap_caps_get_total_size(MALLOC_CAP_DEFAULT));
  lv_label_set_text(s_value_labels[ROW_TOTAL_HEAP], heap_buf);

  // Version and build date come from the app descriptor, which the build system fills from
  // version.txt (maintained by release-please). __DATE__ would report when this translation
  // unit was last compiled, not when the firmware was linked.
  const esp_app_desc_t *app = esp_app_get_description();
  // Sized for the descriptor's declared bounds (version[32] + ' ' + date[16]), not the
  // typical "0.2.3 Aug 10 2026" — otherwise -Wformat-truncation fails the -Werror build.
  char fw_buf[48];
  if (app)
  {
    snprintf(fw_buf, sizeof(fw_buf), "%s %s", app->version, app->date);
  }
  else
  {
    // Should not happen on a normally linked image, but this is an info screen — showing a
    // placeholder beats faulting on a NULL deref just to display a version string.
    snprintf(fw_buf, sizeof(fw_buf), "unknown");
  }
  lv_label_set_text(s_value_labels[ROW_FIRMWARE], fw_buf);

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char mac_buf[18];
  snprintf(mac_buf, sizeof(mac_buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  lv_label_set_text(s_value_labels[ROW_MAC], mac_buf);

  // Initial dynamic values
  update_uptime();
  update_network();
  update_ntp_sync();
  update_tailscale();
  update_battery();

  apply_scroll();

  s_timer_1s = lv_timer_create(timer_1s_cb, 1000, NULL);
  s_timer_10s = lv_timer_create(timer_10s_cb, 10000, NULL);
  s_timer_30s = lv_timer_create(timer_30s_cb, 30000, NULL);
}

void device_info_screen_set_ml(microlink_t *ml)
{
  s_ml_handle = ml;
  update_tailscale();
}

void device_info_screen_scroll_up(void)
{
  s_scroll = ui_circ_prev(s_scroll, ROW_COUNT - ROW_VISIBLE + 1);
  apply_scroll();
}

void device_info_screen_scroll_down(void)
{
  s_scroll = ui_circ_next(s_scroll, ROW_COUNT - ROW_VISIBLE + 1);
  apply_scroll();
}

void device_info_screen_destroy(void)
{
  if (s_timer_1s)
  {
    lv_timer_delete(s_timer_1s);
    s_timer_1s = NULL;
  }
  if (s_timer_10s)
  {
    lv_timer_delete(s_timer_10s);
    s_timer_10s = NULL;
  }
  if (s_timer_30s)
  {
    lv_timer_delete(s_timer_30s);
    s_timer_30s = NULL;
  }
  for (int i = 0; i < ROW_COUNT; i++)
  {
    s_name_labels[i] = NULL;
    s_value_labels[i] = NULL;
  }
}
