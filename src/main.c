#include <esp_err.h>
#include <esp_log.h>
#include "bsp.h"
#include "deep_sleep.h"
#include "ui.h"
#include "wifi_manager.h"
#include "settings.h"
#include "ble_provisioning.h"
#include "app_handlers.h"

// Must keep external linkage: the IDF startup task declares `extern void app_main(void);` and
// calls it (freertos/app_startup.c:198). Making it static, as misc-use-internal-linkage suggests,
// breaks the link. The check is not enabled in this repo's .clang-tidy — it comes from the IDE.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void app_main(void)
{
  // Initialize BSP (LCD + LVGL port, backlight off initially)
  static lv_display_t *disp_handle;
  bsp_display_init(&disp_handle, false);

  // Initialize NVS and load settings
  settings_init();
  settings_apply_timezone(settings_get(SETTINGS_KEY_TZ_OFFSET));
  const bool is_light = settings_get_bool(SETTINGS_KEY_THEME_LIGHT);
  const uint8_t brightness = (uint8_t) settings_get(SETTINGS_KEY_BRIGHTNESS);

  // Initialize deep sleep (auto-sleep timer + wakeup sources).
  // This sequence stays hand-written rather than looping over the descriptor table: boot is
  // initialization, not update (ui_init vs ui_set_theme, deep_sleep_init vs update_timeout, and
  // a 2s backlight fade the live path deliberately does without), and the ordering below —
  // display, then NVS, then the LVGL lock, then the backlight — is load-bearing.
  deep_sleep_init(settings_get_sleep_seconds());

  // Initialize UI (self-contained: creates all widgets + timers)
  lvgl_port_lock(0);
  ui_init(is_light);
  lvgl_port_unlock();

  // Fade in backlight smoothly over 2 seconds
  bsp_display_set_brightness(brightness, 2000);

  // Initialize buttons for brightness control
  bsp_buttons_init(on_button_press);

  // Wi-Fi + BLE provisioning init (BLE fires first on no-credential boot)
  // A failed init leaves the task and the event group unusable, so starting anyway would drive
  // the state machine through NULL handles. Keep running as an offline clock instead.
  const esp_err_t wifi_ret = wifi_manager_init();
  if (wifi_ret == ESP_OK)
  {
    wifi_manager_set_callback(on_wifi_event);
    ble_provisioning_init(on_ble_prov_event);
    app_handlers_register_nav_callbacks();
    wifi_manager_start();
  }
  else
  {
    ESP_LOGE("ZenClock", "wifi_manager_init failed (%s) — running offline", esp_err_to_name(wifi_ret));
    app_handlers_register_nav_callbacks();
  }

  ESP_LOGI("ZenClock", "ZenClock started — BOOT=up/select, IO14=down/back, hold IO14=reset WiFi");
}
