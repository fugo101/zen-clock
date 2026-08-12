// SPDX-License-Identifier: MIT
#include "settings.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *const tag = "Settings";
static const char *nvs_namespace = "zenclock";
static const char *key_theme_light = "theme_light";
static const char *key_brightness = "brightness";
static const char *key_sleep_h = "sleep_h";
static const char *key_sleep_m = "sleep_m";
static const char *key_sleep_s = "sleep_s";
static const char *key_time_fmt = "time_fmt";
static const char *key_show_secs = "show_secs";
static const char *key_tz_offset = "tz_offset";

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void settings_init(void)
{
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_LOGW(tag, "NVS needs to be erased. Erasing...");
    const esp_err_t erase_ret = nvs_flash_erase();
    if (erase_ret != ESP_OK)
    {
      // Aborting here turned a worn-out or missing NVS partition into a permanent boot loop.
      // Every getter below already falls back to a compiled-in default when nvs_open() fails,
      // so a device that cannot persist settings is still a perfectly usable clock.
      ESP_LOGE(tag, "nvs_flash_erase failed (%s) — continuing with defaults, settings will not persist",
               esp_err_to_name(erase_ret));
      return;
    }
    ret = nvs_flash_init();
  }
  if (ret != ESP_OK)
  {
    ESP_LOGE(tag, "nvs_flash_init failed (%s) — continuing with defaults, settings will not persist",
             esp_err_to_name(ret));
    return;
  }
  ESP_LOGI(tag, "NVS initialized.");
}

// Below SETTINGS_BRIGHTNESS_MIN the panel is unreadable, and the settings screen is the only way
// back up — so a stored 0 (which older firmware allowed) locked the user out for good. Clamping on
// read as well as on write is what recovers a device that already has 0 in NVS.
static uint8_t clamp_brightness(uint8_t percent)
{
  if (percent > 100)
  {
    return 100;
  }
  if (percent < SETTINGS_BRIGHTNESS_MIN)
  {
    return SETTINGS_BRIGHTNESS_MIN;
  }
  return percent;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool settings_get_theme_light(void)
{
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READONLY, &my_handle);
  if (err != ESP_OK)
  {
    // Namespace doesn't exist yet, return default (dark theme)
    return false;
  }

  uint8_t is_light = 0; // 0 = dark, 1 = light
  err = nvs_get_u8(my_handle, key_theme_light, &is_light);
  switch (err)
  {
  case ESP_OK:
    ESP_LOGI(tag, "Loaded theme config: %s", is_light ? "Light" : "Dark");
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_LOGD(tag, "Theme config not found, using default (Dark)");
    break;
  default:
    ESP_LOGE(tag, "Error reading theme config (%s)", esp_err_to_name(err));
    break;
  }

  nvs_close(my_handle);
  return is_light != 0;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void settings_set_theme_light(bool is_light)
{
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READWRITE, &my_handle);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "Error opening NVS handle (%s)", esp_err_to_name(err));
    return;
  }

  err = nvs_set_u8(my_handle, key_theme_light, is_light ? 1 : 0);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "Error saving theme config (%s)", esp_err_to_name(err));
  }
  else
  {
    err = nvs_commit(my_handle);
    if (err != ESP_OK)
    {
      ESP_LOGE(tag, "Error committing NVS (%s)", esp_err_to_name(err));
    }
    else
    {
      ESP_LOGI(tag, "Saved theme config: %s", is_light ? "Light" : "Dark");
    }
  }
  nvs_close(my_handle);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
