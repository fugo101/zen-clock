# Deep sleep power policy

**The LCD power rail's GPIO hold latch is released explicitly at boot, in a specific order, and
survives the reboot that follows sleep.** `bsp_display_power_off()` drives `PIN_LCD_PWR` low and
calls `gpio_hold_en()` on it before sleeping; that hold outlives both the sleep and the reboot after
it. `init_power()` releases it with `gpio_hold_dis()` *after* driving the pad high — the driver
requires that order. Reversing it, or dropping the release call, wakes the device with a permanently
dark screen that looks bricked (it isn't — reflashing runs `gpio_hold_dis()` again). `gpio_hold_en()`
alone is sufficient because GPIO15 is inside the ESP32-S3's RTC domain (0–21); the alternative,
`gpio_deep_sleep_hold_en()`, is deliberately not used because it would latch *every* digital pad,
including both wake buttons — worse, not more thorough.

**`rtc_gpio_pullup_en()` on the wake pins is required, not redundant with normal GPIO configuration.**
The two wake-pin pull-ups otherwise live in the IO mux, which is unpowered during sleep, and the
ext1 wake API installs the wake condition and nothing else — it does not itself configure pull-ups.
Without this call both pins float, the wake condition is satisfied immediately, and the device wakes
the instant it sleeps. This was removed once on exactly that misreading and had to be restored; it
is why `RTC_PERIPH` stays powered through sleep even though the LCD panel driver itself is
deliberately not told to power off (`esp_lcd_panel_disp_on_off(false)` would need a static panel
handle and contends with LVGL flushing on the same bus for no benefit once the rail itself is cut).

**There is no force-sleep on low battery — a policy decision, not an oversight.** The battery
percentage source (`docs/adr/0001-battery-percentage-source.md`) is a generic Li-ion OCV-SOC curve,
not one calibrated against this specific pack, so a guessed threshold to force sleep risks doing more
harm (sleeping too early, or a brownout mid-`nvs_commit` from sleeping too late) than the brownout it
would be meant to prevent. The residual risk — brownout during an NVS write on a battery this device
doesn't yet model precisely — is accepted knowingly rather than mitigated with an unreliable
threshold.

**The low-battery brightness clamp is edge-triggered and never written to NVS.** It fires only on
the not-low → low transition, restores on recovery, and never applies on USB power — because it is
about running out of charge, not about charge level while plugged in. Making it level-triggered or
NVS-persisted would let it fight the user's saved brightness setting every time the level hovers near
the threshold, which is the opposite of what a low-battery clamp is for.
