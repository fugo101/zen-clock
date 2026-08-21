// SPDX-License-Identifier: MIT
// ZenClock — Retry backoff policy (see backoff.h).

#include "backoff.h"

uint32_t backoff_next_s(const uint32_t current_s, const bool armed)
{
  if (!armed)
  {
    return current_s;
  }
  const uint32_t doubled = current_s * 2U;
  return (doubled > BACKOFF_MAX_S) ? BACKOFF_MAX_S : doubled;
}
