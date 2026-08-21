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
#include "adc_battery_estimation.h"

static const char *const tag = "bsp_battery";

// Battery ADC channel (GPIO4 = ADC1_CH3)
#define BAT_ADC_UNIT    ADC_UNIT_1
#define BAT_ADC_CHANNEL ADC_CHANNEL_3

// USB detection: voltage >= 4600mV (after ×2 correction) indicates USB power.
// Battery max = 4200mV (4.2V full charge × 2), USB ~4900–5000mV — threshold sits 400mV below USB min.
#define USB_THRESHOLD_MV 4600

// GPIO4 sits on a 100kΩ/100kΩ divider (LilyGo schematic) — equal resistors, only the ratio
// matters to adc_battery_estimation, so any equal pair works; kept at the real board value.
#define BAT_DIVIDER_UPPER_OHM 100000.0f
#define BAT_DIVIDER_LOWER_OHM 100000.0f

// ============================================================
// Static handles
// ============================================================
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_cali_handle;
static adc_battery_estimation_handle_t s_batt_est_handle;

// Mirrors *usb from the most recent bsp_battery_read() — read by charging_usb_cb() below.
// bsp_battery_read() always sets this before calling adc_battery_estimation_get_capacity(), so
// the callback sees the current reading's USB state, not a stale one.
static bool s_is_on_usb;

// Feeds the library's capacity-monotonicity clamp with our existing immediate voltage-threshold
// signal instead of leaving it on the library's own software trend estimation. The trend
// estimator only updates every ~200s (10 samples × 20s) — a USB session shorter than that leaves
// is_charging stuck at its pre-plug-in value the whole time, and get_capacity()'s "discharging:
// capacity must not increase" clamp then pins the reported % at its pre-charge value even after
// the real (now higher) voltage is back to being read post-unplug. Wiring our own threshold check
// here means charging state flips the instant USB does, so that stuck-low case can't happen.
static bool charging_usb_cb(void *user_data)
{
  (void) user_data;
  return s_is_on_usb;
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
  // bsp_battery_read() reports -1, which the UI already renders as "N/A".

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

  // Percentage estimation — hands our own already-initialized ADC + calibration handles to the
  // library via .external (not .internal): adc_battery_estimation has no voltage-only getter, so
  // bsp_battery_read() still needs to read this same channel itself for USB detection (see
  // read_voltage_mv() below), and ESP-IDF won't let two independent adc_oneshot_unit_handle_t own
  // the same physical unit. Left NULL on failure — bsp_battery_read() checks and reports pct as
  // -1, same "N/A" degrade as above.
  adc_battery_estimation_t est_cfg = {0};
  est_cfg.external.adc_handle = s_adc_handle;
  est_cfg.external.adc_cali_handle = s_adc_cali_handle;
  est_cfg.adc_channel = BAT_ADC_CHANNEL;
  est_cfg.upper_resistor = BAT_DIVIDER_UPPER_OHM;
  est_cfg.lower_resistor = BAT_DIVIDER_LOWER_OHM;
  // See charging_usb_cb() above and docs/adr/0001-battery-percentage-source.md — this is our
  // existing immediate USB signal, not the library's own (much slower) trend estimation.
  est_cfg.charging_detect_cb = charging_usb_cb;
  s_batt_est_handle = adc_battery_estimation_create(&est_cfg);
  if (!s_batt_est_handle)
  {
    ESP_LOGE(tag, "adc_battery_estimation_create failed — battery percentage will read N/A");
  }

  ESP_LOGI(tag, "Battery monitor ready (GPIO%d, ADC1_CH%d)", PIN_BAT_ADC, BAT_ADC_CHANNEL);
}

// Raw single ADC conversion — the only remaining consumer is the USB-threshold check in
// bsp_battery_read() below (adc_battery_estimation has no external usb/charge-detect pin to read
// on this board). Not a public value: no UI shows raw mV anymore (see
// docs/adr/0001-battery-percentage-source.md), but it's still logged at debug level so it's
// available over `pio device monitor` if the curve ever needs real calibration.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static int read_voltage_mv(void)
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

  const int mv = voltage * 2; // ×2 for resistor divider
  ESP_LOGD(tag, "Raw battery voltage: %dmV", mv);
  return mv;
}

// ============================================================
// Public API
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void bsp_battery_read(int *pct, bool *usb)
{
  const int reading = read_voltage_mv();
  const bool is_usb = reading >= 0 && reading >= USB_THRESHOLD_MV;
  s_is_on_usb = is_usb; // must be set before adc_battery_estimation_get_capacity() below
  if (usb)
  {
    *usb = is_usb;
  }
  if (pct)
  {
    *pct = -1;
    if (reading >= 0 && s_batt_est_handle)
    {
      float capacity = 0.0f;
      if (adc_battery_estimation_get_capacity(s_batt_est_handle, &capacity) == ESP_OK)
      {
        int result = (int) lroundf(capacity);
        if (result < 0)
        {
          result = 0;
        }
        if (result > 100)
        {
          result = 100;
        }
        *pct = result;
      }
      else
      {
        ESP_LOGE(tag, "adc_battery_estimation_get_capacity failed");
      }
    }
  }
}
