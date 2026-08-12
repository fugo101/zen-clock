// SPDX-License-Identifier: MIT
// Deep sleep manager for ZenClock.
// Handles inactivity timeout, manual trigger, and wakeup configuration.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Initialize deep sleep manager.
   *
   * Creates the sleep task and inactivity timer. Call once after settings_init().
   *
   * @param timeout_s Inactivity timeout in seconds. 0 = disabled (no auto-sleep).
   */
  void deep_sleep_init(uint32_t timeout_s);

  /**
   * @brief Reset the inactivity countdown.
   *
   * Call on every button event to prevent auto-sleep while the user is active.
   * No-op if deep sleep is disabled (timeout_s == 0).
   */
  void deep_sleep_reset_timer(void);

  /**
   * @brief Predicate asked before sleeping. Return true to decline this request.
   *
   * The countdown is re-armed when a request is declined, so the device asks again later rather
   * than staying awake for good.
   */
  typedef bool (*deep_sleep_inhibit_cb_t)(void);

  /**
   * @brief Register the predicate consulted before each sleep. NULL (default) never inhibits.
   *
   * Exists so this component does not have to know about provisioning, WiFi or anything else
   * that might want to hold sleep off — the application supplies the policy.
   */
  void deep_sleep_register_inhibit_cb(deep_sleep_inhibit_cb_t cb);

  /**
   * @brief Trigger sleep sequence immediately.
   *
   * Notifies the sleep task to begin the fade-out and deep sleep entry.
   * Safe to call from any task or timer callback. Clears any pending cancel.
   */
  void deep_sleep_trigger(void);

  /**
   * @brief Call off a sleep that is already fading out.
   *
   * Has no effect once esp_deep_sleep_start() has been reached, and none at all if no sleep is
   * pending. Call it on every button event: without it, a device that hits its inactivity
   * timeout sleeps even as the user is pressing buttons at it.
   */
  void deep_sleep_cancel(void);

  /**
   * @brief Update the inactivity timeout without reinitializing.
   *
   * Call when the user changes sleep H/M/S in settings.
   * Restarts the countdown with the new value. 0 = disable auto-sleep.
   *
   * @param timeout_s New timeout in seconds.
   */
  void deep_sleep_update_timeout(uint32_t timeout_s);

#ifdef __cplusplus
}
#endif
