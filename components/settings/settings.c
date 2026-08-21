// SPDX-License-Identifier: MIT
#include "settings.h"
#include "timezone_fmt.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *const tag = "Settings";
static const char *nvs_namespace = "zenclock";

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
      // Every read below already falls back to a compiled-in default when nvs_open() fails,
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int8_t settings_get(settings_key_t key)
{
  const settings_desc_t *desc = settings_desc(key);
  if (!desc)
  {
    ESP_LOGE(tag, "settings_get called with an unknown key (%d)", (int) key);
    return 0;
  }

  int8_t val = desc->def;

  nvs_handle_t handle;
  if (nvs_open(nvs_namespace, NVS_READONLY, &handle) == ESP_OK)
  {
    const esp_err_t err = nvs_get_i8(handle, desc->nvs_key, &val);
    switch (err)
    {
    case ESP_OK:
    case ESP_ERR_NVS_NOT_FOUND:
      break;
    case ESP_ERR_NVS_TYPE_MISMATCH:
      // Firmware before the descriptor table stored these as u8. NVS type-checks on read, so
      // the value is unreachable rather than misread, and `val` still holds the default. This
      // is the accepted one-time reset documented in ADR-0006, not a fault.
      ESP_LOGW(tag, "%s was stored by an older build — using default %d", desc->nvs_key, desc->def);
      break;
    default:
      ESP_LOGE(tag, "Error reading %s (%s)", desc->nvs_key, esp_err_to_name(err));
      break;
    }
    nvs_close(handle);
  }

  const int8_t clamped = settings_clamp(key, val);
  if (clamped != val)
  {
    // Deliberately loud: read-clamping exists to rescue a device that already has a bad value
    // on flash, and doing that silently would hide whatever wrote it.
    ESP_LOGW(tag, "%s out of range (%d), clamped to %d", desc->nvs_key, val, clamped);
  }
  return clamped;
}

bool settings_get_bool(settings_key_t key)
{
  return settings_get(key) != 0;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void settings_set(settings_key_t key, int val)
{
  const settings_desc_t *desc = settings_desc(key);
  if (!desc)
  {
    ESP_LOGE(tag, "settings_set called with an unknown key (%d)", (int) key);
    return;
  }

  const int8_t clamped = settings_clamp(key, val);

  nvs_handle_t handle;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "Error opening NVS handle (%s)", esp_err_to_name(err));
    return;
  }

  err = nvs_set_i8(handle, desc->nvs_key, clamped);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "Error saving %s (%s)", desc->nvs_key, esp_err_to_name(err));
  }
  else
  {
    err = nvs_commit(handle);
    if (err != ESP_OK)
    {
      ESP_LOGE(tag, "Error committing NVS (%s)", esp_err_to_name(err));
    }
    else
    {
      ESP_LOGI(tag, "%s = %d", desc->nvs_key, clamped);
    }
  }
  nvs_close(handle);
}

uint32_t settings_get_sleep_seconds(void)
{
  return settings_sleep_seconds(settings_get(SETTINGS_KEY_SLEEP_H), settings_get(SETTINGS_KEY_SLEEP_M),
                                settings_get(SETTINGS_KEY_SLEEP_S));
}

void settings_apply_timezone(int8_t offset)
{
  // Clamped here too, not just on read: this is the only function that actually reaches
  // setenv("TZ", ...), and it is public. An out-of-range offset would build a TZ string newlib
  // cannot parse and silently falls back to UTC from — the unexplainable-wrong-clock symptom
  // this clamp exists to prevent, arrived at through the one path that bypasses NVS entirely.
  offset = settings_clamp(SETTINGS_KEY_TZ_OFFSET, offset);

  char tz[12];
  timezone_fmt(tz, sizeof(tz), offset);
  setenv("TZ", tz, 1);
  tzset();
}
