// SPDX-License-Identifier: MIT
// BSP Buttons — press detection on top of espressif/button (iot_button)
//
// Detection model (unchanged from the hand-written driver this replaced):
//   - Short press:  released within < BTN_LONG_MS (800ms)
//   - Long press:   held >= BTN_LONG_MS (fires once while held)
//   - Emergency:    IO14 held >= BTN_EMERGENCY_MS (3000ms, fires once, after LONG)
//
// Why there is still a queue and a task here, when the component does the timing:
// iot_button runs every callback from one shared periodic esp_timer, in the esp_timer task, and
// its docs forbid blocking there. on_button_press() takes lvgl_port_lock(0) — which waits
// portMAX_DELAY — so calling the app callback from the component's context would park the shared
// timer task behind a screen repaint and take the 10s microlink poll down with it. The callbacks
// below therefore only enqueue; this task is what actually calls into the app, exactly as before.
// It is deliberately not btn_worker: that queue carries do_reset_wifi(), which blocks for seconds.

#include "bsp.h"
#include "board_config.h"

#include <esp_log.h>
#include "iot_button.h"
#include "button_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *const tag = "bsp_buttons";

// ============================================================
// Timing constants
// ============================================================
#define BTN_LONG_MS      800
#define BTN_EMERGENCY_MS 3000
// 6144 is historical headroom, not a measured requirement of what runs here now: the actions that
// needed it (the emergency IO14 hold, Settings → Reset WiFi, both reaching ble_provisioning_start()
// → esp_srp_gen_salt_verifier() and its 3072-bit mbedtls MPI work) moved to btn_worker, which
// carries its own 6144. What still runs on this task is on_button_press(), which takes the LVGL
// lock and calls nav_handle_action(). Kept as-is deliberately — sizing it down means measuring the
// deepest LVGL screen transition, and a stack overflow here silently kills every button.
#define BTN_TASK_STACK    6144
#define BTN_TASK_PRIORITY 3
#define BTN_QUEUE_LEN     10

// The button feel of the whole device lives in these four Kconfig values, and they are written
// into a sdkconfig the component manager generates — the same path that can silently reset
// CONFIG_ML_* to defaults after a dependency-manager upgrade (see CLAUDE.md). A silent reset here
// would still compile and still run: debounce would quietly drop to 10ms and SHORT would gain a
// 180ms double-click wait. These turn that into a build failure instead.
_Static_assert(CONFIG_BUTTON_PERIOD_TIME_MS == 10, "sdkconfig drifted: CONFIG_BUTTON_PERIOD_TIME_MS must be 10");
_Static_assert(CONFIG_BUTTON_DEBOUNCE_TICKS == 5, "sdkconfig drifted: CONFIG_BUTTON_DEBOUNCE_TICKS must be 5 (= 50ms)");
_Static_assert(CONFIG_BUTTON_SHORT_PRESS_TIME_MS == 50,
               "sdkconfig drifted: CONFIG_BUTTON_SHORT_PRESS_TIME_MS must be 50");
_Static_assert(CONFIG_BUTTON_LONG_PRESS_TIME_MS == BTN_LONG_MS,
               "sdkconfig drifted: CONFIG_BUTTON_LONG_PRESS_TIME_MS must be 800");

static constexpr int s_btn_pins[BSP_BTN_COUNT] = {PIN_BTN_BOOT, PIN_BTN_IO14};
static bsp_button_cb_t s_callback = NULL;
static QueueHandle_t s_btn_queue = NULL;

typedef struct
{
  uint8_t btn_id;
  uint8_t event;
} btn_msg_t;

// ============================================================
// iot_button callbacks — esp_timer context, must not block
// ============================================================
// The button id and event are packed into usr_data at registration, so one function serves all
// five registrations and nothing has to be looked up from the handle here.
// NOLINTNEXTLINE(misc-unused-parameters)
static void button_cb(void *arg, void *usr_data)
{
  (void) arg;
  const uintptr_t packed = (uintptr_t) usr_data;
  const btn_msg_t msg = {.btn_id = (uint8_t) (packed >> 8U), .event = (uint8_t) (packed & 0xFFU)};

  // Non-blocking by contract. A full queue means the app task is already 10 events behind, at
  // which point dropping the newest is the only option that keeps the timer task moving.
  xQueueSend(s_btn_queue, &msg, 0);
}

#define PACK_CB_DATA(btn_id, event) ((void *) (uintptr_t) (((uintptr_t) (btn_id) << 8U) | (uintptr_t) (event)))