uint8_t settings_get_brightness(void)
{
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READONLY, &my_handle);
  if (err != ESP_OK)
  {
    return 100; // default: full brightness
  }

  uint8_t val = 100;
  err = nvs_get_u8(my_handle, key_brightness, &val);
  switch (err)
  {
  case ESP_OK:
    ESP_LOGI(tag, "Loaded brightness: %d%%", val);
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_LOGD(tag, "Brightness not found, using default (100%%)");
    break;
  default:
    ESP_LOGE(tag, "Error reading brightness (%s)", esp_err_to_name(err));
    break;
  }

  nvs_close(my_handle);
  return clamp_brightness(val);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void settings_set_brightness(uint8_t percent)
{
  percent = clamp_brightness(percent);

  nvs_handle_t my_handle;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READWRITE, &my_handle);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "Error opening NVS handle (%s)", esp_err_to_name(err));
    return;
  }

  err = nvs_set_u8(my_handle, key_brightness, percent);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "Error saving brightness (%s)", esp_err_to_name(err));
  }
  else
  {
    err = nvs_commit(my_handle);
    if (err != ESP_OK)
    {
      ESP_LOGE(tag, "Error committing NVS (%s)", esp_err_to_name(err));
    }
    else
    {
      ESP_LOGI(tag, "Saved brightness: %d%%", percent);
    }
  }
  nvs_close(my_handle);
}

// --- Sleep timeout helpers (H/M/S) ---
// Pattern identical to brightness: open READONLY/READWRITE, get/set u8, close.

static uint8_t get_sleep_component(const char *key, uint8_t default_val) // NOLINT
{
  nvs_handle_t h;
  if (nvs_open(nvs_namespace, NVS_READONLY, &h) != ESP_OK)
  {
    return default_val;
  }
  uint8_t val = default_val;
  nvs_get_u8(h, key, &val);
  nvs_close(h);
  return val;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void set_sleep_component(const char *key, uint8_t val, uint8_t max_val)
{
  if (val > max_val)
  {
    val = max_val;
  }
  nvs_handle_t h;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READWRITE, &h);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "Error opening NVS (%s)", esp_err_to_name(err));
    return;
  }
  err = nvs_set_u8(h, key, val);
  if (err == ESP_OK)
  {
    nvs_commit(h);
  }
  else
  {
    ESP_LOGE(tag, "Error saving %s (%s)", key, esp_err_to_name(err));
  }
  nvs_close(h);
}

uint8_t settings_get_sleep_h(void)
{
  return get_sleep_component(key_sleep_h, 0);
}
uint8_t settings_get_sleep_m(void)
{
  return get_sleep_component(key_sleep_m, 0);
}
uint8_t settings_get_sleep_s(void)
{
  return get_sleep_component(key_sleep_s, 0);
}

void settings_set_sleep_h(uint8_t h)
{
  set_sleep_component(key_sleep_h, h, 23);
}
void settings_set_sleep_m(uint8_t m)
{
  set_sleep_component(key_sleep_m, m, 59);
}
void settings_set_sleep_s(uint8_t s)
{
  set_sleep_component(key_sleep_s, s, 59);
}

bool settings_get_time_format_24h(void)
{
  return get_sleep_component(key_time_fmt, 1) != 0;
}

void settings_set_time_format_24h(bool is_24h)
{
  set_sleep_component(key_time_fmt, is_24h ? 1 : 0, 1);
}

bool settings_get_show_seconds(void)
{
  return get_sleep_component(key_show_secs, 1) != 0;
}

void settings_set_show_seconds(bool show)
{
  set_sleep_component(key_show_secs, show ? 1 : 0, 1);
}

int8_t settings_get_timezone_offset(void)
{
  nvs_handle_t h;
  if (nvs_open(nvs_namespace, NVS_READONLY, &h) != ESP_OK)
  {
    return 7;
  }
  int8_t val = 7;
  nvs_get_i8(h, key_tz_offset, &val);
  nvs_close(h);
  return val;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void settings_set_timezone_offset(int8_t offset)
{
  nvs_handle_t h;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READWRITE, &h);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "Error opening NVS (%s)", esp_err_to_name(err));
    return;
  }
  err = nvs_set_i8(h, key_tz_offset, offset);
  if (err == ESP_OK)
  {
    nvs_commit(h);
  }
  else
  {
    ESP_LOGE(tag, "Error saving tz_offset (%s)", esp_err_to_name(err));
  }
  nvs_close(h);
}

void settings_apply_timezone(int8_t offset)
{
  char tz[12];
  snprintf(tz, sizeof(tz), "UTC%s%d", offset > 0 ? "-" : "+", abs((int) offset));
  setenv("TZ", tz, 1);
  tzset();
}
