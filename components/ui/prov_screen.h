// SPDX-License-Identifier: MIT
// ZenClock — BLE Provisioning overlay screen

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Show full-screen BLE provisioning overlay with QR code.
   * Must be called while holding the LVGL port lock.
   * @param device_name BLE advertisement name (e.g. "PROV_ZenClock_A1B2")
   * @param password    SRP6a password shown on screen (e.g. "A1B2C3D4")
   */
  void prov_screen_show(const char *device_name, const char *password);

  /**
   * @brief Remove the provisioning overlay, revealing the clock screen.
   * Must be called while holding the LVGL port lock.
   *
   * Hiding the overlay does NOT stop provisioning — BLE keeps advertising and a phone can still
   * complete the flow. It only gives the screen back so the clock stays usable.
   */
  void prov_screen_hide(void);

  /**
   * @brief Whether the overlay is currently on screen.
   * Must be called while holding the LVGL port lock.
   */
  bool prov_screen_is_visible(void);

#ifdef __cplusplus
}
#endif
