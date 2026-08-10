# ZenClock — Audit Backlog

Findings from the 2026-08-10 full-project audit (code quality · build/CI/hygiene · runtime robustness).
Rolled out in phases; **tick items here as each phase lands.**

Severity: 🔴 critical · 🟠 high · 🟡 medium · ⚪ low

**Status legend:** `[ ]` open · `[x]` done · `[~]` partially done · `[-]` won't fix (reason inline)

---

## Phase 0 — Tracking doc, credential leak, provenance  ✅ done

| | Sev | Where | Issue |
|---|---|---|---|
| [x] | 🔴 | `.gitignore` | No `sdkconfig.credentials` entry while README told users to put a live Tailscale key there. Added credential + cruft ignores. |
| [x] | 🔴 | `README.md` | Documented `board_build.esp-idf.sdkconfig_extra` — **an option that does not exist**; PlatformIO silently ignored it. Verified `espidf.py:106-110` only reads `build.esp-idf.sdkconfig_path`, and that it is passed as `-DSDKCONFIG=` (i.e. *replaces* the sdkconfig, cannot layer a fragment onto it). Section rewritten with two mechanisms that actually work. |
| [x] | 🔴 | `scripts/check_secrets.py` | New guard: fails if a credential Kconfig is non-empty in any tracked sdkconfig. Supports `--staged`. This is the only thing that actually *prevents* the leak. Wire into CI in Phase 5. |
| [-] | 🟠 | `sdkconfig.lilygo-t-display-s3:4325` | Comment claims "These values are stored in sdkconfig (git-ignored)" — false, the file **is** tracked. **Not fixable here:** sdkconfig is generated; a warning comment edited into it was verified to be wiped by confgen on the very next `pio run`. The string originates in the submodule at `vendor/microlink/components/microlink/Kconfig:256` (`comment "These values are stored in sdkconfig (git-ignored)."`) → needs a PR against the microlink fork. Tracked as an upstream item below. The guard script is the durable protection. |
| [x] | ⚪ | audit | Verified no real key was ever committed on any ref — only the `tskey-auth-xxxxxxxxxxxx` placeholder at `7fbc4f2`. |
| [x] | 🟠 | `README.md:105` | Clone URL `fudio101/zen-clock` returns 404 → `fugo101/zen-clock`. (`.gitmodules` → `fudio101/microlink` is correct, left alone.) |
| [x] | ⚪ | `README.md:70` | Symlink path was one level too deep. |
| [x] | 🟡 | `THIRD_PARTY.md` | New. Records `wireguard_lwip` provenance (smartalock, BSD-3, **diverged fork**), microlink pin, managed-component licenses, and how to re-check upstream. |
| [x] | 🟡 | `README.md`, `CLAUDE.md` | Neither mentioned `smartalock` — a reader could not tell `wireguard_lwip` was third-party BSD code. Credits added. |
| [-] | ⚪ | `LICENSE` | **Audit finding was wrong** — a correct MIT `LICENSE` (Copyright (c) 2026 fudio101) was already present. The check that reported it missing (`ls LICENSE* NOTICE*`) was aborted by zsh because `NOTICE*` matched nothing, so the `LICENSE*` glob never ran. No change made; README now links to it. |

**Upstream check performed (2026-08-10):** `smartalock/wireguard-lwip` has 3 new commits; the one that
matters (`ac84f4c`, cryptokey routing checked `dest` instead of `src` — a whitelist bypass) is **already
fixed in our vendored copy**, as are both 2022 replay-detection fixes. Nothing to port. Details and the
re-check procedure are in `THIRD_PARTY.md`.

**Upstream items (belong in the `fudio101/microlink` fork, not this repo):**

- [ ] `components/microlink/Kconfig:256` — drop the false `comment "These values are stored in sdkconfig
  (git-ignored)."`; it is what put the leak instruction in front of users in the first place.
- [ ] Add a `smartalock` git remote so `wireguard_lwip` upstream can be diffed without hunting for the URL
  (procedure in `THIRD_PARTY.md`).

---

## Phase 1 — Critical: crash & wedge  ✅ done

