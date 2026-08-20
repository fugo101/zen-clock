# BSP — Board Support Package

> **[AI Context]** This is the **only** hardware abstraction layer for the ZenClock project.
> All hardware access from `main.c` and UI code MUST go through this API.
> Pin definitions are in [`board_config.h`](../../include/board_config.h) — never hardcode pins here.

## Architecture

```
bsp/
├── include/bsp.h         ← Public API (the ONLY header consumers include)
├── priv_include/bsp_priv.h ← Internal cross-module declarations (DO NOT use outside bsp)
├── src/
│   ├── bsp_display.c     ← Init orchestrator + I80 bus + ST7789 panel + LVGL port
│   ├── bsp_backlight.c   ← Thin facade over lcd_backlight component
│   ├── bsp_battery.c     ← ADC1 oneshot + calibration + adc_battery_estimation (percentage)
│   └── bsp_buttons.c     ← GPIO ISR + FreeRTOS debounce task + callback API
└── CMakeLists.txt         ← ESP-IDF component registration
```

### Dependency Graph

```
main.c ──► bsp (this component)
             ├──► lcd_backlight   (LEDC PWM wrapper)
             ├──► esp_lvgl_port   (LVGL ↔ ESP-IDF bridge)
             ├──► esp_lcd         (I80 bus + ST7789 driver)
             └──► board_config.h  (pin definitions — ../../include/)
```

## Public API

Header: [`include/bsp.h`](include/bsp.h)

### Display

| Function                                                             | Description                                                                                                      |
|----------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------|
| `void bsp_display_init(lv_disp_t **disp, bool backlight_on)`         | Initialize all hardware (I80 bus, ST7789, LVGL port, ADC, backlight, power GPIO). Call **once** from `app_main`. |
| `void bsp_display_set_brightness(uint8_t percent, uint32_t fade_ms)` | Set backlight 0–100% with optional smooth fade (0 = instant).                                                    |
| `uint8_t bsp_display_get_brightness(void)`                           | Get current brightness percentage.                                                                               |
| `void bsp_display_power_off(void)`                                    | Cuts the LCD power rail and latches it via `gpio_hold_en()` — survives deep sleep and the reboot after it. Fade the backlight out *first*; this kills the panel rail outright. See `CLAUDE.md`'s "Deep Sleep: the LCD rail is latched" for the release-order gotcha. |

### Battery

| Function                                              | Description                                                                                                                                                                                                                                                                                            |
|--------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `void bsp_battery_read(int *mv, int *pct, bool *usb)` | The **only** battery function — no standalone getters. `*mv` is one raw ADC conversion. `*pct` comes from `espressif/adc_battery_estimation`, which does its own independent, internally-filtered read (~11 ADC conversions total per call — no longer O(1)); it's not meaningful while `*usb` is true (ADC reads the USB rail, not the battery) and callers must not display it in that case. `*usb` is a plain voltage-threshold check (≥ 4600 mV). Any output pointer may be `NULL`. On failure `*mv`/`*pct` are `-1`, `*usb` is `false`. See `docs/adr/0001-battery-percentage-source.md`. |

### Buttons

| Function / Type                                                    | Description                                                                                    |
|----------------------------------------------------------------------|--------------------------------------------------------------------------------------------------|
| `BSP_BTN_BOOT` (0)                                                  | BOOT button identifier (GPIO0)                                                                   |
| `BSP_BTN_IO14` (1)                                                  | Side button identifier (GPIO14)                                                                  |
| `BSP_BTN_COUNT` (2)                                                 | Number of buttons                                                                                |
| `bsp_btn_event_t` — `BSP_BTN_SHORT` / `BSP_BTN_LONG` / `BSP_BTN_EMERGENCY` | Gesture classification, not a raw press/release: `SHORT` fires on release (< 800ms), `LONG` fires *while still held* (≥ 800ms), `EMERGENCY` fires *while still held* (≥ 3000ms, IO14 only) |
| `typedef void (*bsp_button_cb_t)(int btn_id, bsp_btn_event_t event)` | Callback type: `btn_id` = `BSP_BTN_BOOT` or `BSP_BTN_IO14`, `event` = one of the three gestures above |
| `void bsp_buttons_init(bsp_button_cb_t callback)`                   | Start button monitoring task. Calls `callback` on classified gesture events.                     |

## Internal API (`bsp_priv.h`)

> ⚠️ **Do NOT include from outside this component.**

These functions are called by `bsp_display_init()` during startup:

