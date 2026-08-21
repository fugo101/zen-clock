// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "settings_table.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Initialize the NVS flash.
   * Must be called early in app_main before reading/writing settings.
   *
   * Never aborts: if NVS cannot be initialized the device runs on compiled-in defaults and
   * nothing persists, which is preferable to a boot loop on a worn flash partition.
   */
  void settings_init(void);

  /**
   * @brief Read one setting from NVS.
   *
   * Returns the descriptor's default when the key is absent, when NVS cannot be opened, or when
   * the stored value has a different NVS type than the one this build writes. The result is
   * always clamped into the descriptor's range — a value outside it, stored by an earlier build,
   * is corrected on the way out and logged, so whatever wrote it stays findable (ADR-0004).
   */
  int8_t settings_get(settings_key_t key);

  /** @brief Read a boolean setting. Convenience over settings_get() != 0. */
  bool settings_get_bool(settings_key_t key);

  /**
   * @brief Clamp a value into its setting's range and write it to NVS.
   *
   * A failed write is logged and swallowed — an unwritable settings partition must not take the
   * clock down with it.
   */
  void settings_set(settings_key_t key, int val);

  /** @brief Total stored auto-sleep timeout in seconds. */
  uint32_t settings_get_sleep_seconds(void);

  /**
   * @brief Apply timezone offset to the system (setenv + tzset).
   * Call on boot and whenever the setting changes.
   * @param offset Offset, clamped into SETTINGS_TZ_MIN..SETTINGS_TZ_MAX before being applied.
   */
  void settings_apply_timezone(int8_t offset);

#ifdef __cplusplus
}
#endif