| | Sev | Where | Issue |
|---|---|---|---|
| [x] | 🔴 | `wifi_manager.c:679,447,433,101` | **Stale `BIT_STOP` kills WiFi permanently.** `wifi_manager_stop()` only sets the bit; in `WIFI_ST_IDLE` the task is blocked on `ulTaskNotifyTake` and never consumes it. After re-provisioning, `start()` runs one loop iteration, `check_stop_signal()` eats the stale bit → back to IDLE **firing no event**, so `schedule_reconnect()` never runs. Offline until reboot. |
| [x] | 🔴 | `wifi_manager.c:346`, `do_aggregated_scan()` | `wifi_manager_stop()` returns while the task is still inside a blocking `esp_wifi_scan_start` (~12 s) or the 15 s connect wait, so BLE provisioning starts while WiFi is still scanning. Add `BIT_STOP` to the wait mask and check it between scan rounds. |
| [x] | 🔴 | `prov_screen.c:32,104` + `nav.c:88` | **Use-after-free.** The QR overlay is a child of the active screen; every `show_*_screen()` does `lv_obj_delete(old)`, which frees the overlay while `s_overlay` stays non-NULL. Long-press BOOT during provisioning → `prov_screen_hide()` deletes freed memory. Fix with an `LV_EVENT_DELETE` callback that nulls the pointer. |
| [x] | 🔴 | `ble_provisioning.c:136,226` | `NETWORK_PROV_END` clears `s_active` **before** the callback, so `ble_provisioning_stop()` early-returns and `network_prov_mgr_deinit()` never runs — leaking `prov_ctx`/lock/scheme every provisioning, then tearing down the BT controller underneath a live manager. Split `s_initialized` from `s_active`. |
| [x] | 🔴 | `bsp_buttons.c:27` | `BTN_TASK_STACK 2560` — emergency reset runs `esp_srp_gen_salt_verifier()` (mbedtls 3072-bit MPI) on this task. Raise to 6144. |
| [x] | 🔴 | `sdkconfig:2665` | `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304` — `BLE_PROV_STARTED` builds the QR + 5 labels on the event-loop task. Raise to 4096. |
| [x] | 🟠 | `wifi_manager.c` SCANNING/CONNECTING | **Found while fixing the above.** Once a stop can interrupt scan/connect, those paths fell through to `fire_event(NO_MATCH)` / `fire_event(ALL_FAILED)` — so a deliberate stop made the app schedule a reconnect that would later fire *during* BLE provisioning and grab the radio back. Failure events are now suppressed when `stop_requested()`. |
| [x] | 🟠 | `wifi_manager.c` VERIFYING | **Found while fixing the above.** `BIT_DISCONNECTED` was tested before `BIT_STOP`, but `wifi_manager_stop()` calls `esp_wifi_disconnect()` and therefore always raises both — so stopping from VERIFYING reported a spurious `WIFI_MGR_DISCONNECTED`. Order swapped to match the CONNECTED state, which already had it right. |

**Hardware verification:** Reset WiFi → re-provision → reconnect confirmed working on-device (the
symptom that motivated the fix). The BLE teardown fix was *not* verified by heap measurement — it rests
on code review plus matching the upstream example. If a heap trend across repeated provisioning cycles
ever becomes measurable, re-check it.

**Known interaction, resolved in Phase 2:** `wifi_manager_stop()` now blocks (up to `STOP_TIMEOUT_MS`,
typically one scan round) and is still called under the LVGL lock via `on_button_press`, so Reset WiFi
freezes the display for a few seconds. This is the correct trade — the previous non-blocking stop let
BLE provisioning start while the radio was still scanning — and Phase 2.3 removes the freeze by moving
nav action callbacks off the lock.

---

## Phase 2 — Concurrency & LVGL safety

| | Sev | Where | Issue |
|---|---|---|---|
| [ ] | 🟠 | `app_handlers.c:319,335` | `device_info_screen_set_ml()` → `lv_label_set_text()` called from the wifi task **without `lvgl_port_lock`**. Every other LVGL call in the file is locked. |
| [ ] | 🟠 | `app_handlers.c:87` | `ts_poll_cb` uses `lvgl_port_lock(0)` = `portMAX_DELAY` on the shared esp_timer task → blocks deep-sleep and wifi-retry timers behind it. Use `lvgl_port_lock(50)` and skip the poll on failure. |
| [ ] | 🟠 | `app_handlers.c:170-172` → `nav.c:260` | `nav_handle_action()` runs entirely under the LVGL lock, including Reset WiFi (BLE bring-up + SRP + NVS + possible `esp_restart`) and per-keypress `nvs_commit`. Defer action callbacks to a worker. |
| [ ] | 🟡 | `app_handlers.c:30,315,334` + `ts_poll_cb` | `s_ml` written by the wifi task, read by the esp_timer task and an LVGL timer, unsynchronized. |

---

## Phase 3 — WiFi reliability

