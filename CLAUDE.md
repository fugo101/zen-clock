# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash Commands

```bash
# Build
pio run

# Flash to device
pio run -t upload

# Monitor serial output (115200 baud)
pio device monitor

# Build + flash + monitor in one step
pio run -t upload && pio device monitor

# Format all C/C++ files
./format.sh        # Linux/macOS
format.bat         # Windows
```

## Project Overview

ZenClock is ESP-IDF 6.0+ firmware for the **LilyGo T-Display-S3** (ESP32-S3, 320×170 ST7789 LCD via Intel 8080 bus).
Built with PlatformIO. The main board config is `sdkconfig.lilygo-t-display-s3`.

## Architecture

### Boot Sequence (`src/main.c`)

`main.c` is a pure boot orchestrator — no business logic:

```
bsp_display_init → settings_init → deep_sleep_init(sleep_s)
→ ui_init(is_light)  [calls nav_init() internally]
→ bsp_display_set_brightness(brightness_from_nvs, 2000ms)
→ bsp_buttons_init → wifi_manager_init → ble_provisioning_init
→ app_handlers_register_nav_callbacks() → wifi_manager_start
```

On first boot (no NVS credentials), `wifi_manager_start()` fires `WIFI_MGR_NO_CRED`, which triggers
`ble_provisioning_start()`.

### Event Callbacks (`src/app_handlers.c`)

All event callbacks and nav wiring live here:

| Symbol                                  | Role                                                                                                                     |
|-----------------------------------------|--------------------------------------------------------------------------------------------------------------------------|
| `on_button_press`                       | Maps BSP button events → `nav_handle_action()`; handles emergency IO14 hold directly                                     |
| `on_wifi_event`                         | WiFi manager state machine transitions; calls `microlink_init/start` on first CONNECTED, `microlink_rebind` on reconnect |
| `on_ble_prov_event`                     | BLE provisioning lifecycle events                                                                                        |
| `on_sntp_sync`                          | NTP sync status updates                                                                                                  |
| `app_handlers_register_nav_callbacks()` | Wires `do_reset_wifi`, `do_sleep_now`, `do_ntp_resync` and `do_provisioning` into nav system — called once at boot            |

`on_wifi_event` starts BLE provisioning **only** on `NO_CRED`. `NO_MATCH`, `ALL_FAILED` and
`DISCONNECTED` schedule a reconnect instead (30s doubling to 5min) — losing coverage must never drop the device into
provisioning, and a retry must always stay armed.

### Components

