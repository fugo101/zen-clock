/*
 * SPDX-FileCopyrightText: © 2025 Hiruna Wijesinghe <hiruna.kawinda@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <driver/ledc.h>
#include "esp_err.h"

  /**
   * @brief lcd_backlight handle
   */
  typedef struct lcd_backlight_dev_t *lcd_backlight_handle_t;

  /**
   * @brief lcd_backlight configuration
   */
  typedef struct
  {
    int gpio_num;               /*!< GPIO pin for backlight */
    uint32_t pwm_freq_hz;       /*!< PWM frequency in Hz (e.g., 5000) */
    ledc_timer_t timer_num;     /*!< LEDC Timer (e.g., LEDC_TIMER_0) */
    ledc_channel_t channel_num; /*!< LEDC Channel (e.g., LEDC_CHANNEL_0) */
  } lcd_backlight_config_t;

  /**
   * @brief Initialize lcd_backlight driver
   *
   * @param config Pointer to the backlight configuration struct
   * @param out_hdl Pointer to the output lcd_backlight handle
   *
   * @return
   *      - ESP_OK Success
   *      - ESP_ERR_INVALID_ARG Parameter error
   *      - ESP_ERR_NO_MEM Out of memory
   *      - ESP_FAIL Other failures
   */
  esp_err_t lcd_backlight_init(const lcd_backlight_config_t *config, lcd_backlight_handle_t *out_hdl);

  /**
   * @brief Set brightness using percentage value (0-100)
   *
   * @param handle lcd_backlight handle
   * @param percent Brightness percentage value, range is [0..100]
   * @param fade_time_ms Duration of the brightness change/fade. If 0, brightness changes immediately
   *
   * @return
   *      - ESP_OK Success
   *      - ESP_ERR_INVALID_ARG Parameter error
   *      - ESP_FAIL Other failures
   */
  esp_err_t lcd_backlight_set_brightness(lcd_backlight_handle_t handle, uint8_t percent, uint32_t fade_time_ms);

  /**
   * @brief Get current brightness level as a percentage
   *
   * @param handle lcd_backlight handle
   *
   * @return
   *      - Current percentage value of brightness
   *      - 0 is also returned if handle is NULL
   */
  uint8_t lcd_backlight_get_brightness(lcd_backlight_handle_t handle);

#ifdef __cplusplus
}
#endif