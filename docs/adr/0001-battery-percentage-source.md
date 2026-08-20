# Battery percentage source

Issue #44 replaced `bsp_battery.c`'s hand-rolled sigmoid curve — which amplified single-sample ADC
noise into ±3–7% status-bar jitter — with `espressif/adc_battery_estimation` (Analog Devices
OCV-SOC model, 21 points). Three decisions came out of that swap that aren't obvious from the code:

**`*mv` and `*pct` in `bsp_battery_read()` no longer share one ADC conversion.** They used to,
deliberately (see `docs/DECISIONS.md`, Battery / BSP). The library's internal 10-sample-averaged
read is the actual jitter fix and it owns that read itself — it exposes no raw-voltage getter to
share a sample with. `*mv` stays a single instantaneous raw read for diagnostics; `*pct` is the
library's independently-read, filtered value. Accepted as the cost of the fix, not a regression.

**The library's `charging_detect_cb` is wired to the same immediate voltage-threshold check `*usb`
uses — not left for the library's own software trend estimation.** First cut left it unset,
reasoning that the trend estimator (which needs ~200s of sample history before trusting anything
over its last-known state) was fine as an internal-only detail, since the app-wide `*usb` (charge
icon, low-battery clamp) would keep using the immediate threshold check regardless. Code review
caught the actual consequence: with no callback, `is_charging` inside `get_capacity()` stays
whatever it last was for up to 200s, so a USB session shorter than that leaves the "discharging:
capacity must not increase" clamp suppressing the real voltage rise the whole time — and *after*
unplugging, since `is_charging` is still stale, the same clamp then pins the reported % at its
stale pre-charge value instead of letting it reflect the now-higher real level. A brief top-up could
permanently depress the displayed percentage. Wiring the existing threshold check as the callback
means charging state flips the instant USB does, closing the gap — one source of truth for "on
USB", reused rather than duplicated.

**On USB, the percentage is hidden rather than shown or frozen.** The ADC reads the USB rail on
that pin, not the battery — the value is structurally above every point in both of the library's
curves, so it clamps to the top regardless of curve or charging-detection choice. Swapping the
library does not fix this by itself. Hiding it (status bar: blank; System Info: "Charging (mV)")
was chosen over freezing the last known pre-USB value, since it needs no extra state and the charge
icon already carries the meaning.
