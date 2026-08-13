// SPDX-License-Identifier: MIT
// ZenClock — Pure TZ string formatting, split out of settings.c so it can build for `native`
// (settings.c itself pulls in esp_log.h/nvs.h and can't).

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /** Builds a POSIX TZ string like "UTC-7" from a UTC offset in hours. POSIX sign-inversion
      convention: UTC+7 becomes "UTC-7". buf must be at least 12 bytes. */
  void timezone_fmt(char *buf, size_t buf_size, int8_t offset);

#ifdef __cplusplus
}
#endif
