// SPDX-License-Identifier: MIT
// ZenClock — BLE Provisioning overlay screen

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Publish "the QR overlay should be up, with this name and password".
   *
   * Callable from any task, with no LVGL lock held. ui.c's reconcile tick builds the overlay on
   * the LVGL task within one tick. Publishing the same intent twice is a no-op.
   *
   * @param device_name BLE advertisement name (e.g. "PROV_ZenClock_A1B2")
   * @param password    SRP6a password shown on screen (e.g. "A1B2C3D4")
   */
  void prov_screen_show(const char *device_name, const char *password);

  /**
   * @brief Publish "the QR overlay should be down", revealing the clock screen.
   * Callable from any task, with no LVGL lock held.
   *
   * Hiding the overlay does NOT stop provisioning — BLE keeps advertising and a phone can still
   * complete the flow. It only gives the screen back so the clock stays usable.
   */
  void prov_screen_hide(void);

  /**
   * @brief Whether the overlay is meant to be up.
   *
   * Reports the published intent, not the widget, and takes no LVGL lock — so the deep-sleep
   * inhibit and the nav-action guard can both ask from their own tasks. Between a publish and the
   * next reconcile tick this leads what is on screen by up to one tick, which is what the callers
   * want: an input arriving in that window belongs to the overlay, not to the screen behind it.
   */
  bool prov_screen_is_visible(void);

  /**
   * @brief Build or tear down the overlay to match the published intent.
   *
   * Called from ui.c's reconcile tick. MUST be called with the LVGL port lock held.
   */
  void prov_screen_reconcile(void);

#ifdef __cplusplus
}
#endif
