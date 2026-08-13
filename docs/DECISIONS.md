# ZenClock — Decisions & Traps

Why the code looks the way it does. Grouped by subsystem, so when you go deep on one driver you can
read just its section first.

This started as the backlog for the 2026-08-10 full-project audit (~50 findings, closed across
PR #14-#35). The backlog is finished; what survives here is the part worth keeping: **decisions that
look wrong until you know why**, traps already paid for once, and places where the audit itself was
wrong. Several entries exist specifically to stop someone — including a future us — from "fixing"
working code back into a bug.

Two rules for editing this file:

- If you reverse a decision here, delete the entry. A stale rationale is worse than none.
- Record what you actually verified, not what you believe. "Code-reviewed" and "measured on device"
  are different claims and are kept apart on purpose.

---

## Still open

Everything else in this file is decided or done. These are not.

| What | Why it's still open |
|------|---------------------|
| Deep-sleep current never measured | Needs an inline current meter. The "~6 µA" figure was removed from both READMEs rather than shipped unverified. |
| Low-battery icon (red < 15%, blink < 5%) never observed | The brightness clamp half **was** confirmed on device (screen dims, the Settings value correctly stays put — it's a live clamp, never written to NVS). The colour/blink half needs a real pack drained below 15%. |
| BLE teardown never checked by heap measurement | `network_prov_mgr_deinit()` in the `NETWORK_PROV_END` handler rests on code review plus the upstream example. If a heap trend across repeated provisioning cycles becomes measurable, re-check it. |
| `do_dns_probe()` proves DNS resolves, not that anything is reachable | See the WiFi section — found 2026-08-13, not in the original audit. |
| Battery reads 100% on USB power | Pre-existing since the initial BSP commit (`033f806`), not an audit regression. See the Battery section. |

---

## WiFi manager

**The device is a clock first.** It must stay usable with no WiFi, out of coverage, or on a LAN-only
network. Losing the network is normal operation, never a stuck state. Several fixes below exist only
to enforce that.

**Only `NO_CRED` starts BLE provisioning.** `DISCONNECTED`, `NO_MATCH` and `ALL_FAILED` schedule a
backoff reconnect instead (30s → ×2 → 300s ceiling). Losing coverage must never drop the device into
provisioning, and a retry must always stay armed. The enum comments in `wifi_manager.h` said
otherwise for a long time and were wrong.

**`BIT_STOP` is checked before `BIT_DISCONNECTED`,** in both VERIFYING and CONNECTED.
`wifi_manager_stop()` calls `esp_wifi_disconnect()`, so a deliberate stop always raises both bits.
Testing DISCONNECTED first reports a spurious failure event and has the app schedule a reconnect
against its own stop.

**`s_task_parked` distinguishes "genuinely blocked on the notify" from IDLE-in-passing.** IDLE is
also the state the task transits between `ulTaskNotifyTake()` and `set_state()`. An early return
there set no `BIT_STOP`, returned `ESP_OK`, and let the task walk into a full scan — misleading even
a caller that checks the return. Don't reintroduce the early return.

**Failure events are suppressed while `stop_requested()`.** Otherwise a deliberate stop makes the app
arm a reconnect that later fires *during* BLE provisioning and takes the radio back.

**A 32-char SSID with no NUL terminator is correct.** `wifi_cfg.sta.ssid` is `uint8_t[32]`, exactly
the 802.11 maximum; the driver reads it bounded and never requires termination.
`network_provisioning`'s own `handlers.c:118-126` copies the same field on the same contract — 32
bytes, no `-1`, no NUL required — with a comment explaining why. (Its mechanics differ slightly:
`strnlen`+`memset`+`memcpy` there, `strlen` plus a `>` clamp on a zero-initialized struct here.) **The password clamp next to it *does* use `- 1`** (`password[64]` is
63 chars + NUL). The two look inconsistent and are supposed to be — there's a comment at the site
saying so. The original audit called the SSID half a bug; it was wrong.

**No periodic DNS re-probe in `WIFI_ST_CONNECTED`.** `do_dns_probe()` contains an unbounded
`getaddrinfo()` and `wifi_manager_stop()` must be able to interrupt that state within
`STOP_TIMEOUT_MS`. A successful NTP sync clears the no-internet state instead — reaching a time
server is proof the internet works. The unbounded `getaddrinfo()` is documented in `wifi_priv.h` and
deliberately not fixed.

**`WIFI_MGR_NO_INTERNET` fires *after* `WIFI_MGR_CONNECTED`, not instead of it.** The association and
the IP lease are real and a LAN-only network is usable, so the state machine still enters CONNECTED;
the handler paints yellow over the green just drawn. Reversing the order silently disables the
feature.

> ⚠️ **`do_dns_probe()` does not verify reachability** — found 2026-08-13 during hardware testing,
> not in the original audit. It only checks that `getaddrinfo()` succeeded; it never connects to the
> address. Observed live: with a DNS server returning `0.0.0.0` for the probe host (AdGuard Home in
> "Null IP" blocking mode), the probe logged `DNS probe OK` while NTP timed out completely and the
> device had no usable internet. A captive portal that answers DNS with its own portal IP defeats it
> the same way. Not fixed — noting the real limit of what "verified online" currently means.

**Bounded LVGL lock in `on_wifi_event()`.** `fire_event()` runs synchronously on the wifi task,
including from inside `try_connect_candidate()`. An unbounded `lvgl_port_lock(0)` there blocks the
wifi task invisibly to `STOP_TIMEOUT_MS`. All call sites use a 50 ms bounded lock and skip the paint
on failure. A missed paint costs nothing — every WiFi status is repainted on the next event.
Fixed by reasoning from the `ts_poll_cb()` precedent, not from a reproduced hang; how long the wifi
task was ever actually blocked was never measured.

---

## BLE provisioning

**`network_prov_mgr_stop_provisioning()` is asynchronous.** The header says it "will initiate a
process to stop the service and return" (`manager.h:429-434`); the cleanup delay defaults to 1000 ms
and **measured 1.2 s on device**. Calling `network_prov_mgr_deinit()` + `free_sec2_credentials()`
inside that window hands protocomm's shallow copy of the SRP salt/verifier back to the allocator and
panics the device about a second later. **All teardown must happen in the `NETWORK_PROV_END`
handler.** Before PR #19 this function had never actually run — the success path returned early —
so the whole region was unexercised code.

**`NETWORK_PROV_END` reports every stop as success**, and that path releases ~110 KB of BT RAM
permanently. The outcome is therefore latched from `NETWORK_PROV_WIFI_CRED_SUCCESS`, and a cancel
reports `BLE_PROV_STOPPED`. Only `BLE_PROV_SUCCESS` may call `release_memory()`.

**`ble_provisioning_release_memory()` is one-way.** After it, `ble_provisioning_start()` returns
`ESP_ERR_INVALID_STATE` forever; re-provisioning requires `esp_restart()`.

**`ble_provisioning_is_active()` returns `s_active && !s_stopping`** — false the moment a stop is
requested. Otherwise re-entering during the 1.2 s teardown window takes the "just re-show the QR"
branch against a manager that is tearing itself down.

**The QR overlay is dismissible on purpose.** Nav actions are swallowed while it's up so no screen
transition can delete it, but `BACK` hides it and returns to the clock with BLE still advertising.
**A device that has never been provisioned stays in this state indefinitely and must remain usable
as a clock.** This is why the deep-sleep inhibit predicate uses `prov_screen_is_visible()` and *not*
`ble_provisioning_is_active()` — the latter would mean such a device never auto-sleeps.

**Wrong-password retry does not stop/restart the BLE manager.** Clear the NVS credential and re-show
the QR so the phone retries over the existing BLE connection.

**After `BLE_PROV_SUCCESS`, `network_prov_mgr` leaves WiFi connected.** `try_connect_candidate()`
must `esp_wifi_disconnect()` + `vTaskDelay(300ms)` before reconnecting, or `ASSOC_LEAVE` causes a
15-second timeout → `WIFI_MGR_ALL_FAILED`.

**Never log `cfg->ssid` directly.** It is `uint8_t[32]` with no guaranteed NUL, and
`wifi_sta_config_t` places `password[64]` immediately after it — `%s`/`strlen()` ran straight into
the password bytes and leaked the plaintext WiFi password to the serial log and into the NVS `ssid`
key, which then permanently broke reconnection (`ESP_ERR_NVS_INVALID_LENGTH` on the next boot).
Bounded-copy with `strnlen`+`memcpy` first, following the precedent at `handlers.c:123-126`.
**Verified fixed on device
2026-08-13** with a 32-byte-UTF-8 SSID: no password anywhere in the log, and the device reconnected
cleanly after reboot.

**Security 2 (SRP6a).** Password = last 4 bytes of the WiFi MAC as 8 hex chars, regenerated on every
`ble_provisioning_start()`. Username is fixed to `"wifiprov"` — the Espressif BLE Prov app hardcodes
this regardless of the QR code's `username` field.

**Six sdkconfig keys hold this together:** `CONFIG_BT_ENABLED`, `CONFIG_BT_BLUEDROID_ENABLED`,
`CONFIG_BT_BLE_ENABLED`, `CONFIG_BT_BLE_42_FEATURES_SUPPORTED` (S3 defaults to BLE 5.0; legacy GAP
needs this), `CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2`, `CONFIG_LV_USE_QRCODE`. Check these
first if provisioning breaks.

---

## Deep sleep & power

> ⚠️ **`gpio_hold_en(PIN_LCD_PWR)` outlives the sleep *and the reboot after it*.** `init_power()`
> (`bsp_display.c`) releases it with `gpio_hold_dis()` **after** driving the pad high — that order is
> required by the driver (`gpio.h:458`). Drop or reorder those calls and the device wakes with a
> permanently dark screen and looks bricked. It isn't: reflashing runs `gpio_hold_dis()` again.

**`gpio_deep_sleep_hold_en()` is deliberately not used** — it latches *every* digital pad including
both wake buttons. GPIO15 is inside the S3's RTC range (0-21), so `gpio_hold_en()` alone survives
sleep.

> ⚠️ **`rtc_gpio_pullup_en()` on the wake pins is required, not redundant.** The sentence *"This
> function does not modify pin configuration"* (`esp_sleep.h:302`) means the ext1 call installs the
> wake condition **and nothing else** — it does not touch pull-ups. `bsp_buttons.c`'s digital
> pull-ups live in the IO mux, which is unpowered during sleep. Remove these and both pins float,
> `ANY_LOW` is satisfied immediately, and **the device wakes the instant it sleeps**. This was
> removed once on exactly that misreading and had to be restored.

**"Sleeps then wakes instantly" looks exactly like a panic from the outside.** Don't trust the word
"panic" in this area — check whether the log actually contains `Guru Meditation`/`Backtrace`, and
look for wake evidence (`SNTP: Deep sleep wake…`). The log that settled this was 31 KB with **no
panic text at all**; it was entirely early wakeup.

**Two parts of the sleep power-down are deliberately skipped:** `esp_lcd_panel_disp_on_off(false)`
(would need a static panel handle, and driving the i80 bus from the sleep task while LVGL may be
flushing is contention for nothing — the rail goes away anyway), and `gpio_deep_sleep_hold_en()`
(above). `RTC_PERIPH` stays ON: it is what keeps the wake-pin pull-ups alive.

**No force-sleep on low battery — a policy decision, not an oversight.** The % → voltage curve has
never been calibrated against a real measurement, so a guessed threshold to force sleep risked being
worse than the brownout it was meant to prevent. Brownout mid-`nvs_commit` therefore remains a known
accepted risk.

**The low-battery clamp is edge-triggered and never written to NVS.** It fires on the not-low → low
transition and restores on recovery, so it can't fight the user's saved brightness. It never applies
on USB power — it's about running out, not about charge level while plugged in.

**`esp_deep_sleep_start()` is `__attribute__((__noreturn__))`** (`esp_sleep.h:642`), so the sleep
task falling off its end was unreachable. The audit's claim that this was a fatal FreeRTOS error was
wrong.

---

## Battery / BSP

**None of the ADC init path is `ESP_ERROR_CHECK`'d.** `adc_cali_create_scheme_curve_fitting()`
returns `ESP_ERR_NOT_SUPPORTED` on a chip whose eFuse carries no calibration data — a property of
that individual part, not a bug. It bricked boot for a cosmetic battery percentage. All three calls
degrade; handles stay NULL, readings report `-1`, and the UI already renders that as "N/A".

**`bsp_battery_read(mv, pct, usb)` exists so one ADC conversion feeds all three outputs.** Beyond
halving conversions, it fixes a real bug the audit missed: with two separate calls, C's unspecified
argument evaluation order meant the printed `%` and `mV` could come from different samples and
visibly disagree.

**Battery reads 100% whenever USB is connected** — known, pre-existing since `033f806`, not fixed.
`percentage_from_mv()` runs the raw ADC voltage through one curve regardless of power source. On USB
the pin sees ≥ 4600 mV (that's the USB-detection threshold), well above the 4200 mV "full" the curve
is built on, so it clamps to 100. The charge icon (`LV_SYMBOL_CHARGE`) swapping in on USB is correct
and intended; only the number is meaningless. Fix belongs with a real battery-curve calibration.

---

## Settings / NVS

**`SETTINGS_BRIGHTNESS_MIN` (10) is clamped on read as well as write.** Clamping only the writer
would leave already-bricked devices stuck at 0 — a black screen with no way to see the setting that
fixes it. The same constant feeds the Brightness item's `.min` so the two can't drift.

**NVS writes are debounced behind a 1 s one-shot `lv_timer`,** flushed early on `exit_edit()`.
Holding UP on Brightness used to write ~11 blocking flash commits in 2 s. **The remaining write is
still on the LVGL task** — frequency reduced, not moved off the lock. Known, accepted.

> **`lv_timer` one-shots delete themselves** when the repeat count runs out (`lv_timer.c:369`). In
> the callback, null your handle **first**, or the flush path will `lv_timer_delete()` it a second
> time.

**`settings_screen_destroy()` flushes the pending write.** Any future caller that deletes the parent
some other way will silently lose the last unsaved edit.

**Timezone is whole hours only.** `+05:30`/`+05:45` zones can't be set correctly. Not fixing:
it would mean changing the NVS-stored offset from hours to 15-minute units, migrating already-saved
values, and touching the RANGE item + `timezone_fmt()` + its 3 unit tests — a feature, not cleanup.
The device is deployed in Vietnam (UTC+7, whole-hour).

**POSIX TZ inverts the sign.** UTC+7 becomes `TZ="UTC-7"`. Easy to write backwards from intuition —
`timezone_fmt()` has tests covering the direction.

**`timezone_fmt.c` is split out of `settings.c`** purely so it can build for the host-side
`[env:native]` tests; `settings.c` pulls in `esp_log.h`/`nvs.h` and can't.

---

## UI / LVGL

**LVGL timers only for UI updates — never FreeRTOS tasks with `lvgl_port_lock/unlock`.** Task-driven
updates cause rendering artifacts on the Intel 8080 bus.

**`lvgl_port_lock(0)` is `portMAX_DELAY`, not a try-lock** (`esp_lvgl_port.c:146`). Fine for the
boot path and for LVGL-timer contexts. **Not fine** for callbacks on the wifi task or the shared
`esp_timer` task — use the bounded 50 ms pattern (`wifi_event_lvgl_lock()`, `ts_poll_cb()`) there.

> ⚠️ **A timed-out `lvgl_port_lock()` does not hold the mutex.** Unlocking anyway is a real bug, not
> a no-op — it releases a recursive mutex you don't own. Always check the return and skip the paint;
> never call `lvgl_port_unlock()` on the failure path.

**Nav action callbacks run *outside* the LVGL lock.** `nav_handle_action()` returns the callback and
the caller runs it after unlocking — Reset WiFi alone is `wifi_manager_stop()` (up to 6 s) + NVS
erase + BLE bring-up + SRP. This also unblocks the esp_event loop, which `BLE_PROV_STARTED` needs
while `do_reset_wifi()` is running.

**Heavy nav work runs on a dedicated worker task, not the button task.** Running it inline stalled
`bsp_buttons.c`'s software `held_ms` counter, which skewed the long/emergency thresholds and made the
two-button deep-sleep combo unreachable while a blocking action ran. Presses during a Reset WiFi are
no longer swallowed (verified on device).

**`settings_row_t` / `menu_row_t` are the single source of truth for row order.** Before them the
settings indices were encoded six different ways (named defines, two independent `case` ladders, raw
`s_items[8]/[9]/[10]` subscripts, and magic numbers assuming "row 0 is a header"). Reordering the
list silently misrouted actions. Don't restate the row list in prose anywhere — point at the enum.

**The header-skip loops in `settings_screen_focus_prev/next` have an iteration cap.** `ui_circ_next/
prev` are pure modular arithmetic, so a future table edit leaving zero non-header rows would spin
forever on the LVGL task. `bugprone-infinite-loop` is disabled repo-wide, so static analysis would
never catch a regression here.

**`ui_utils.h`/`.c` must never include `lvgl.h`.** They're symlinked into `test/test_pure_logic/` and
built by `[env:native]`, which has no LVGL and no ESP-IDF. Anything taking an `lv_obj_t*` or
`lv_timer_t*` belongs in `ui_list.h`/`.c` instead — that's the only reason the two file pairs exist
separately.

**`show_screen()`'s `content_before_status_bar` flag is explicit on purpose.** LVGL z-order follows
creation order, and the clock screen genuinely creates content before the status bar while the other
three do the reverse. Normalizing that silently would be a real behaviour change.

**The five `*_destroy()` functions are deliberately *not* unified.** They were traced individually
and are genuinely different: only settings has the `flush_pending()` side effect, only settings
deletes a child object via `hide_edit_box()`, only menu carries a load-bearing "don't reset focus"
invariant (`nav.c` owns that). Forcing a common shape would hide exactly the part each one exists to
do. The repeated timer-teardown idiom *was* worth extracting — that's `ui_timer_delete()`.

**A failed NTP sync must stay visible.** `SNTP_STATUS_FAILED` once fell into an `else` branch and
*hid* the icon, so the device showed `00:00:00 01/01/1970` above a clean status bar. It's a `switch`
over the enum now, with no `default:`, so `-Werror` catches the next status added.

**Never hardcode colours.** Screen backgrounds go through a shared `lv_style_t` so `ui_set_theme()`
can call `lv_obj_report_style_change()` once; per-object inline `lv_obj_set_style_*` won't update on
a theme switch.

---

## Buttons

**The post-release drain discards the other button's queued events.** Deliberate anti-bounce, and
both buttons share one queue by design. It does **not** affect the BOOT+IO14 sleep combo — that path
polls `gpio_get_level()` directly and never goes through the queue. No current UX needs two fast
sequential presses across both buttons. The audit described this as happening *during* the hold; it
actually happens in the drain after release.

**The ISR passes NULL for `pxHigherPriorityTaskWoken`.** Legal (FreeRTOS guards it) and unobservable
here — the very next thing the task does is a 50 ms debounce `vTaskDelay`.

---

## Build, CI & tooling

**`sdkconfig.lilygo-t-display-s3` is tracked in git and `menuconfig` writes the Tailscale auth key
straight into it.** `scripts/check_secrets.py --staged` is the only thing standing between that and a
committed credential. Run `--staged` before every commit; CI runs it without `--staged` as the first
check in the job (after the runner-setup steps).

**Hand-edited comments in `sdkconfig` are wiped by confgen on the next `pio run`** (verified). Values
survive; comments don't. That's why rationale for a Kconfig choice lives in this file instead — and
why the one bad comment that mattered had to be fixed upstream in `vendor/microlink`'s `Kconfig`,
not here.

**No sdkconfig drift check in CI, deliberately.** Unlike `dependencies.lock` — which carries a stable
`manifest_hash` and is regenerated by the build, making `git diff --exit-code` a clean signal — a
generated-and-rewritten sdkconfig has no verified-stable diff target, so the gate risks failing on
churn that isn't a regression. Declined rather than shipped untested. The credential risk that
motivated it is covered by `check_secrets.py`.

> ⚠️ **`pio check` bare does *not* use `.clang-tidy`.** PlatformIO's `clangtidy` tool always injects
> its own `--checks=*` (verify with `pio check -v`), and clang-tidy honours the **last** `-checks=`
> on the command line — `.clang-tidy`'s `Checks:` is only read when no CLI flag is present. A bare
> run produces 600+ findings from checks that were never enabled. The fix lives in `platformio.ini`'s
> `check_flags` (so it applies to CLion's button too, which can't pass custom flags), and
> `scripts/pio_check.py` drift-guards that string against `.clang-tidy` — two static files with no
> way to derive one from the other, so the duplication is deliberate and policed.

**IDE-injected clang-tidy checks are not repo policy.** CLion/clangd enable checks outside
`.clang-tidy`. Two known traps: `misc-use-internal-linkage` demanding `static app_main` (**wrong** —
ESP-IDF calls it via `extern` at `app_startup.c:198`; obeying breaks the link), and
`bugprone-signed-bitwise`, which is disabled here but has the alias `hicpp-signed-bitwise`. Both are
written up in `docs/clang-tidy-suppressions.md`. `bugprone-*` *is* enabled, so not every
`bugprone-` warning is an IDE artifact — check before dismissing.

**`-bugprone-branch-clone` is disabled repo-wide.** It fires on *every* `ESP_LOGx` call (the macro
expands to an if/else-if ladder over log levels with the same `esp_log(...)` shape in each arm).
Too many sites to suppress individually.

**`NOLINTNEXTLINE` applies to exactly one following line.** Multi-line explanations go **above** it,
never between it and the code — otherwise the real line is pushed out of range and the suppression
silently does nothing.

**Actions are pinned to moving major tags, not SHAs** — a maintainer decision, with
`.github/dependabot.yml` proposing major bumps as PRs. Don't SHA-pin this repo.

**Release Please runs on a GitHub App token, not `GITHUB_TOKEN` and no longer a PAT.**
`GITHUB_TOKEN`-authored PRs don't trigger downstream workflows, so CI would never run on the
Release PR itself — that's why a PAT was used originally. A PAT is a human's credential though: it
expires silently, and when it does, Release Please just stops opening Release PRs with no error
anyone sees. `actions/create-github-app-token@v3` mints a short-lived, repo-scoped token instead.
Secrets: `RELEASE_APP_ID`, `RELEASE_APP_PRIVATE_KEY`.

**The CI job's `name:` is the required-status-check contract.** `Build, Test & Analyze` is
registered as the required status check on the `main` ruleset. Renaming the job without also
updating the ruleset means the required check never reports again and every PR blocks forever.
Consequence worth remembering: with exactly one self-hosted runner, a runner outage now blocks all
merges, not just delays CI feedback.

**`bootstrap-sha` was removed from `release-please-config.json` once tagged releases existed.**
Release Please locates the last release from `.release-please-manifest.json` and the repo's tags, so
the bootstrap SHA became vestigial past the first release — verified by checking that the next
Release PR's `CHANGELOG.md` diff contained only commits after `v0.3.0`, not the full project
history. If a future Release PR's changelog ever includes ancient commits, restore the line.

**`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` stays on with no OTA code in the tree.**
`partitions.csv` already pre-wires the dual-slot layout and OTA is a stated goal; disabling now to
re-enable later costs more than leaving it. The residual risk is accepted knowingly.

**`board_build.esp-idf.sdkconfig_path` *replaces* the sdkconfig** (`-DSDKCONFIG=`); it does not layer
a fragment onto it. `sdkconfig_extra` does not exist — PlatformIO silently ignores it.

**`[env:native]` deliberately does not set `test_build_src`.** It compiles only the symlinked pure
sources (`ui_utils.c`, `timezone_fmt.c`) and must never pull in ESP-IDF code.

**`wifi_credentials.c` is not unit-testable as the audit claimed.** Every function is direct NVS I/O
with no separable pure logic; the nearest candidate (an empty-SSID check) wasn't worth a refactor
just to have something to test. 2 of the 3 named targets are covered, and that's the honest ceiling.

**macOS has no `timeout`, and `pio device monitor` crashes when redirected to a file.** The board
uses native USB-CDC, so **every reset re-enumerates the port** and a plain `cat` capture dies
silently — it looks like "nothing happened" rather than "capture broke". Use a self-healing loop that
re-attaches, and verify data is still arriving before trusting a quiet log.

**CI's `Build firmware` step was ~9 minutes and dominated the job (94% of total CI time), even
though the self-hosted runner's workspace persists between runs.** `actions/checkout` defaults to
`clean: true`, i.e. `git clean -ffdx`, and `.pio/` (285 MB) plus `managed_components/` (184 MB) are
both gitignored — so every run started from zero and rebuilt all ~2200 objects (only ~30 are project
code; the rest is ESP-IDF/LVGL/mbedTLS/BT, unchanged run to run). Fixed by pointing
`PLATFORMIO_BUILD_CACHE_DIR` at a directory in the runner's `$HOME`, outside the workspace `git
clean` touches. This works because PlatformIO/espidf compiles every source through **SCons**, not
ninja — cmake only configures (`get_cmake_code_model()`), ninja only runs for `menuconfig` — so
SCons's built-in `CacheDir` (content-addressed, no external tool needed) covers the whole build,
bootloader included. Verified locally: cold build 89s, cache-retrieved rebuild 5.9s, bit-identical
`firmware.bin` sha256.

**The cache namespace is hashed, not a flat directory — this is load-bearing, not decoration.**
ESP-IDF 6.x's espidf.py passes compiler flags to SCons via response files
(`@.../toolchain/cflags`), and `process_response_file()` appends the file's *path* to `CFLAGS`, not
its contents. SCons computes the build signature from the command line, so it cannot see a flag
change (e.g. an optimization-level edit in `sdkconfig`) that leaves the response-file path
unchanged — a bare cache would silently serve objects built with stale flags. CI namespaces the
cache dir by `hashFiles(sdkconfig.lilygo-t-display-s3, platformio.ini, dependencies.lock)`, so any
change to those falls through to one fresh cold build in a new namespace instead of a wrong cache
hit. `platformio.ini` itself is untouched — the cache is CI-only, via `PLATFORMIO_BUILD_CACHE_DIR`,
so a dev machine's `idf.py menuconfig` churn can't produce a stale local cache.

**SCons's `CacheDir` (pinned via `tool-scons` 4.8.1) has no eviction — it only grows.** Each
sdkconfig/platformio.ini/dependencies.lock combination is a new ~285 MB namespace. `CacheRetrieveFunc`
calls `os.utime()` on every cache hit, an explicit syscall unaffected by `relatime`, so `find
-atime +14 -delete` in the CI job's prune step is a reliable "still in use" signal, not a guess.

**`actions/cache` was considered and rejected as the primary mechanism.** It's the right tool for a
GitHub-hosted runner, but this repo's single self-hosted runner already has a persistent `$HOME` —
reading the cache from local disk costs nothing, while `actions/cache` would ship several hundred MB
over the internet on every run to buy resilience against a runner rebuild, which only costs one cold
build anyway. Revisit if this repo ever moves to GitHub-hosted runners.

**Docs-only PRs skip the build/test/analyze steps, but the job itself never skips.** The `Build,
Test & Analyze` job name is the required-status-check contract (see below) — a `paths-ignore` at the
workflow level would skip the whole job and the check would never report, blocking every PR
indefinitely. Instead, an in-job `Detect build-relevant changes` step diffs against the PR base (or
`github.event.before` on push) with an inverted, fail-safe allow-list: only `docs/`, `README.md`,
`CLAUDE.md`, `CHANGELOG.md`, `THIRD_PARTY.md`, `LICENSE`, and editor-config dirs are considered
inert. Any file not on that list — including a file or directory never seen before — forces a full
build. `version.txt`, `dependencies.lock`, and `sdkconfig.*` are deliberately not on the list, so a
Release Please PR still gets a full build.

> ⚠️ **`is_cmake_reconfigure_required()` in the espidf builder only watches the root and `src/`
> `CMakeLists.txt`, not `components/*/CMakeLists.txt`.** Since ninja never runs a build (see above),
> nothing else forces a reconfigure either — adding a source file to a component's `CMakeLists.txt`
> can silently not get compiled until something else dirties the build dir (edit `src/CMakeLists.txt`,
> or `rm -rf .pio/build`). Not something this cache change introduced — CI already deletes `.pio/build`
> every run so it always reconfigures — but a real trap on a dev machine with a persistent build dir.

---

## Repo & third-party

> ⚠️ **`fugo101` and `fudio101` are two different accounts. Neither is a typo.** This repo's `origin`
> is **`fugo101/zen-clock`**; the microlink fork and the `LICENSE` copyright are **`fudio101`**.
> `fudio101/zen-clock` returns 404 — "correcting" the README back to it reintroduces a real bug.
> (`github-fudio101` appearing in a remote URL is a local SSH `Host` alias, not part of any GitHub
> path.)

> ⚠️ **Don't run `git submodule sync` in `vendor/microlink`** unless you mean it. It rewrites the
> remote from `.gitmodules`, and a global `url.insteadOf` rule then rewrites https → the *default*
> SSH identity, which has no push rights. Symptom: `git push` denied. Fix:
> `git remote set-url origin git@github-fudio101:fudio101/microlink.git`. Check `git remote -v`
> before pushing.

**After merging a submodule PR on GitHub, the local branch does not fast-forward itself.** Bump the
submodule pointer from the *merged tip of `main`*, not from your feature branch — another PR merging
in between will leave the pointer stranded on an older commit. This has happened.

**`wireguard_lwip` is a diverged BSD-3 fork of `smartalock/wireguard-lwip`.** Provenance, the pin,
and the upstream re-check procedure are in `THIRD_PARTY.md`. Last check (2026-08-10): the one
upstream commit that mattered (`ac84f4c`, cryptokey routing checking `dest` instead of `src` — a
whitelist bypass) is already fixed in our copy, as are both 2022 replay-detection fixes.

**A caret range already blocks a major bump** — `^9` and `^9.5.0` share the same `<10.0.0` ceiling.
The genuinely unbounded dependency was `idf: ">=6.0.0"`, now `<7.0.0`: this project carries ESP-IDF
6.x-specific patches that an unannounced 7.x jump would break.

---

## Hardware verification log

What has actually run on a device, and what hasn't. Kept separate from the claims above on purpose.

**Verified on device:**

- Reset WiFi → re-provision → reconnect (the symptom that started the whole audit)
- Deep sleep: cancellable fade, inhibit while the QR is up, LCD rail cut and released on the next boot
- Worker task: a button press during a Reset WiFi in progress is no longer swallowed
- Settings/menu enum refactor: full UP/DOWN wraps over all 16 rows, edit-then-BACK still persisting,
  12 consecutive screen transitions, zero panics
- Shared scroll/timer helpers: 18+ min, 14+ screen transitions, z-order intact, 1s/10s/30s timers
  still ticking
- **2026-08-13** — plaintext-password leak via unterminated SSID: provisioned a 32-byte-UTF-8 SSID
  over BLE. No password anywhere in 4600 lines of log; clean reconnect after reboot with no
  `ESP_ERR_NVS_INVALID_LENGTH`
- **2026-08-13** — NTP retry backoff after a failed sync: measured `30s → 60s → 120s → 240s`,
  doubling toward the 300 s ceiling exactly as designed
- **2026-08-13** — WiFi reconnect backoff: measured `30s → 60s → 120s`, via both the `DISCONNECTED`
  and `NO_MATCH` paths
- **2026-08-13** — red NTP icon on sustained sync failure; USB charge icon toggling on plug/unplug;
  low-battery brightness clamp (screen dims, stored setting untouched)

**Not verified on device:**

- Deep-sleep current draw (no meter)
- Low-battery icon colour/blink (needs a pack below 15% / 5%)
- BLE teardown heap trend across repeated provisioning cycles
- Yellow "connected, no internet" icon. Two attempts failed for environmental reasons, not code:
  a phone hotspot drops the AP entirely when mobile data is turned off, and a router without WAN
  stopped serving DHCP. Blocking the probe host at the DNS layer instead surfaced the
  `do_dns_probe()` limitation above rather than the yellow state. The paint path is the same shape as
  the red-NTP path that *is* verified, but that's an argument, not a measurement.
