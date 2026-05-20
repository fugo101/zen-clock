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

// ============================================================
// Init (called from bsp_display.c during bsp_display_init)
// ============================================================
void bsp_battery_setup(void)
{
  ESP_LOGI(tag, "Configuring battery monitor...");

  // ADC unit
  // NOLINTNEXTLINE(*-invalid-enum-default-initialization)
  const adc_oneshot_unit_init_cfg_t adc_cfg = {
      .unit_id = BAT_ADC_UNIT,
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_cfg, &s_adc_handle));

  // ADC channel
  const adc_oneshot_chan_cfg_t chan_cfg = {
      .bitwidth = ADC_BITWIDTH_DEFAULT,
      .atten = ADC_ATTEN_DB_12,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CHANNEL, &chan_cfg));

  // Calibration
  const adc_cali_curve_fitting_config_t cali_cfg = {
      .unit_id = BAT_ADC_UNIT,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali_handle));

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

bool bsp_battery_usb_connected(void)
{
  int mv = bsp_battery_get_voltage();
  if (mv < 0)
  {
    return false;
  }
  return mv >= USB_THRESHOLD_MV;
}
