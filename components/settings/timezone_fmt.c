// SPDX-License-Identifier: MIT
#include "timezone_fmt.h"

#include <stdio.h>
#include <stdlib.h>

void timezone_fmt(char *buf, const size_t buf_size, const int8_t offset)
{
  snprintf(buf, buf_size, "UTC%s%d", offset > 0 ? "-" : "+", abs((int) offset));
}