// ============================================================
// Button task — drains the queue and calls the app callback
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void button_task(void *pvParameters) // NOLINT(readability-non-const-parameter)
{
  (void) pvParameters;
  btn_msg_t msg;

  while (1) // NOLINT
  {
    if (!xQueueReceive(s_btn_queue, &msg, portMAX_DELAY))
    {
      continue;
    }

    if (msg.event == BSP_BTN_EMERGENCY)
    {
      ESP_LOGW(tag, "btn%u: EMERGENCY", (unsigned) msg.btn_id);
    }
    else
    {
      ESP_LOGD(tag, "btn%u: %s", (unsigned) msg.btn_id, (msg.event == BSP_BTN_LONG) ? "LONG" : "SHORT");
    }

    if (s_callback)
    {
      s_callback((int) msg.btn_id, (bsp_btn_event_t) msg.event);
    }
  }
}

// ============================================================
// Registration helper — one button's full event set
// ============================================================
static esp_err_t register_button(const int btn_id, button_handle_t *out_handle)
{
  const button_config_t btn_cfg = {0};
  const button_gpio_config_t gpio_cfg = {
      .gpio_num = s_btn_pins[btn_id],
      .active_level = 0, // active-low: the pad reads 0 while pressed
                         // enable_power_save is deliberately off — it installs GPIO wakeup on these same two pads,
                         // which deep_sleep already owns an ext1 wake condition and rtc_gpio_pullup_en() on. See #104.
  };

  esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, out_handle);
  if (err != ESP_OK)
  {
    return err;
  }

  err = iot_button_register_cb(*out_handle, BUTTON_SINGLE_CLICK, NULL, button_cb, PACK_CB_DATA(btn_id, BSP_BTN_SHORT));
  if (err != ESP_OK)
  {
    return err;
  }

  // Two thresholds on the same button. Registration order does not matter — iot_button_register_cb()
  // inserts sorted by press_time (iot_button.c:399-424). What does matter is that the *lowest*
  // threshold equals CONFIG_BUTTON_LONG_PRESS_TIME_MS: the first callback only fires when
  // cb_info[0].press_time == long_press_ticks * TICKS_INTERVAL (iot_button.c:161). Both are 800
  // here and the _Static_assert above pins them together, so introducing a threshold below
  // BTN_LONG_MS without moving the Kconfig value would silently kill the 800ms LONG.
  button_event_args_t args = {.long_press = {.press_time = BTN_LONG_MS}};
  err = iot_button_register_cb(*out_handle, BUTTON_LONG_PRESS_START, &args, button_cb,
                               PACK_CB_DATA(btn_id, BSP_BTN_LONG));
  if (err != ESP_OK || btn_id != BSP_BTN_IO14)
  {
    return err;
  }

  args.long_press.press_time = BTN_EMERGENCY_MS;
  return iot_button_register_cb(*out_handle, BUTTON_LONG_PRESS_START, &args, button_cb,
                                PACK_CB_DATA(btn_id, BSP_BTN_EMERGENCY));
}

// ============================================================
// Public API
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void bsp_buttons_init(const bsp_button_cb_t callback)
{
  ESP_LOGI(tag, "Initializing buttons...");

  s_callback = callback;
  s_btn_queue = xQueueCreate(BTN_QUEUE_LEN, sizeof(btn_msg_t));
  if (!s_btn_queue)
  {
    ESP_LOGE(tag, "Failed to create button queue — buttons disabled");
    return; // leave the buttons unregistered rather than have a callback push into a NULL queue
  }

  // Handles are kept so a failure partway through can undo the buttons already registered. Losing
  // one is not a leak that sits still: its callbacks stay live and keep pushing into a queue that
  // no task drains, which fills at BTN_QUEUE_LEN and then silently discards every press forever.
  button_handle_t handles[BSP_BTN_COUNT] = {NULL};

  for (int i = 0; i < BSP_BTN_COUNT; i++)
  {
    const esp_err_t err = register_button(i, &handles[i]);
    if (err != ESP_OK)
    {
      // Degrade, don't abort: a device that cannot read its buttons is still a working clock.
      ESP_LOGE(tag, "btn%d setup failed (%s) — buttons disabled", i, esp_err_to_name(err));
      for (int j = 0; j <= i; j++)
      {
        if (handles[j])
        {
          iot_button_delete(handles[j]);
        }
      }
      return;
    }
  }

  const BaseType_t xret =
      xTaskCreatePinnedToCore(button_task, "buttons", BTN_TASK_STACK, NULL, BTN_TASK_PRIORITY, NULL, 0);
  if (xret != pdPASS)
  {
    ESP_LOGE(tag, "Failed to create button task — buttons disabled");
    return;
  }

  ESP_LOGI(tag, "Buttons ready: GPIO%d (BOOT), GPIO%d (IO14)", PIN_BTN_BOOT, PIN_BTN_IO14);
}
