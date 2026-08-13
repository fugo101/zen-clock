// SPDX-License-Identifier: MIT
// BSP Battery — ADC init, voltage reading, percentage calculation

#include "bsp.h"
#include "bsp_priv.h"
#include "board_config.h"

#include <stdio.h>
#include <math.h>
#include <esp_log.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *const tag = "bsp_battery";

// Battery ADC channel (GPIO4 = ADC1_CH3)
#define BAT_ADC_UNIT    ADC_UNIT_1
#define BAT_ADC_CHANNEL ADC_CHANNEL_3

// USB detection: voltage >= 4600mV (after ×2 correction) indicates USB power.
// Battery max = 4200mV (4.2V full charge × 2), USB ~4900–5000mV — threshold sits 400mV below USB min.
#define USB_THRESHOLD_MV 4600

// ============================================================
// Static handles
// ============================================================
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_cali_handle;

// ============================================================
// Battery percentage curve (float for ESP32-S3 hardware FPU)
// ============================================================
static float volts_to_percentage(float volts)
{
  return 123.0f - (123.0f / powf((1.0f + powf(volts / 3.7f, 80.0f)), 0.165f));
}

// Shared by bsp_battery_get_percentage() and bsp_battery_read() so the two never disagree.
static int percentage_from_mv(int mv)
{
  float pct = volts_to_percentage((float) mv / 1000.0f);
  int result = (int) ceilf(pct);
  if (result < 0)
  {
    result = 0;
  }
  if (result > 100)
  {
    result = 100;
  }
  return result;
}

// ============================================================
// Init (called from bsp_display.c during bsp_display_init)
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void bsp_battery_setup(void)
{
  ESP_LOGI(tag, "Configuring battery monitor...");

  // None of this is ESP_ERROR_CHECK'd. The battery reading is cosmetic — a percentage in the
  // status bar — and aborting here bricks boot for it. The curve-fitting scheme in particular
  // returns ESP_ERR_NOT_SUPPORTED on a chip whose eFuse holds no calibration data, which is a
  // property of that individual part, not a bug. On any failure the handles stay NULL and
  // bsp_battery_get_voltage() reports -1, which the UI already renders as "N/A".

  // ADC unit
  // NOLINTNEXTLINE(*-invalid-enum-default-initialization)
  const adc_oneshot_unit_init_cfg_t adc_cfg = {
      .unit_id = BAT_ADC_UNIT,
  };
  esp_err_t err = adc_oneshot_new_unit(&adc_cfg, &s_adc_handle);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "adc_oneshot_new_unit failed (%s) — battery level unavailable", esp_err_to_name(err));
    s_adc_handle = NULL;
    return;
  }

  // ADC channel
  const adc_oneshot_chan_cfg_t chan_cfg = {
      .bitwidth = ADC_BITWIDTH_DEFAULT,
      .atten = ADC_ATTEN_DB_12,
  };
  err = adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CHANNEL, &chan_cfg);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "adc_oneshot_config_channel failed (%s) — battery level unavailable", esp_err_to_name(err));
    adc_oneshot_del_unit(s_adc_handle);
    s_adc_handle = NULL;
    return;
  }

  // Calibration
  const adc_cali_curve_fitting_config_t cali_cfg = {
      .unit_id = BAT_ADC_UNIT,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali_handle);
  if (err != ESP_OK)
  {
    // Left NULL on purpose: adc_cali_raw_to_voltage() rejects a NULL handle with
    // ESP_ERR_INVALID_ARG, so readings degrade to -1 instead of reporting a wrong voltage.
    ESP_LOGE(tag, "ADC calibration unavailable (%s) — battery level will read N/A", esp_err_to_name(err));
    s_adc_cali_handle = NULL;
    return;
  }

  ESP_LOGI(tag, "Battery monitor ready (GPIO%d, ADC1_CH%d)", PIN_BAT_ADC, BAT_ADC_CHANNEL);
}

// ============================================================
// Public API
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int bsp_battery_get_voltage(void)
{
  if (!s_adc_handle)
  {
    ESP_LOGW(tag, "Battery ADC not initialized");
    return -1;
  }

  int adc_raw, voltage;
  esp_err_t err = adc_oneshot_read(s_adc_handle, BAT_ADC_CHANNEL, &adc_raw);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "ADC read failed: %s", esp_err_to_name(err));
    return -1;
  }

  err = adc_cali_raw_to_voltage(s_adc_cali_handle, adc_raw, &voltage);
  if (err != ESP_OK)
  {
    ESP_LOGE(tag, "ADC calibration failed: %s", esp_err_to_name(err));
    return -1;
  }

  return voltage * 2; // ×2 for resistor divider
}

int bsp_battery_get_percentage(void)
{
  const int mv = bsp_battery_get_voltage();
  if (mv < 0)
  {
    return -1;
  }
  return percentage_from_mv(mv);
}

bool bsp_battery_usb_connected(void)
{
  int mv = bsp_battery_get_voltage();
  if (mv < 0)
  {
    return false;
  }
  return mv >= USB_THRESHOLD_MV;
}

void bsp_battery_read(int *mv, int *pct, bool *usb)
{
  const int reading = bsp_battery_get_voltage(); // single ADC conversion for all three outputs
  if (mv)
  {
    *mv = reading;
  }
  if (pct)
  {
    *pct = reading < 0 ? -1 : percentage_from_mv(reading);
  }
  if (usb)
  {
    *usb = reading >= 0 && reading >= USB_THRESHOLD_MV;
  }
}
