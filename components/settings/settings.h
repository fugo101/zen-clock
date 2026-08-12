// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Lowest brightness the device will ever apply or store, in percent.
 *
 * The backlight is the only way to read the settings screen, so 0% is not a valid setting —
 * it hides the control needed to undo it. Enforced on both read and write in settings.c, and
 * used as the `.min` of the Brightness item in the UI so the two cannot drift apart.
 */
#define SETTINGS_BRIGHTNESS_MIN 10

  /**
   * @brief Initialize the NVS flash.
   * Must be called early in app_main before reading/writing settings.
   *
   * Never aborts: if NVS cannot be initialized the device runs on compiled-in defaults and
   * nothing persists, which is preferable to a boot loop on a worn flash partition.
   */
  void settings_init(void);

  /**
   * @brief Get the stored theme configuration.
   * @return true if light theme, false if dark theme (default).
   */
  bool settings_get_theme_light(void);

  /**
   * @brief Store the theme configuration to NVS.
   * @param is_light true for light theme, false for dark theme.
   */
  void settings_set_theme_light(bool is_light);

  /**
   * @brief Get the stored brightness percentage.
   * @return Brightness SETTINGS_BRIGHTNESS_MIN–100 (default: 100 if not set). A lower value
   *         stored by an older build is clamped up, so the screen can always be read.
   */
  uint8_t settings_get_brightness(void);

  /**
   * @brief Store brightness percentage to NVS.
   * @param percent Brightness, clamped into SETTINGS_BRIGHTNESS_MIN–100.
   */
  void settings_set_brightness(uint8_t percent);

  /**
   * @brief Get auto-sleep timeout hours (0–23). Default 0.
   */
  uint8_t settings_get_sleep_h(void);

  /**
   * @brief Store auto-sleep timeout hours to NVS.
   * @param h Hours 0–23 (clamped if exceeds 23).
   */
  void settings_set_sleep_h(uint8_t h);

  /**
   * @brief Get auto-sleep timeout minutes (0–59). Default 0.
   */
  uint8_t settings_get_sleep_m(void);

  /**
   * @brief Store auto-sleep timeout minutes to NVS.
   * @param m Minutes 0–59 (clamped if exceeds 59).
   */
  void settings_set_sleep_m(uint8_t m);

  /**
   * @brief Get auto-sleep timeout seconds (0–59). Default 0.
   */
  uint8_t settings_get_sleep_s(void);

  /**
   * @brief Store auto-sleep timeout seconds to NVS.
   * @param s Seconds 0–59 (clamped if exceeds 59).
   */
  void settings_set_sleep_s(uint8_t s);

  /**
   * @brief Get time format. true = 24H (default), false = 12H.
   */
  bool settings_get_time_format_24h(void);

  /**
   * @brief Store time format to NVS.
   */
  void settings_set_time_format_24h(bool is_24h);

  /**
   * @brief Get show-seconds setting. true = show (default), false = hide.
   */
  bool settings_get_show_seconds(void);

  /**
   * @brief Store show-seconds setting to NVS.
   */
  void settings_set_show_seconds(bool show);

  /**
   * @brief Get timezone UTC offset (-12..+14). Default 7 (UTC+7).
   */
  int8_t settings_get_timezone_offset(void);

  /**
   * @brief Store timezone UTC offset to NVS.
   */
  void settings_set_timezone_offset(int8_t offset);

  /**
   * @brief Apply timezone offset to the system (setenv + tzset).
   * Call on boot and whenever the setting changes.
   */
  void settings_apply_timezone(int8_t offset);

#ifdef __cplusplus
}
#endif
