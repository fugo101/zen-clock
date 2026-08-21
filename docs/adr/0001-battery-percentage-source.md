# Battery percentage source

Issue #44 replaced `bsp_battery.c`'s hand-rolled sigmoid curve — which amplified single-sample ADC
noise into ±3–7% status-bar jitter — with `espressif/adc_battery_estimation` (Analog Devices
OCV-SOC model, 21 points). Four decisions came out of that swap that aren't obvious from the code:

**Raw millivolts is not part of the public API.** `bsp_battery_read()` briefly returned `*mv`
alongside `*pct`, but the library's internal 10-sample-averaged read (the actual jitter fix) owns
its own ADC conversion and exposes no raw-voltage getter to share a sample with — so `*mv` and
`*pct` could never be guaranteed consistent anyway. Once that guarantee was gone, exposing `*mv`
publicly bought nothing but a harder contract to explain, so it was dropped instead of kept as a
"diagnostic" value. `bsp_battery.c` still does the raw read internally (needed for the USB
threshold below) and logs it at `ESP_LOGD`, so it's still available over `pio device monitor` if a
real curve calibration is ever done.

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
library does not fix this by itself. Hiding it in the status bar (blank label, charge icon carries
the meaning) was chosen over freezing the last known pre-USB value, since it needs no extra state.

**System Info's Battery row was deleted, not fixed to agree with the status bar.** It read battery
state independently, on its own offset 30s timer, from the same shared, slow-converging
`adc_battery_estimation` handle. Confirmed on hardware (2026-08-21): reconnecting USB after a
battery-only stretch, the status bar and System Info briefly showed *different* percentages before
both settled — each had caught the library's LPF mid-convergence at a different moment. The two
displays could be synchronized (issue #61 already proposed routing System Info through
`status_bar_register_battery_cb()` instead of its own timer), but the status bar is already visible
on every screen including System Info (`show_screen()` in `nav.c` mounts it unconditionally) — so
the row was showing the same fact twice, just sometimes-inconsistently. Deleting it removes the
disagreement instead of synchronizing it, and incidentally closes #61 too (only one reader left).

**None of the ADC init path is `ESP_ERROR_CHECK`'d.** `adc_cali_create_scheme_curve_fitting()`
returns `ESP_ERR_NOT_SUPPORTED` on a chip whose eFuse carries no calibration data — a property of
that individual part, not a bug — and failing boot over a cosmetic battery percentage would be a
worse outcome than degrading gracefully. Every call in the init path, including
`adc_battery_estimation_create()`, follows the same rule: on failure the handle stays NULL, readings
report `-1`, and the UI already renders that as "N/A".

**The `adc_battery_estimation` library can't own the ADC unit — `bsp_battery.c` still does.** The
library has no voltage-only getter and no other way to detect USB power on this board (there is no
dedicated charge-detect pin), so `bsp_battery_read()` must keep reading the channel itself for the
USB-threshold check, and ESP-IDF won't let two independent `adc_oneshot_unit_handle_t`s own the same
physical unit anyway. The library is handed the already-initialized handles via
`.external.adc_handle`/`.adc_cali_handle` rather than its own `.internal.*` auto-creation path — the
wrapper around it could be thinned by the library swap, but not eliminated, regardless of whether raw
millivolts are exposed publicly.

**`bsp_battery_read()` is no longer O(1) — an accepted cost of the library swap.** `*pct` now costs
the library's own ~10-sample filtered read plus a std-dev pass, plus one raw conversion for the
USB-threshold check — roughly 11 ADC conversions per call instead of 1. This no longer compounds
across two independent readers the way it briefly did before the System Info Battery row was
deleted; only `status_bar.c`'s single 30s timer calls it now.

**The single-reader contract now delivers a single derived view, not just a single reading.**
Issue #79 found the low-battery predicate written character-for-character in two components:
`status_bar.c` computed it to paint the icon red, then handed the raw `(pct, usb)` to its callback,
where `app_handlers.c` recomputed the identical expression to clamp brightness. Nothing was
inconsistent yet, but a change to what "low" means had to land twice, in two components, with two
explanatory comments. `components/battery_view/` is now the one mapping from `(pct, usb)` to symbol,
tint, blink, label text and the low flag, plus the edge-detection step the clamp arms from; the
status bar and the brightness policy are both adapters over it. `status_bar_battery_cb_t` carries
the `battery_view_t` rather than the raw pair specifically so a future third reader cannot introduce
a fourth copy of the predicate. The contract this ADR established is unchanged — one ADC reader, one
callback, last writer wins — and the thresholds moved out of `status_bar.h` into
`battery_view.c` as file-private constants, since a public threshold is an invitation to re-derive
the predicate at a call site. The `pct < 0` path is no longer a separate branch in the status bar:
`"N/A"` + empty glyph + no tint + no blink falls out of the same mapping as every other reading, and
is deliberately *not* low, so an ADC failure restores brightness rather than holding a clamp on a
reading nothing can confirm.
