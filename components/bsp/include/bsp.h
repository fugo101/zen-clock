// SPDX-License-Identifier: MIT
// ZenClock BSP — Board Support Package for LilyGo T-Display-S3

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_lvgl_port.h"

  // ============================================================
  // Display Init & Control
  // ============================================================

  /**
   * @brief Initialize LCD hardware and LVGL display port.
   *
   * Initializes I80 bus, ST7789 panel, battery ADC, backlight,
   * and registers the display with LVGL via esp_lvgl_port.
   *
   * @param disp_handle  Output: LVGL display handle
   * @param backlight_on If true, turn on backlight immediately at 100%
   */
  void bsp_display_init(lv_disp_t **disp_handle, bool backlight_on);

  /**
   * @brief Set LCD brightness as percentage with optional fade.
   *
   * @param percent      Brightness 0-100%
   * @param fade_time_ms Fade duration in ms (0 = instant)
   */
  void bsp_display_set_brightness(uint8_t percent, uint32_t fade_time_ms);

  /**
   * @brief Get current brightness percentage.
   */
  uint8_t bsp_display_get_brightness(void);

  /**
   * @brief Cut power to the LCD and latch the rail off across deep sleep.
   *
   * Call immediately before esp_deep_sleep_start(); the latch is released on the next boot by
   * bsp_display_init(). Fade the backlight out first — this kills the panel rail outright.
   */
  void bsp_display_power_off(void);

  // ============================================================
  // Battery Monitoring
  // ============================================================

  /**
   * @brief Get battery voltage in millivolts (×2 corrected for divider).
   */
  int bsp_battery_get_voltage(void);

  /**
   * @brief Get battery percentage (0-100).
   */
  int bsp_battery_get_percentage(void);

  /**
   * @brief Check if USB power is connected based on voltage reading.
   */
  bool bsp_battery_usb_connected(void);

  /**
   * @brief Read voltage, percentage and USB status from a single ADC conversion.
   *
   * bsp_battery_get_voltage/_get_percentage/_usb_connected() each trigger their own fresh ADC
   * conversion; calling more than one per refresh both wastes conversions and — because C leaves
   * argument evaluation order unspecified — can print a percentage and a voltage that came from
   * two different samples and disagree with each other. Use this instead when more than one of
   * the three values is needed at once. Any out pointer may be NULL. On ADC failure, *mv and *pct
   * are set to -1 and *usb to false, matching the individual getters' failure behavior.
   */
  void bsp_battery_read(int *mv, int *pct, bool *usb);

  // ============================================================
  // Button Input
  // ============================================================

#define BSP_BTN_BOOT  0 // BOOT button (GPIO0)
#define BSP_BTN_IO14  1 // Side button (GPIO14)
#define BSP_BTN_COUNT 2

  /**
   * @brief Button event types (timing-based detection).
   */
  typedef enum
  {
    BSP_BTN_SHORT,     ///< Released within < 800ms
    BSP_BTN_LONG,      ///< Held ≥ 800ms (fires while still held)
    BSP_BTN_EMERGENCY, ///< Held ≥ 3000ms (IO14 only, fires while still held)
  } bsp_btn_event_t;

  /**
   * @brief Button event callback type.
   *
   * @param btn_id  BSP_BTN_BOOT or BSP_BTN_IO14
   * @param event   BSP_BTN_SHORT, BSP_BTN_LONG, or BSP_BTN_EMERGENCY
   */
  typedef void (*bsp_button_cb_t)(int btn_id, bsp_btn_event_t event);

  /**
   * @brief Initialize buttons and start monitoring task.
   *
   * @param callback Function called on button events (short/long/emergency)
   */
  void bsp_buttons_init(bsp_button_cb_t callback);

#ifdef __cplusplus
}
#endif