| Function                              | Called by       | Purpose                                      |
|---------------------------------------|-----------------|----------------------------------------------|
| `bsp_backlight_setup()`               | `bsp_display.c` | Init LEDC PWM via `lcd_backlight` component  |
| `bsp_backlight_set(percent, fade_ms)` | `bsp_display.c` | Delegate to `lcd_backlight_set_brightness()` |
| `bsp_backlight_get()`                 | `bsp_display.c` | Delegate to `lcd_backlight_get_brightness()` |
| `bsp_battery_setup()`                 | `bsp_display.c` | Init ADC1 + calibration handle               |

## Init Sequence

`bsp_display_init()` is the single entry point that orchestrates all hardware setup:

```
bsp_display_init()
  ├── lvgl_port_init()          ← LVGL task + timer
  ├── init_power()              ← GPIO15 HIGH (LCD LDO on) + GPIO9 pull-up
  ├── bsp_backlight_setup()     ← LEDC PWM init (via lcd_backlight)
  ├── bsp_battery_setup()       ← ADC1 unit + channel + calibration
  ├── init_i80_bus()            ← 8-bit parallel bus (17MHz pixel clock)
  ├── init_panel()              ← ST7789 driver + orientation + color invert
  ├── register_lvgl_display()   ← LVGL display port (double-buffered, PSRAM)
  └── [optional backlight on]   ← If backlight_on=true → 100% instant
```

## Display Orientation

Landscape mode (buttons on left, USB on right) requires matching settings in **two places** inside `bsp_display.c`:

```c
// Panel hardware (init_panel):
esp_lcd_panel_swap_xy(panel_handle, true);
esp_lcd_panel_mirror(panel_handle, false, true);
esp_lcd_panel_set_gap(panel_handle, 0, 35);

// LVGL port (register_lvgl_display):
.rotation = { .swap_xy = true, .mirror_x = false, .mirror_y = true }
```

> If these don't match → garbled/smeared display.

## Key Constants

| Constant           | Value                                                 | Location        |
|--------------------|-------------------------------------------------------|-----------------|
| Pixel clock        | 17 MHz                                                | `bsp_display.c` |
| I80 bus width      | 8 bits                                                | `bsp_display.c` |
| LVGL buffer size   | `320×170` pixels (full screen)                        | `bsp_display.c` |
| LVGL double buffer | Yes (PSRAM, ~213 KB total)                            | `bsp_display.c` |
| LVGL task stack    | 6 KB, priority 2, core 1                              | `bsp_display.c` |
| I80 max transfer   | Full screen (`320×170×2` bytes)                       | `bsp_display.c` |
| Button debounce    | 50 ms                                                 | `bsp_buttons.c` |
| Button task        | 2 KB stack, priority 3, core 0                        | `bsp_buttons.c` |
| ADC                | ADC1_CH3, 12dB attenuation, curve-fitting calibration | `bsp_battery.c` |
| USB threshold      | ≥ 4600 mV (after ×2)                                  | `bsp_battery.c` |
| Battery divider    | 100kΩ/100kΩ (GPIO4)                                   | `bsp_battery.c` |

## Usage Example

```c
#include "bsp.h"
#include "ui.h"

static void on_button_press(int btn_id, bsp_btn_event_t event) {
    if (event != BSP_BTN_SHORT) return;
    uint8_t br = bsp_display_get_brightness();
    if (btn_id == BSP_BTN_BOOT)  br = (br >= 100) ? 100 : br + 10;
    if (btn_id == BSP_BTN_IO14)  br = (br <= 10)  ? 10  : br - 10;
    bsp_display_set_brightness(br, 200);
}

void app_main(void) {
    static lv_display_t *disp;
    bsp_display_init(&disp, false);      // all hardware up, backlight off

    lvgl_port_lock(0);                   // MUST lock before LVGL calls
    ui_init();                           // hand-written LVGL UI
    lvgl_port_unlock();

    bsp_display_set_brightness(100, 2000); // smooth 2s fade-in
    bsp_buttons_init(on_button_press);
    // app_main returns — FreeRTOS scheduler runs LVGL + button tasks
}
```

## ESP-IDF Component Dependencies

From [`CMakeLists.txt`](CMakeLists.txt):

```
REQUIRES: esp_lvgl_port, driver, freertos, esp_lcd, lvgl, esp_timer,
          soc, esp_adc, lcd_backlight, esp_driver_ledc, esp_driver_gpio,
          adc_battery_estimation
```

## Rules for AI Agents

1. **Always use `bsp_display_set_brightness()`** — never call `lcd_backlight` directly.
2. **All LVGL calls** must be inside `lvgl_port_lock(0)` / `lvgl_port_unlock()`.
3. **Pin definitions** are in `board_config.h` only — never duplicate or hardcode.
4. **Read `bsp.h` first** to understand the public API before reading `.c` files.
5. **`bsp_priv.h`** is internal — do not include from `main.c` or UI code.
