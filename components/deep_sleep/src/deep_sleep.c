// SPDX-License-Identifier: MIT
#include "deep_sleep.h"
#include "bsp.h"
#include "board_config.h"
#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *const tag = "deep_sleep";

// Wake on either button press (active-low)
#define WAKEUP_PIN_MASK ((1ULL << PIN_BTN_BOOT) | (1ULL << PIN_BTN_IO14))

static esp_timer_handle_t s_timer = NULL;
static TaskHandle_t s_task = NULL;
static uint64_t s_timeout_us = 0;

static void sleep_task_fn(void *arg) // NOLINT(readability-non-const-parameter)
{
  (void) arg;
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

  ESP_LOGI(tag, "entering deep sleep");
  bsp_display_set_brightness(0, 1500);
  vTaskDelay(pdMS_TO_TICKS(1600));

  esp_sleep_enable_ext1_wakeup(WAKEUP_PIN_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  rtc_gpio_pullup_en(PIN_BTN_BOOT);
  rtc_gpio_pulldown_dis(PIN_BTN_BOOT);
  rtc_gpio_pullup_en(PIN_BTN_IO14);
  rtc_gpio_pulldown_dis(PIN_BTN_IO14);
  esp_deep_sleep_start();
}

static void inactivity_cb(void *arg) // NOLINT(readability-non-const-parameter)
{
  (void) arg;
  deep_sleep_trigger();
}

void deep_sleep_init(uint32_t timeout_s)
{
  // Both creations are checked: on failure the handle stays NULL, every other entry point in
  // this file already guards on it, and the device simply never sleeps — which is a far better
  // outcome for a mains-or-battery clock than a crash inside FreeRTOS or esp_timer.
  if (xTaskCreate(sleep_task_fn, "deep_sleep", 2048, NULL, 4, &s_task) != pdPASS)
  {
    ESP_LOGE(tag, "Failed to create sleep task — deep sleep disabled");
    s_task = NULL;
    return;
  }

  const esp_timer_create_args_t args = {
      .callback = inactivity_cb,
      .name = "sleep_tmr",
      .arg = NULL,
      .dispatch_method = ESP_TIMER_TASK,
  };
  const esp_err_t ret = esp_timer_create(&args, &s_timer);
  if (ret != ESP_OK)
  {
    // Manual sleep (both buttons, or Settings → Sleep Now) still works; only the inactivity
    // timer is lost, so say exactly that rather than claiming deep sleep is gone.
    ESP_LOGE(tag, "esp_timer_create(sleep_tmr) failed: %s — auto-sleep disabled, manual sleep still works",
             esp_err_to_name(ret));
    s_timer = NULL;
    return;
  }

  s_timeout_us = (uint64_t) timeout_s * 1000000ULL;
  if (timeout_s > 0)
  {
    esp_timer_start_once(s_timer, s_timeout_us);
    ESP_LOGI(tag, "auto-sleep in %lu s", (unsigned long) timeout_s);
  }
  else
  {
    ESP_LOGI(tag, "auto-sleep disabled");
  }
}

void deep_sleep_reset_timer(void)
{
  if (!s_timer || s_timeout_us == 0)
  {
    return;
  }
  // stop is safe even if timer already fired (returns ESP_ERR_INVALID_STATE, ignored)
  esp_timer_stop(s_timer);
  esp_timer_start_once(s_timer, s_timeout_us);
}

void deep_sleep_trigger(void)
{
  if (s_task)
  {
    xTaskNotifyGive(s_task);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void deep_sleep_update_timeout(const uint32_t timeout_s)
{
  if (!s_timer)
  {
    return;
  }
  esp_timer_stop(s_timer);
  s_timeout_us = (uint64_t) timeout_s * 1000000ULL;
  if (timeout_s > 0)
  {
    esp_timer_start_once(s_timer, s_timeout_us);
    ESP_LOGI(tag, "auto-sleep updated: %lu s", (unsigned long) timeout_s);
  }
  else
  {
    ESP_LOGI(tag, "auto-sleep disabled");
  }
}
