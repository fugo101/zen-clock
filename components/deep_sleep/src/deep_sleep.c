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

// Backlight fade before sleeping, plus a margin so the LEDC ramp has finished. The wait is polled
// rather than a single delay so a button press can still call the whole thing off.
#define FADE_MS         1500
#define FADE_MARGIN_MS  100
#define CANCEL_POLL_MS  50
#define RESTORE_FADE_MS 300

static esp_timer_handle_t s_timer = NULL;
static TaskHandle_t s_task = NULL;
static uint64_t s_timeout_us = 0;
static deep_sleep_inhibit_cb_t s_inhibit_cb = NULL;

// Set by deep_sleep_cancel() from any task, read by the sleep task while it fades.
// Only deep_sleep_trigger() clears it, and that is what makes the ordering work:
// on_button_press() calls cancel() before it can reach trigger(), so the two-button sleep combo
// still sleeps, while a stray press during a fade still calls it off.
static volatile bool s_cancel = false;

static bool sleep_inhibited(void)
{
  return s_inhibit_cb != NULL && s_inhibit_cb();
}

// Returns true when the fade ran to completion, false if it was cancelled part-way.
static bool fade_out_unless_cancelled(void)
{
  for (int waited = 0; waited < FADE_MS + FADE_MARGIN_MS; waited += CANCEL_POLL_MS)
  {
    vTaskDelay(pdMS_TO_TICKS(CANCEL_POLL_MS));
    if (s_cancel)
    {
      return false;
    }
  }
  return true;
}

static void
sleep_task_fn(void *arg) // NOLINT(readability-non-const-parameter, readability-function-cognitive-complexity)
{
  (void) arg;

  // Loops: a sleep request can now be declined or cancelled, and the task has to survive that to
  // handle the next one. (esp_deep_sleep_start() is noreturn, so the successful path never
  // reaches the bottom of the loop.)
  for (;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // A request still pending while a cancel is outstanding predates that cancel, and is void:
    // deep_sleep_trigger() clears s_cancel, so anything genuinely newer would have cleared it.
    // This is load-bearing since the buttons moved to espressif/button. Both buttons now fire
    // LONG within one 10ms tick of the combo instead of the second one's events being swallowed
    // by the old driver's poll-while-held loop, so the combo reaches deep_sleep_trigger() twice.
    // Without this, a fade called off by a stray press was followed immediately by the second,
    // stale request: the screen dipped toward black and came back ~250ms later, visibly.
    if (s_cancel)
    {
      continue;
    }

    if (sleep_inhibited())
    {
      // Re-arm, or the device asks once and never again: the inactivity timer is one-shot and is
      // only restarted by button presses, which is exactly what is not happening while the user
      // is holding a phone up to the QR code.
      ESP_LOGI(tag, "sleep inhibited — rescheduling");
      deep_sleep_reset_timer();
      continue;
    }

    ESP_LOGI(tag, "entering deep sleep");
    // Read before the fade starts, while it still reports the level the user chose.
    const uint8_t prev_brightness = bsp_display_get_brightness();
    bsp_display_set_brightness(0, FADE_MS);

    if (!fade_out_unless_cancelled())
    {
      ESP_LOGI(tag, "sleep cancelled — restoring display");
      bsp_display_set_brightness(prev_brightness, RESTORE_FADE_MS);
      deep_sleep_reset_timer();
      continue;
    }

    bsp_display_power_off();

    esp_sleep_enable_ext1_wakeup_io(WAKEUP_PIN_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // These pull-ups are load-bearing, not decoration. "This function does not modify pin
    // configuration" (esp_sleep.h:302) means the ext1 call installs the wake condition and
    // nothing else; the digital pull-ups bsp_buttons.c configured live in the IO mux, which is
    // unpowered during deep sleep. Without re-asserting them in the RTC domain both wake pins
    // float, ANY_LOW is satisfied immediately, and the device wakes the instant it sleeps —
    // which looks exactly like a crash-and-reboot from the outside.
    //
    // No rtc_gpio_init() beforehand: Espressif's own deep_sleep example calls these two directly,
    // and ext1 puts the pads into RTC mode itself right before sleeping.
    rtc_gpio_pullup_en(PIN_BTN_BOOT);
    rtc_gpio_pulldown_dis(PIN_BTN_BOOT);
    rtc_gpio_pullup_en(PIN_BTN_IO14);
    rtc_gpio_pulldown_dis(PIN_BTN_IO14);

    esp_deep_sleep_start();
  }
}

static void inactivity_cb(void *arg) // NOLINT(readability-non-const-parameter)
{
  (void) arg;
  deep_sleep_trigger();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
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
    // Clearing here, and only here, resolves the one ambiguous ordering: on_button_press() calls
    // deep_sleep_cancel() on every event before it can decide the two-button combo means sleep.
    s_cancel = false;
    xTaskNotifyGive(s_task);
  }
}

void deep_sleep_cancel(void)
{
  s_cancel = true;
}

void deep_sleep_register_inhibit_cb(deep_sleep_inhibit_cb_t cb)
{
  s_inhibit_cb = cb;
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