| | Sev | Where | Issue |
|---|---|---|---|
| [ ] | 🟠 | `app_handlers.c:39-49` | `esp_timer_start_once()` on an armed timer returns `INVALID_STATE` (ignored) while the backoff doubles anyway → two events in a row inflate the delay without arming any retry. Stop before start; check returns. |
| [ ] | 🟠 | `app_handlers.c:33-37` | `reconnect_timer_cb` ignores `wifi_manager_start()`'s `INVALID_STATE`; nothing reschedules. |
| [ ] | 🟡 | `app_handlers.c:44,323` | `esp_timer_create()` returns unchecked — on failure the retry and Tailscale poll die silently. |
| [ ] | 🟡 | `app_handlers.c:349` | `ble_provisioning_start()` return ignored on the `NO_CRED` path; if BLE RAM was released the device sits idle forever. `do_reset_wifi()` handles this correctly — mirror it. |
| [ ] | 🟡 | `wifi_manager.c:392,597` | DNS probe timeout `return true` "assuming connected" → permanent fake-CONNECTED behind a captive portal, with no periodic re-verification. |

---

## Phase 4 — Power / UX / flash wear

| | Sev | Where | Issue |
|---|---|---|---|
| [ ] | 🟡 | `settings_screen.c:82` | Brightness `.min = 0` is persisted → black screen on next boot with no way to see the setting. Use `.min = 10`. |
| [ ] | 🟡 | `status_bar.c:222-245` | `SNTP_STATUS_FAILED` falls into the `else` branch and **hides** the icon → `00:00:00 01/01/1970` with a clean status bar. Render it red instead. |
| [ ] | 🟠 | `deep_sleep.c:21-37` | One-shot `ulTaskNotifyTake` with **no abort path** — sleeps mid-provisioning (the inactivity timer is only reset by button presses). Also falls off the end of the task function, which is a fatal FreeRTOS error. |
| [ ] | 🟡 | `deep_sleep.c:27-36` | Backlight fades but `PIN_LCD_PWR` is never pulled low, no `esp_lcd_panel_disp_on_off(false)`, no `gpio_deep_sleep_hold_en()`, and `RTC_PERIPH` is forced ON. README claims ~6 µA. **Needs a real current measurement to verify.** |
| [ ] | ⚪ | `deep_sleep.c:30` | `esp_sleep_enable_ext1_wakeup` deprecated since IDF 5.4; `rtc_gpio_pullup_en` applied without `rtc_gpio_init()` on digitally-configured pads. |
| [ ] | 🟡 | `settings_screen.c:538-576` | `nvs_commit` on **every** UP/DOWN press — holding UP on Brightness writes ~11 commits in 2 s, each a blocking flash write under the LVGL lock. Debounce. |
| [ ] | 🟠 | `clock_face_text.c:47` | Full `nvs_open`/`get`/`close` cycle **every second** on the render task (86 400/day). Cache it like `show_seconds` already is. |
| [ ] | 🟡 | `wifi_manager.c:626-643` | `ESP_ERROR_CHECK(esp_event_loop_create_default())` → `abort()`/boot loop if the loop already exists. |
| [ ] | 🟡 | `settings.c:24-32` | `ESP_ERROR_CHECK(nvs_flash_erase())` — a worn NVS partition becomes a permanent boot loop instead of a degraded device. |
| [ ] | 🟡 | `bsp_backlight.c:36` | `ESP_ERROR_CHECK` on a runtime brightness call — a transient LEDC error reboots mid-interaction. |
| [ ] | 🟡 | `wifi_manager.c:635`, `bsp_buttons.c:152`, `sntp_sync.c:222`, `deep_sleep.c:47,55` | Event group / mutex / queue / task creation returns unchecked; NULL handles then crash inside FreeRTOS asserts or get notified as garbage. |
| [ ] | ⚪ | `device_info_screen.c:342` | `esp_app_get_description()` used without a NULL check. |
| [ ] | 🟡 | `bsp_battery.c`, `status_bar.c:99` | No low-battery handling at all — nothing throttles brightness or force-sleeps before brownout, which can hit mid-`nvs_commit`. |

---

## Phase 5 — CI gates & build config

