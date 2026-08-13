/*
 * SPDX-FileCopyrightText: © 2025 Hiruna Wijesinghe <hiruna.kawinda@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#include "lcd_backlight.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *const tag = "LCD_BACKLIGHT";

// Use 10-bit resolution for smooth fading (1024 levels)
#define LCD_BACKLIGHT_DUTY_RES         LEDC_TIMER_10_BIT
#define LCD_BACKLIGHT_MAX_DUTY         ((1 << LCD_BACKLIGHT_DUTY_RES) - 1)
#define LCD_BACKLIGHT_MIN_FADE_TIME_MS 50
#define LCD_BACKLIGHT_MAX_FADE_TIME_MS 10000

struct [[maybe_unused]] lcd_backlight_dev_t
{
  ledc_channel_t channel;
  ledc_timer_t timer_num;
  uint8_t brightness_pct;
};

static uint32_t brightness_pct_to_duty_cycle(uint8_t brightness_pct)
{
  if (brightness_pct > 100)
  {
    brightness_pct = 100;
  }
  return (uint32_t) brightness_pct * LCD_BACKLIGHT_MAX_DUTY / 100;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
esp_err_t lcd_backlight_init(const lcd_backlight_config_t *config, lcd_backlight_handle_t *out_hdl)
{
  esp_err_t err = ESP_FAIL;
  if (config == NULL || out_hdl == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  struct lcd_backlight_dev_t *dev = malloc(sizeof(struct lcd_backlight_dev_t));
  if (!dev)
  {
    return ESP_ERR_NO_MEM;
  }

  const ledc_timer_config_t timer_cfg = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                         .duty_resolution = LCD_BACKLIGHT_DUTY_RES,
                                         .timer_num = config->timer_num,
                                         .freq_hz = config->pwm_freq_hz,
                                         .clk_cfg = LEDC_AUTO_CLK};
  err = ledc_timer_config(&timer_cfg);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "ledc_timer_config error");
    goto cleanup;
  }

  const ledc_channel_config_t channel_cfg = {.gpio_num = config->gpio_num,
                                             .speed_mode = LEDC_LOW_SPEED_MODE,
                                             .channel = config->channel_num,
                                             .timer_sel = config->timer_num,
                                             .duty = 0,
                                             .hpoint = 0};
  err = ledc_channel_config(&channel_cfg);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "ledc_channel_config error");
    goto cleanup;
  }

  // Enable fade functionality
  err = ledc_fade_func_install(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
  { // ESP_ERR_INVALID_STATE = Fade function already installed
    ESP_LOGE(tag, "ledc_fade_func_install error");
    goto cleanup;
  }

  dev->channel = config->channel_num;
  dev->timer_num = config->timer_num;
  dev->brightness_pct = 0;

  *out_hdl = dev;
  return ESP_OK;

cleanup:
  free(dev);
  return err;
}

esp_err_t
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
lcd_backlight_set_brightness(const lcd_backlight_handle_t handle, uint8_t percent, const uint32_t fade_time_ms)
{
  if (!handle)
  {
    return ESP_ERR_INVALID_ARG;
  }

  if (percent > 100)
  {
    percent = 100;
  }

  handle->brightness_pct = percent;
  const uint32_t duty_cycle = brightness_pct_to_duty_cycle(percent);
  esp_err_t err = ESP_FAIL;

  uint32_t fade_time = fade_time_ms;
  if (fade_time > LCD_BACKLIGHT_MAX_FADE_TIME_MS)
  {
    fade_time = LCD_BACKLIGHT_MAX_FADE_TIME_MS;
  }

  if (fade_time >= LCD_BACKLIGHT_MIN_FADE_TIME_MS)
  {
    err = ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, handle->channel, duty_cycle, fade_time, LEDC_FADE_NO_WAIT);
    if (err != ESP_OK)
    {
      ESP_LOGE(tag, "ledc_set_fade_time_and_start error");
      return err;
    }
  }
  else
  {
    err = ledc_set_duty(LEDC_LOW_SPEED_MODE, handle->channel, duty_cycle);
    if (err != ESP_OK)
    {
      ESP_LOGE(tag, "ledc_set_duty error");
      return err;
    }
    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, handle->channel);
    if (err != ESP_OK)
    {
      ESP_LOGE(tag, "ledc_update_duty error");
      return err;
    }
  }

  return ESP_OK;
}

uint8_t lcd_backlight_get_brightness(lcd_backlight_handle_t handle)
{
  if (!handle)
  {
    return 0;
  }
  return handle->brightness_pct;
}
