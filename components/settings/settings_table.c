// SPDX-License-Identifier: MIT
#include "settings_table.h"

#include <stddef.h>

// Indexed by settings_key_t. The NVS key strings are frozen: they name data already on every
// deployed device, so renaming one silently reverts that setting to its default.
static const settings_desc_t s_desc[SETTINGS_KEY_COUNT] = {
    [SETTINGS_KEY_NONE] = {.nvs_key = NULL},
    [SETTINGS_KEY_THEME_LIGHT] = {.nvs_key = "theme_light", .def = 0, .min = 0, .max = 1},
    [SETTINGS_KEY_BRIGHTNESS] = {.nvs_key = "brightness", .def = 100, .min = SETTINGS_BRIGHTNESS_MIN, .max = 100},
    [SETTINGS_KEY_TIME_FMT] = {.nvs_key = "time_fmt", .def = 1, .min = 0, .max = 1, .invert = true},
    [SETTINGS_KEY_SHOW_SECS] = {.nvs_key = "show_secs", .def = 1, .min = 0, .max = 1, .invert = true},
    [SETTINGS_KEY_TZ_OFFSET] = {.nvs_key = "tz_offset", .def = 7, .min = SETTINGS_TZ_MIN, .max = SETTINGS_TZ_MAX},
    [SETTINGS_KEY_SLEEP_H] = {.nvs_key = "sleep_h", .def = 0, .min = 0, .max = 23},
    [SETTINGS_KEY_SLEEP_M] = {.nvs_key = "sleep_m", .def = 0, .min = 0, .max = 59},
    [SETTINGS_KEY_SLEEP_S] = {.nvs_key = "sleep_s", .def = 0, .min = 0, .max = 59},
};

const settings_desc_t *settings_desc(settings_key_t key)
{
  if (key <= SETTINGS_KEY_NONE || key >= SETTINGS_KEY_COUNT)
  {
    return NULL;
  }
  return &s_desc[key];
}

int8_t settings_clamp(settings_key_t key, int val)
{
  const settings_desc_t *desc = settings_desc(key);
  if (!desc)
  {
    return 0;
  }
  if (val < desc->min)
  {
    return desc->min;
  }
  if (val > desc->max)
  {
    return desc->max;
  }
  return (int8_t) val;
}

int settings_option_index(settings_key_t key, int val)
{
  const settings_desc_t *desc = settings_desc(key);
  if (!desc || !desc->invert)
  {
    return val;
  }
  return val ? 0 : 1;
}

uint32_t settings_sleep_seconds(int hours, int minutes, int seconds)
{
  return (uint32_t) hours * 3600 + (uint32_t) minutes * 60 + (uint32_t) seconds;
}