| | Sev | Where | Issue |
|---|---|---|---|
| [ ] | 🟠 | `.github/workflows/ci.yml` | Only step of substance is `pio run`. No format check, no `pio check`, no tests, no lock/sdkconfig drift check — despite `.clang-format`, `.clang-tidy`, `format.sh` and `scripts/format.py` all existing. |
| [ ] | 🟠 | `ci.yml` | Wire in `scripts/check_secrets.py` (from Phase 0) so a leaked key fails CI, not just a local hook. |
| [ ] | 🟡 | `ci.yml:14` | `if: ... head.repo.fork == false` makes fork PRs **skip**, and GitHub counts a skipped required check as passing → fork PRs merge unverified. |
| [ ] | 🟡 | `ci.yml` | No `timeout-minutes` (defaults to 360) and no `concurrency` group on the single self-hosted runner. |
| [ ] | 🟡 | `platformio.ini:17` + `ci.yml` | `extra_scripts = post:scripts/fix_compiledb.py` runs repo-controlled Python on a persistent self-hosted runner that also holds `secrets.PAT_TOKEN`. |
| [ ] | 🟡 | both workflows | Actions pinned to mutable major tags (`checkout@v7`, `upload-artifact@v7`, `release-please-action@v5`, `action-gh-release@v3`) in workflows with `contents: write` + a PAT. SHA-pin + add dependabot. |
| [ ] | 🟠 | `sdkconfig:696` | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (explicitly enabled) with **zero OTA code** in the tree — nothing ever calls `esp_ota_mark_app_valid_cancel_rollback()`. |
| [ ] | 🟡 | `release-please.yml:80-88` | Release assets lack `bootloader.bin`, so a blank board can't be flashed from a release. No checksums, no documented offsets. |
| [ ] | 🟡 | `src/idf_component.yml` | `lvgl/lvgl: "^9"` and `idf: ">=6.0.0"` are unbounded. Tighten and add a lockfile drift check. |
| [ ] | ⚪ | `src/idf_component.yml` | `network_provisioning` declared in two manifests; keep only the `ble_provisioning` one. |
| [ ] | ⚪ | `platformio.ini` | No `monitor_filters = esp32_exception_decoder` — backtraces stay raw addresses. |
| [ ] | 🟠 | `test/` | **Zero tests of any kind.** Add `[env:native]` + Unity for the host-testable pure logic (`settings_apply_timezone`, `wifi_credentials.c`, `ui_circ_next/prev`) and run it in CI. |
| [ ] | ⚪ | `scripts/format.py:26-32` | Silently runs `pip install --user clang-format`; add `--check` and fail with instructions instead. |

---

## Phase 6 — Cleanup & docs drift

| | Sev | Where | Issue |
|---|---|---|---|
| [ ] | 🟡 | `nav.c:41-43` vs `settings_screen.c:79-95,159` | Settings item indices duplicated three ways (named defines, bare `case 1/2/4/…`, `s_items[8]/[9]/[10]`). Reordering the list silently misroutes actions. One shared enum. |
| [ ] | 🟡 | `settings_screen.c:113-116`, `menu_screen.c:33-34` | No `*_destroy()`, so static widget pointers dangle after leaving the screen. Currently masked by re-creation on entry; inconsistent with the other screens. |
| [ ] | ⚪ | `settings_screen.c:121-152`, `device_info_screen.c:79-111`, `nav.c:72-154` | `apply_scroll()` duplicated verbatim; layout constants duplicated in three screens; four near-identical `show_*_screen()` bodies. |
| [ ] | ⚪ | various | Dead public API: `sntp_sync_is_synced`, `sntp_sync_stop`, `lcd_backlight_deinit`, `wifi_manager_is_connected`, `wifi_manager_get_state`, `wifi_manager_has_credential`, `bsp_display_get_brightness`, enum `BLE_PROV_CONNECTED`. (`ble_provisioning_is_active` gets used in Phase 4 — keep.) |
| [ ] | ⚪ | `src/CMakeLists.txt` | `app_handlers.c` calls `microlink_*` but `microlink` isn't in `REQUIRES`; resolves only transitively via `ui`. |
| [ ] | 🟡 | `wifi_manager.c:308,318` | A 32-char SSID / 64-char password fills the buffer with no NUL terminator; use `sizeof(...) - 1` for the password (WPA max is 63). |
| [ ] | 🟡 | `sntp_sync.c:264-282` | `sntp_sync_stop()` does `vTaskDelete` on a task that may be blocked inside lwIP. Latent — currently unreferenced. |
| [ ] | ⚪ | `settings.c:284-290` | Timezone is whole-hour only; `+05:30`/`+05:45` zones cannot be set correctly. |
| [ ] | ⚪ | `CLAUDE.md:224` | Says TZ is hardcoded in `app_main` — it is user-configurable and applied in `settings.c:288`. |
| [ ] | ⚪ | `CLAUDE.md` | "10 items" → actually 4 headers + **11** items (`SETTINGS_ITEM_COUNT 15`); label is `"Show Secs"`, not "Show Seconds". |
| [ ] | ⚪ | `CLAUDE.md` | Describes `on_wifi_event` falling back to `ble_provisioning_start()` on `NO_MATCH`/`ALL_FAILED`/`DISCONNECTED`; the code uses `schedule_reconnect()` with backoff. |
| [ ] | ⚪ | `README.md:22` | "ESP-IDF v6.0.0" → toolchain is 6.0.1. |
| [ ] | ⚪ | `lib/README`, `include/README` | Untouched Arduino-flavored PlatformIO scaffolding; delete. |
| [ ] | ⚪ | `bsp_buttons.c:40,91-94` | ISR send passes NULL for `pxHigherPriorityTaskWoken`; the drain loop discards the *other* button's events while one is held. |
| [ ] | ⚪ | `status_bar.c:69`, `device_info_screen.c:236` | Two/three ADC conversions where one would do. |