| Component                      | Purpose                                                                                                                                                                                                                    |
|--------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `components/bsp/`              | Board Support: display init, battery (GPIO4/ADC1_CH3), backlight (LEDC), buttons                                                                                                                                           |
| `components/ui/`               | LVGL UI — modular widgets, see below                                                                                                                                                                                       |
| `components/wifi_manager/`     | WiFi state machine: IDLE → SCANNING → CONNECTING → VERIFYING → CONNECTED                                                                                                                                                   |
| `components/ble_provisioning/` | BLE WiFi provisioning via `espressif/network_provisioning`                                                                                                                                                                 |
| `components/settings/`         | NVS-backed settings: theme, brightness, sleep timeout (H/M/S)                                                                                                                                                              |
| `components/sntp_sync/`        | NTP time synchronization; skips initial sync on deep-sleep wake if recently synced                                                                                                                                         |
| `components/deep_sleep/`       | Auto-sleep timer (inactivity) + manual trigger + ext1 wakeup on GPIO0/GPIO14. Cancellable during the fade; declines while an inhibit callback says so. Cuts the LCD rail and latches it — see the hold/release warning below |
| `components/lcd_backlight/`    | LCD backlight driver via LEDC PWM                                                                                                                                                                                          |
| `components/microlink/`        | Tailscale VPN client — symlink → `vendor/microlink/components/microlink`                                                                                                                                                   |
| `components/wireguard_lwip/`   | WireGuard lwIP netif — symlink → `vendor/microlink/.../wireguard_lwip`. Third-party BSD-3 ([smartalock/wireguard-lwip](https://github.com/smartalock/wireguard-lwip)), diverged fork — see `THIRD_PARTY.md` before syncing |

### UI Component (`components/ui/`)

Public API: `ui_init(bool is_light)` and `ui_set_theme(bool is_light)`. Callers have zero knowledge of screens or
widgets.

`ui_init()` only sets the LVGL theme, then calls `nav_init()` which creates and loads the initial clock screen.

| Source                 | Role                                                                                              |
|------------------------|---------------------------------------------------------------------------------------------------|
| `ui.c`                 | Theme init + delegates to `nav_init()`                                                            |
| `nav.c`                | **Navigation state machine** — owns all screen transitions (Clock ↔ Menu ↔ Settings)              |
| `clock_face_text.c`    | HH:MM:SS (DS-Digital 48) + DD/MM/YYYY (DS-Digital 16), internal 1s LVGL timer                     |
| `status_bar.c`         | Tailscale(⇄)/NTP(syncing-only)/WiFi/battery icons; 30s LVGL timer for battery; reads BSP directly |
| `prov_screen.c`        | QR code overlay shown during BLE provisioning                                                     |
| `menu_screen.c`        | Menu list screen                                                                                  |
| `settings_screen.c`    | Scrollable settings list with inline edit — 4 groups, 12 items (+ 4 section headers)              |
| `device_info_screen.c` | System Info screen — 12 read-only rows, scrollable (5 visible), timers 1s/10s/30s                 |

**Navigation flow:**

```
Clock → (BOOT long press / SELECT) → Menu → (SELECT) → Settings
                                            → (SELECT) → System Info
Settings (12 items across 4 groups, 5 visible at a time, scrollable):
  — Display —   Theme, Brightness
  — Clock —     Time Format (24H/12H), Show Seconds, Timezone
  — Sleep —     Sleep H, Sleep M, Sleep S, Sleep Now
  — Network —   NTP Resync, Reset WiFi, Provisioning
UP/DOWN navigate, SELECT = enter edit / execute action, BACK = return to Menu
Edit mode (TOGGLE/RANGE items): UP/DOWN change value (auto-saved to NVS + applied live), SELECT or BACK exits
Action items (Sleep Now, NTP Resync, Reset WiFi, Provisioning): SELECT executes immediately, no edit mode

System Info (12 rows, 5 visible, scrollable):
  Chip, Firmware, MAC, Free Heap, Total Heap, Uptime, SSID, IP,
  Last NTP (HH:MM DD/MM/YY), TS Status, TS IP, Battery
UP/DOWN scroll, BACK = return to Menu
```

**Nav public API** (`nav.h`):

```c
void nav_init(void);                                    // creates + loads clock screen
nav_action_cb_t nav_handle_action(nav_action_t action); // called by on_button_press; returns
                                                        // deferred work to run OUTSIDE the LVGL lock
void nav_register_reset_wifi_cb(nav_action_cb_t cb);   // wired by app_handlers_register_nav_callbacks
void nav_register_sleep_cb(nav_action_cb_t cb);        // wired by app_handlers_register_nav_callbacks
void nav_register_ntp_resync_cb(nav_action_cb_t cb);   // wired by app_handlers_register_nav_callbacks
void nav_register_provisioning_cb(nav_action_cb_t cb); // wired by app_handlers_register_nav_callbacks
```

**Nav actions** map to buttons:
| Action | Button | |--------|--------| | `NAV_ACTION_UP` | BOOT short press | | `NAV_ACTION_SELECT` | BOOT long
press | | `NAV_ACTION_DOWN` | IO14 short press | | `NAV_ACTION_BACK` | IO14 long press |

Clock face is swappable: replace `clock_face_text.c` with another implementation in `components/ui/CMakeLists.txt`.

### Button Actions

| Button                     | Short press (< 800ms)                  | Long press (≥ 800ms)                              | Emergency hold (≥ 3s)                                               |
|----------------------------|----------------------------------------|---------------------------------------------------|---------------------------------------------------------------------|
| BOOT (GPIO0)               | Navigate UP / increase value in edit   | SELECT / open Menu from Clock, enter edit mode    | —                                                                   |
| IO14 (GPIO14)              | Navigate DOWN / decrease value in edit | BACK / exit edit mode (no-op on Clock face)       | Reset WiFi → BLE provisioning (or `esp_restart()` if BLE RAM freed) |
| BOOT + IO14 simultaneously | —                                      | Trigger deep sleep (backlight fades 1.5s → sleep) | —                                                                   |

Any button press during the fade cancels the sleep and restores brightness. Sleep is declined
outright while the provisioning QR is on screen.

Theme and brightness are now adjusted via the Settings screen (Settings → Theme, Settings → Brightness).

### WiFi Manager API

Single-credential NVS model (namespace `wifi_cred`, keys `ssid`/`pass`):

```c
wifi_manager_set_credential(ssid, pass)
wifi_manager_clear_credential()
wifi_manager_has_credential()
```

## Critical Rules

### UI Updates

**Always use LVGL timers** for UI updates — never FreeRTOS tasks with `lvgl_port_lock/unlock`. FreeRTOS task updates
cause rendering artifacts on the Intel 8080 bus.

When touching LVGL from an event callback, acquire the lock:

```c
lvgl_port_lock(0);
// LVGL calls here
lvgl_port_unlock();
```

**Never hardcode colors** (e.g., `lv_color_white()`). The UI supports Light and Dark themes — use theme-aware color
access.

### Deep Sleep: the LCD rail is latched

`bsp_display_power_off()` drives `PIN_LCD_PWR` (GPIO15) low and calls `gpio_hold_en()` on it, so the
rail stays off through deep sleep. **That latch outlives the sleep and the reboot that follows.**
`init_power()` in `bsp_display.c` releases it with `gpio_hold_dis()` after driving the pad high —
that order is required by the driver. Remove or reorder those calls and the device wakes with a
permanently dark screen and looks bricked. It is not: reflashing runs `gpio_hold_dis()` again.

`gpio_deep_sleep_hold_en()` is deliberately **not** used — it would latch every digital pad,
including the two wake buttons. GPIO15 is inside the ESP32-S3 RTC range (0-21), so `gpio_hold_en()`
alone survives sleep.

### BLE Provisioning Gotchas

1. **`ble_provisioning_release_memory()` is one-way.** Frees ~110 KB BLE RAM. Subsequent `ble_provisioning_start()`
   returns `ESP_ERR_INVALID_STATE`. Requires `esp_restart()` to re-provision.

2. **The QR overlay is dismissible, and that is deliberate.** While it is up, nav actions are swallowed
   (`on_button_press`) so no screen transition can delete it — but `BACK` (IO14 long)
   hides it and returns to the clock, with BLE still advertising. A device that has never been provisioned stays in this
   state indefinitely and must remain usable as a clock. Settings → Network → **Provisioning** brings it back;
   `BLE_PROV_FAILED` re-shows it automatically.

3. **Wrong-password retry:** On `BLE_PROV_FAILED`, do NOT stop/restart the BLE manager. Clear the NVS credential and
   re-show the QR overlay so the phone retries over the existing BLE connection.

4. **Post-provisioning WiFi:** After `BLE_PROV_SUCCESS`, `network_prov_mgr` leaves WiFi connected.
   `wifi_manager.c::try_connect_candidate()` must call `esp_wifi_disconnect()` + `vTaskDelay(300ms)` before
   reconnecting, otherwise `ASSOC_LEAVE` triggers a 15-second timeout → `WIFI_MGR_ALL_FAILED`.

5. **Security level:** Uses Security 2 (SRP6a). Password = last 4 bytes of WiFi MAC as 8 hex chars (e.g. `D917D7DC`),
   generated fresh each `ble_provisioning_start()` call via `esp_srp_gen_salt_verifier()`. Username is fixed to
   `"wifiprov"` — the Espressif BLE Prov app hardcodes this value regardless of the QR code `username` field.
   Salt+verifier are heap-allocated and freed on `NETWORK_PROV_END`.

6. **BLE device name:** `PROV_ZenClock_XXYY` where XXYY = last 2 bytes of MAC address.

### Required sdkconfig Keys

If BLE provisioning breaks, verify these six keys in `sdkconfig.lilygo-t-display-s3`:

```
CONFIG_BT_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_BLE_ENABLED=y
CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y        # ESP32-S3 defaults BLE 5.0; legacy GAP needs this
CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2=y  # Kconfig-guarded, required for SRP6a
CONFIG_LV_USE_QRCODE=y                        # LVGL QR widget, off by default
```

## Timezone

Set as `setenv("TZ", "UTC-7", 1)` in `app_main` — POSIX sign-inversion convention for UTC+7.

## Dependencies

Managed components (in `managed_components/`):

- `espressif__esp_lvgl_port` — LVGL port for ESP-IDF
- `espressif__network_provisioning` ^1.2.4 — BLE provisioning manager
- `espressif__cjson` — JSON (used by provisioning QR)
- `lvgl__lvgl` — LVGL graphics library

Declared in `src/idf_component.yml`.

Vendor submodule (in `vendor/`):

- `vendor/microlink` — MicroLink Tailscale client (branch: `esp-idf-6x-compat`)
    - Symlinked into `components/microlink/` and `components/wireguard_lwip/`
    - Clone the repo with `--recursive` or run `git submodule update --init` after clone

## Tailscale / MicroLink

MicroLink connects to Tailscale on every `WIFI_MGR_CONNECTED` event:

- First connect: `microlink_init()` + `microlink_start()` — full registration, key exchange
- Reconnect: `microlink_rebind()` — reopens sockets, preserves VPN session and WireGuard keys (~7s recovery)
- Auth key and device name come from Kconfig (`CONFIG_ML_TAILSCALE_AUTH_KEY`, `CONFIG_ML_DEVICE_NAME`)
- Skipped entirely if no auth key configured and no stored credentials (`microlink_has_stored_credentials()`)

NVS namespaces used by MicroLink: `"microlink"` (keys), `"ml_peers"` (peer cache). To factory-reset Tailscale state:
call `microlink_factory_reset()` **before** `microlink_init()`.

**UI integration:**

- Status bar: `LV_SYMBOL_SHUFFLE` (⇄) icon left of SNTP — dim=idle, orange=connecting, green=connected, red=error
- System Info rows: `TS Status` (state string) + `TS IP` (VPN IP when connected, else N/A)
- A 10s `esp_timer` in `app_handlers.c` polls `microlink_get_state()` and calls `status_bar_set_ts_status()`
- `device_info_screen_set_ml(s_ml)` called after `microlink_start()` and `microlink_rebind()`

**Status bar icon visibility:**

- NTP (`LV_SYMBOL_REFRESH`): visible only while syncing (orange); hidden at all other times — chain collapses
  automatically
- Tailscale (`LV_SYMBOL_SHUFFLE`): always visible, color reflects connection state
