# LCD_BACKLIGHT LED Driver Component (for ZenClock)

**[AI Context & Instructions]**
> To any AI Agent or LLM reading this file:
> This is a local ESP-IDF component used to control the LCD screen brightness on the LilyGo T-Display-S3 board (ZenClock
> project).
> It is a highly optimized **wrapper around the standard ESP-IDF `ledc` (PWM) API**.
>
> **AI Rules for using this component:**
> 1. Do NOT write raw `ledc_set_duty()` or `ledc_update_duty()` for the display backlight. Always use the functions
     provided by `lcd_backlight.h`.
> 2. This component supports smooth fading (fade time) via ESP32's hardware fade feature. Use it to create premium,
     smooth UI transitions when turning the screen on/off or adjusting brightness.
> 3. Brightness is controlled purely via percentage (0-100%).

---

## 1. Overview

The `lcd_backlight` component simplifies the ESP-IDF LEDC setup and provides an easy-to-use API to set brightness or
fade it smoothly using the ESP32's PWM capabilities.

## 2. API Summary

Include the header in your C files:

```c
#include "lcd_backlight.h"
```

**Key Data Types:**

* `lcd_backlight_handle_t`: The handle used to interact with the initialized driver.
* `lcd_backlight_config_t`: Configuration structure to initialize the driver (GPIO, PWM freq, Timer, Channel).

**Key Functions:**

* `esp_err_t lcd_backlight_init(const lcd_backlight_config_t *config, lcd_backlight_handle_t *out_hdl);`
    * Initializes the LEDC peripheral and prepares the fade functions.
* `esp_err_t lcd_backlight_set_brightness(lcd_backlight_handle_t handle, uint8_t percent, uint32_t fade_time_ms);`
    * Sets brightness by percentage (0 - 100%). `fade_time_ms` > 0 will animate the transition.
* `uint8_t lcd_backlight_get_brightness(lcd_backlight_handle_t handle);`
    * Returns current brightness (0-100%).

## 3. Usage Example (for AI and Humans)

Here is the standard way to initialize and use this component in `main.c`:

```c
#include "lcd_backlight.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Define the GPIO pin for your backlight (Usually GPIO 38 on T-Display-S3)
#define LCD_PIN_NUM_BK_LIGHT   38 

static lcd_backlight_handle_t lcd_backlight_hdl;

void lcd_brightness_init(void) {
    const lcd_backlight_config_t config = {
        .gpio_num = LCD_PIN_NUM_BK_LIGHT,
        .pwm_freq_hz = 5000,
        .timer_num = LEDC_TIMER_1,
        .channel_num = LEDC_CHANNEL_0
    };

    // Initialize the backlight component
    ESP_ERROR_CHECK(lcd_backlight_init(&config, &lcd_backlight_hdl));
    
    // Turn on the backlight with a smooth 1-second fade to 100%
    lcd_backlight_set_brightness(lcd_backlight_hdl, 100, 1000);
}
```