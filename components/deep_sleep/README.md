# Deep Sleep Manager

> **[AI Context]** This component manages ESP32-S3 deep sleep for ZenClock.
> It provides inactivity auto-sleep, manual trigger, and wakeup configuration.
> Deep sleep cuts current draw by orders of magnitude versus the active ~80mA — essential for
> battery life. **The exact sleep current has never been measured on this board**; earlier revisions
> of this file quoted ~6µA, which was a figure nobody had verified. Treat it as unknown until
> someone puts a meter on it.
> **On wake, the ESP32 performs a full restart.** RTC SRAM persists; regular SRAM does not.

## Architecture

```
components/deep_sleep/
├── include/deep_sleep.h  ← Public API
├── src/deep_sleep.c      ← esp_timer + FreeRTOS sleep task
└── CMakeLists.txt
```

### How It Works

```
deep_sleep_init()
  ├── xTaskCreate(sleep_task_fn)    ← blocks on ulTaskNotifyTake
  ├── esp_timer_create(inactivity)  ← one-shot timer
  └── esp_timer_start_once(timeout) ← if timeout_s > 0

Inactivity fires → inactivity_cb → deep_sleep_trigger()
Manual trigger  → deep_sleep_trigger()
Both buttons held ≥ 800ms → deep_sleep_trigger() [via app_handlers.c]

deep_sleep_trigger() → clears the cancel flag → xTaskNotifyGive(sleep_task)
deep_sleep_cancel()  → sets the cancel flag   [called on every button event]

sleep_task wakes (and loops — a request can be declined or cancelled):
  ├── inhibit_cb()?  ──yes──> log + deep_sleep_reset_timer() + wait for the next request
  ├── bsp_display_set_brightness(0, 1500ms)     ← fade out
  ├── poll the cancel flag every 50ms for 1600ms
  │     └── cancelled ──> restore brightness + reset timer + wait for the next request
  ├── bsp_display_power_off()                   ← LCD rail low + gpio_hold_en(GPIO15)
  ├── esp_sleep_enable_ext1_wakeup_io(GPIO0 | GPIO14, ANY_LOW)
  ├── esp_sleep_pd_config(RTC_PERIPH, ON)       ← keeps the internal pull-ups powered
  ├── rtc_gpio_pullup_en(GPIO0/GPIO14)          ← REQUIRED, see below
  └── esp_deep_sleep_start()                    ← never returns
```

**The `rtc_gpio_pullup_en()` calls are load-bearing.** `esp_sleep_enable_ext1_wakeup_io()` installs
the wake condition and nothing else — *"This function does not modify pin configuration"*
(`esp_sleep.h:302`). The pull-ups `bsp_buttons.c` configures live in the digital IO mux, which is
unpowered during deep sleep. Drop these calls and both wake pins float, `ANY_LOW` is satisfied
immediately, and the device wakes the instant it sleeps — indistinguishable from a crash-and-reboot
unless you check the wakeup cause. `rtc_gpio_init()` is not needed first; Espressif's own deep_sleep
example calls these two directly.

**The LCD rail is latched off.** `gpio_hold_en(PIN_LCD_PWR)` survives deep sleep on purpose — GPIO15
is inside the ESP32-S3 RTC range, so it holds without `gpio_deep_sleep_hold_en()` (which would latch
the wake buttons too). The latch **must** be released on the next boot, and
`bsp_display_init()` → `init_power()` does exactly that with `gpio_hold_dis()`. Remove that call and
the device wakes with a permanently dark screen.

### Refusing to sleep

`deep_sleep_register_inhibit_cb()` takes a predicate that is consulted before every sleep; returning
true declines the request and re-arms the countdown. `app_handlers.c` uses it to refuse while the
provisioning QR is on screen — the user is looking at a phone, not pressing buttons, so nothing else
would hold sleep off, and sleeping restarts the chip and drops the session.

It is deliberately keyed on the overlay being visible rather than on `ble_provisioning_is_active()`:
a device that has never been provisioned advertises indefinitely by design, so the latter would
block auto-sleep forever.

### Wakeup Sources

Both buttons wake the device (ext1, active-low):

| GPIO   | Button | Mask bit     |
|--------|--------|--------------|
| GPIO0  | BOOT   | `1ULL << 0`  |
| GPIO14 | IO14   | `1ULL << 14` |

Press either button to wake. The device performs a full restart and resumes the normal boot sequence.

## Public API

Header: [`include/deep_sleep.h`](include/deep_sleep.h)

| Function                               | Description                                                                                              |
|----------------------------------------|----------------------------------------------------------------------------------------------------------|
| `deep_sleep_init(timeout_s)`           | Initialize sleep manager. Creates task + timer. `0` = auto-sleep disabled. Call after `settings_init()`. |
| `deep_sleep_reset_timer()`             | Reset the inactivity countdown. Call on every button event. No-op if auto-sleep disabled.                |
| `deep_sleep_trigger()`                 | Trigger sleep immediately. Safe from any task or timer callback. Clears any pending cancel.              |
| `deep_sleep_cancel()`                  | Call off a sleep that is already fading out. Call on every button event, next to `reset_timer()`.        |
| `deep_sleep_register_inhibit_cb(cb)`   | Predicate consulted before each sleep; true declines the request and re-arms the countdown.              |
| `deep_sleep_update_timeout(timeout_s)` | Hot-update timeout after settings change. `0` = disable auto-sleep.                                      |

## Usage

```c
#include "deep_sleep.h"

// In app_main, after settings_init():
deep_sleep_init(settings_get_sleep_seconds());

// In button handler — reset timer on every press:
void on_button_press(int btn_id, bsp_btn_event_t event) {
    deep_sleep_reset_timer();
    // ... nav handling ...
}

// When user changes Sleep H/M/S in settings:
deep_sleep_update_timeout(new_sleep_s);

// Manual trigger (e.g. "Sleep Now" menu item):
deep_sleep_trigger();
```

## Sleep Timeout Configuration

Sleep timeout is stored in NVS (namespace `zenclock`):

| NVS Key   | Range  | Default | Settings item |
|-----------|--------|---------|---------------|
| `sleep_h` | 0–23 h | 0       | Sleep H       |
| `sleep_m` | 0–59 m | 0       | Sleep M       |
| `sleep_s` | 0–59 s | 0       | Sleep S       |

All three set to `0` disables auto-sleep entirely.

## Rules for AI Agents

1. **Call `deep_sleep_init()` once at boot**, after `settings_init()` and before `bsp_buttons_init()`.
2. **Always call `deep_sleep_reset_timer()` *and* `deep_sleep_cancel()` at the top of `on_button_press()`** — before any
   other logic. Resetting the timer alone does not stop a sleep that is already fading out: the request has left the
   timer behind and the sleep task is committed to it.
3. **Never call `esp_deep_sleep_start()` directly** — always use `deep_sleep_trigger()` so the backlight fades first.
4. **`deep_sleep_trigger()` is non-blocking** — it notifies the sleep task via FreeRTOS notification. The caller returns
   immediately.
5. **After deep sleep wake, all FreeRTOS state is gone** — the device does a full restart from `app_main`.
