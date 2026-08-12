// SPDX-License-Identifier: MIT
// BSP Buttons — GPIO interrupt + timing-based press detection
//
// Detection model:
//   - Short press:  released within < BTN_LONG_MS (800ms)
//   - Long press:   held >= BTN_LONG_MS (fires once while held)
//   - Emergency:    IO14 held >= BTN_EMERGENCY_MS (3000ms, fires once)

#include "bsp.h"
#include "board_config.h"

#include <esp_log.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *const tag = "bsp_buttons";

// ============================================================
// Timing constants
// ============================================================
#define BTN_DEBOUNCE_MS  50
#define BTN_LONG_MS      800
#define BTN_EMERGENCY_MS 3000
#define BTN_POLL_MS      50
// 2560 was not enough: the emergency IO14 hold and the Settings → Reset WiFi item both run
// their action inline on this task, reaching ble_provisioning_start() → esp_srp_gen_salt_verifier(),
// which does 3072-bit mbedtls MPI work and overflowed the stack.
#define BTN_TASK_STACK    6144
#define BTN_TASK_PRIORITY 3
#define BTN_QUEUE_LEN     10

static constexpr int s_btn_pins[BSP_BTN_COUNT] = {PIN_BTN_BOOT, PIN_BTN_IO14};
static bsp_button_cb_t s_callback = NULL;
static QueueHandle_t s_btn_queue = NULL;

// ============================================================
// GPIO ISR — sends button ID to queue to wake the task
// ============================================================
// readability-identifier-naming misreads the IRAM_ATTR attribute macro as a variable; arg is
// used via the cast below (same misc-unused-parameters false positive as rssi_compare() /
// wifi_cred_load() — see docs/clang-tidy-suppressions.md).
// NOLINTNEXTLINE(readability-identifier-naming, misc-unused-parameters)
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
  int btn_id = (int) (intptr_t) arg;
  xQueueSendFromISR(s_btn_queue, &btn_id, NULL);
}

// ============================================================
// Check if a button is currently pressed (active LOW)
// ============================================================
static inline bool btn_is_pressed(int btn_id)
{
  return gpio_get_level(s_btn_pins[btn_id]) == 0;
}

// ============================================================
// Button task — detects short/long/emergency presses
//
// Flow:
//   1. Wait for ISR queue event (button edge detected)
//   2. Debounce: wait BTN_DEBOUNCE_MS, re-check level
//   3. If pressed: poll every BTN_POLL_MS while held
//      - At BTN_LONG_MS → fire BSP_BTN_LONG
//      - At BTN_EMERGENCY_MS (IO14 only) → fire BSP_BTN_EMERGENCY
//   4. On release: if long was never fired → fire BSP_BTN_SHORT
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void button_task(void *pvParameters) // NOLINT(readability-non-const-parameter)
{
  (void) pvParameters;
  int btn_id;

  while (1) // NOLINT
  {
    if (!xQueueReceive(s_btn_queue, &btn_id, portMAX_DELAY))
    {
      continue;
    }

    // Debounce: wait and re-read
    vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));

    if (!btn_is_pressed(btn_id))
    {
      // Spurious edge or bounce — ignore
      continue;
    }

    // Button is confirmed pressed — start timing
    bool long_fired = false;
    bool emergency_fired = false;
    uint32_t held_ms = BTN_DEBOUNCE_MS; // already waited this much

    // Drain any duplicate ISR events for this button
    int dummy;
    while (xQueueReceive(s_btn_queue, &dummy, 0))
    {
      // discard
    }

    // Poll while button is held
    while (btn_is_pressed(btn_id))
    {
      vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
      held_ms += BTN_POLL_MS;

      // Long press threshold
      if (!long_fired && held_ms >= BTN_LONG_MS)
      {
        long_fired = true;
        ESP_LOGD(tag, "btn%d: LONG (%lums)", btn_id, (unsigned long) held_ms);
        if (s_callback)
        {
          s_callback(btn_id, BSP_BTN_LONG);
        }
      }

      // Emergency threshold (IO14 only)
      if (!emergency_fired && btn_id == BSP_BTN_IO14 && held_ms >= BTN_EMERGENCY_MS)
      {
        emergency_fired = true;
        ESP_LOGW(tag, "btn%d: EMERGENCY (%lums)", btn_id, (unsigned long) held_ms);
        if (s_callback)
        {
          s_callback(btn_id, BSP_BTN_EMERGENCY);
        }
      }
    }

    // Button released
    if (!long_fired)
    {
      ESP_LOGD(tag, "btn%d: SHORT (%lums)", btn_id, (unsigned long) held_ms);
      if (s_callback)
      {
        s_callback(btn_id, BSP_BTN_SHORT);
      }
    }

    // Drain any release-edge ISR events
    while (xQueueReceive(s_btn_queue, &dummy, 0))
    {
      // discard
    }
  }
}

// ============================================================
// Public API
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void bsp_buttons_init(const bsp_button_cb_t callback)
{
  ESP_LOGI(tag, "Initializing buttons...");

  s_callback = callback;
  s_btn_queue = xQueueCreate(BTN_QUEUE_LEN, sizeof(int));
  if (!s_btn_queue)
  {
    ESP_LOGE(tag, "Failed to create button queue — buttons disabled");
    return; // leave the ISR unregistered rather than have it push into a NULL queue
  }

  // Install GPIO ISR service (ignore if already installed)
  esp_err_t err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
  {
    ESP_ERROR_CHECK(err);
  }

  // Configure button GPIOs with interrupt on any edge
  for (int i = 0; i < BSP_BTN_COUNT; i++)
  {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_btn_pins[i]),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_isr_handler_add(s_btn_pins[i], gpio_isr_handler, (void *) (intptr_t) i));
  }

  // Start button processing task
  const BaseType_t xret =
      xTaskCreatePinnedToCore(button_task, "buttons", BTN_TASK_STACK, NULL, BTN_TASK_PRIORITY, NULL, 0);
  if (xret != pdPASS)
  {
    ESP_LOGE(tag, "Failed to create button task — buttons disabled");
    return;
  }

  ESP_LOGI(tag, "Buttons ready: GPIO%d (BOOT), GPIO%d (IO14)", PIN_BTN_BOOT, PIN_BTN_IO14);
}
