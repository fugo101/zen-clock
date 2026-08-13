# ZenClock

A beautiful clock project running on the **LilyGo T-Display-S3** board.

## Hardware

| Spec          | Value                                                                                      |
|---------------|--------------------------------------------------------------------------------------------|
| **Board**     | LilyGo T-Display-S3 (non-touch, v1.2)                                                      |
| **MCU**       | ESP32-S3R8 (Dual-core Xtensa LX7, 240 MHz)                                                 |
| **Flash**     | 16 MB                                                                                      |
| **PSRAM**     | 8 MB Octal (OPI)                                                                           |
| **Display**   | 1.9" ST7789, 320×170, Intel 8080 8-bit parallel                                            |
| **Backlight** | PWM controlled via ESP-IDF LEDC (lcd_backlight component)                                  |
| **Battery**   | GPIO4 ADC via resistor divider                                                             |
| **Buttons**   | GPIO0 (BOOT) + GPIO14 — 3-button navigation (UP/Select, DOWN/Back, hold IO14 = WiFi reset) |

## Software Stack

| Layer            | Technology                                                                                  |
|------------------|---------------------------------------------------------------------------------------------|
| **Framework**    | ESP-IDF v6.0.1 (via PlatformIO, `platform = espressif32@7.0.1`)                             |
| **Graphics**     | LVGL 9.5.0                                                                                  |
| **LVGL Port**    | [esp_lvgl_port](https://github.com/espressif/esp-bsp/tree/master/components/esp_lvgl_port)  |
| **UI**           | Hand-written LVGL code (no external UI designer)                                            |
| **Provisioning** | espressif/network_provisioning ^1.2.4 (BLE)                                                 |
| **VPN**          | [MicroLink](https://github.com/fudio101/microlink) — Tailscale client for ESP32 (WireGuard) |
| **Build System** | PlatformIO + ESP-IDF Component Manager                                                      |

## Project Structure

```
ZenClock/
├── components/
│   ├── bsp/                   # Board Support Package (modular HAL)
│   │   ├── README.md          # 📖 Detailed architecture & API docs
│   │   ├── include/bsp.h      # Public API
│   │   ├── priv_include/      # Internal cross-module declarations
│   │   └── src/
│   │       ├── bsp_display.c  # I80 bus + ST7789 + LVGL port
│   │       ├── bsp_battery.c  # ADC + voltage + percentage
│   │       ├── bsp_backlight.c # Brightness facade
│   │       └── bsp_buttons.c  # Button ISR + debounce
│   ├── lcd_backlight/         # PWM backlight driver (LEDC wrapper)
│   ├── deep_sleep/            # Deep sleep manager (auto-sleep + manual trigger + ext1 wakeup)
│   │   └── README.md          # 📖 Deep sleep API & wake behavior docs
│   ├── settings/              # Persistent configuration via NVS
│   │   └── README.md          # 📖 Settings architecture & API docs
│   ├── sntp_sync/             # SNTP time synchronization (RTC-backed skip on wake)
│   │   └── README.md          # 📖 SNTP architecture & deep-sleep wake docs
│   ├── ui/                    # Hand-written LVGL UI
│   │   ├── README.md          # 📖 Layout, constraints & widget docs
│   │   ├── ui.h               # Public API
│   │   ├── ui.c               # Theme init + delegates to nav
│   │   ├── nav.c/.h           # Screen navigation state machine
│   │   ├── menu_screen.c/.h   # Main menu screen
│   │   ├── settings_screen.c/.h # Settings screen with inline edit
│   │   ├── device_info_screen.c/.h # System Info screen (12 rows, scrollable)
│   │   ├── clock_face.h       # Clock face interface (swappable implementation)
│   │   ├── clock_face_text.c  # Text-based clock face rendering
│   │   ├── status_bar.c/.h    # Tailscale/NTP/WiFi/battery status icons
│   │   ├── ui_list.c/.h       # Shared LVGL scroll/timer helpers + layout constants
│   │   ├── ui_utils.c/.h      # Pure helpers (no LVGL) — reused by native unit tests
│   │   └── prov_screen.c/.h   # QR code overlay for BLE provisioning
│   ├── wifi_manager/          # WiFi connection + BLE provisioning fallback
│   │   └── README.md          # 📖 WiFi manager API & architecture
│   ├── ble_provisioning/      # BLE provisioning (espressif/network_provisioning)
│   │   ├── include/ble_provisioning.h
│   │   ├── src/ble_provisioning.c
│   │   ├── CMakeLists.txt
│   │   └── idf_component.yml
│   ├── microlink -> ../vendor/microlink/components/microlink
│   │                          # Tailscale VPN client (symlink → submodule)
│   └── wireguard_lwip -> ../vendor/microlink/components/wireguard_lwip
│                          # WireGuard lwIP integration (symlink → submodule)
│                          # Third-party BSD-3 — see THIRD_PARTY.md
├── vendor/
│   └── microlink/             # git submodule (branch: main)
├── include/
│   └── board_config.h         # Pin definitions and board constants (single source)
├── src/
│   ├── main.c                 # Application entry point
│   ├── app_handlers.c/.h      # Event callbacks (button, WiFi, BLE, SNTP, MicroLink)
│   ├── CMakeLists.txt         # Main component build config
│   └── idf_component.yml      # LVGL + esp_lvgl_port dependencies
├── platformio.ini
├── partitions.csv
└── sdkconfig.lilygo-t-display-s3
```

> **Component docs:** See [`components/bsp/README.md`](components/bsp/README.md), [
`components/ui/README.md`](components/ui/README.md), [
`components/sntp_sync/README.md`](components/sntp_sync/README.md), [
`components/settings/README.md`](components/settings/README.md), [
`components/deep_sleep/README.md`](components/deep_sleep/README.md), and [
`components/wifi_manager/README.md`](components/wifi_manager/README.md) for detailed API documentation.

## Getting Started

### Prerequisites

1. [PlatformIO](https://platformio.org/) installed (VS Code extension recommended)
2. LilyGo T-Display-S3 connected via USB

### Clone

```bash
git clone --recursive https://github.com/fugo101/zen-clock.git
```

> `--recursive` is required to initialize the `vendor/microlink` submodule.
> Without it, `components/microlink` and `components/wireguard_lwip` symlinks will be dangling and the build will fail.
>
> If you already cloned without `--recursive`:
> ```bash
> git submodule update --init
> ```

### Build & Flash

```bash
# Build the project
pio run

# Flash to device
pio run -t upload

# Monitor serial output
pio device monitor
```

### Install from a Release (no PlatformIO required)

Just want to flash a board without building from source? Every
[GitHub Release](https://github.com/fugo101/zen-clock/releases) ships four binaries, a checksum
file, and the exact flash command for that version — for example
[v0.3.0](https://github.com/fugo101/zen-clock/releases/tag/v0.3.0).

1. **Install esptool** (one-time):
   ```bash
   pip install esptool
   ```
2. **Download** the release's 5 assets — `bootloader-<version>.bin`, `partitions-<version>.bin`,
   `ota_data_initial-<version>.bin`, `firmware-<version>.bin`, `checksums-<version>.txt` — from its
   Assets section, into one folder.
3. **Verify the download** (optional but recommended):
   ```bash
   sha256sum -c checksums-<version>.txt
   ```
4. **Connect the board over USB**, then flash all four images in one command (offsets are fixed —
   copy this verbatim, only the version in the filenames changes per release):
   ```bash
   esptool.py --chip esp32s3 write_flash -z --flash_mode dio --flash_freq 80m --flash_size 16MB \
     0x0     bootloader-<version>.bin \
     0x8000  partitions-<version>.bin \
     0xe000  ota_data_initial-<version>.bin \
     0x10000 firmware-<version>.bin
   ```
5. **Watch it boot** — `esptool.py` resets the board automatically once flashing finishes. To see
   serial output at 115200 baud: `python3 -m serial.tools.miniterm <port> 115200` (installed
   alongside `esptool` via `pyserial`), or `pio device monitor` if you have PlatformIO.

This works on a completely blank board — the bootloader and partition table are included, so there's
no PlatformIO project or prior flash required.

### Development

Before pushing, run the same gates `.github/workflows/ci.yml` runs:

```bash
pio test -e native              # host-side unit tests, see test/test_pure_logic/
python3 scripts/pio_check.py    # static analysis (clang-tidy)
python3 scripts/format.py --check
python3 scripts/check_secrets.py --staged   # credential leak guard
```

### First Boot

**On first boot (no WiFi credentials in NVS):**

1. Display shows ZenClock UI with smooth 2-second backlight fade-in
2. WiFi manager fires `WIFI_MGR_NO_CRED` event → triggers BLE provisioning
3. BLE provisioning QR code overlay appears — shows QR code and 8-character password (e.g. `D917D7DE`)
4. Use
   the [Espressif BLE Provisioning app](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/provisioning/provisioning.html#provisioning-tools) (
   iOS/Android),
   scan the QR code, enter the displayed password when prompted (Security 2 / SRP6a), then select your WiFi network
5. Once connected, provisioning screen closes and clock displays time (synced via SNTP)

**Offline operation (credentials already set):**

If WiFi credentials are stored but the network is unavailable at boot, the device starts normally in offline mode —
clock shows time from RTC, no BLE QR code appears. WiFi reconnects automatically with exponential backoff (30 s → 60
s → … → 5 min max). NTP syncs immediately once connection is restored.

**Button Controls:**

- **BOOT button (GPIO0)**
    - Short press: Navigate UP (or increase value in edit mode)
    - Long press: SELECT / Enter (open menu from Clock, enter edit mode, or confirm)
- **Side button (GPIO14)**
    - Short press: Navigate DOWN (or decrease value in edit mode)
    - Long press: BACK / Exit (go back or exit edit mode; **no-op on Clock face**)
    - Hold 3 seconds (EMERGENCY): Clear WiFi credentials → BLE provisioning
- **BOOT + IO14 simultaneously (≥ 800ms):** Trigger deep sleep — backlight fades over 1.5s, then the LCD rail is cut
  and the device sleeps. Press any button during the fade to call it off; press either button while asleep to wake.
  Sleep is declined while the provisioning QR is on screen. (Sleep current has not been measured on this board.)

**Navigation Flow:**

```
Clock → (BOOT long press) → Menu → (SELECT) → Settings
                                 → (SELECT) → System Info
Settings (16 rows = 4 section headers + 12 items, scrollable — 5 visible at a time):
  — Display —   Theme, Brightness
  — Clock —     Time Format (24H/12H), Show Secs, Timezone (UTC offset)
  — Sleep —     Sleep H, Sleep M, Sleep S, Sleep Now
  — Network —   NTP Resync, Reset WiFi, Provisioning

System Info (12 rows, 5 visible, scrollable):
  Chip, Firmware, MAC, Free Heap, Total Heap, Uptime,
  SSID, IP, Last NTP, TS Status, TS IP, Battery
```

TOGGLE/RANGE items use inline edit: SELECT to enter, UP/DOWN to change value (auto-saved to NVS), SELECT or BACK to
exit.
ACTION items (Sleep Now, NTP Resync, Reset WiFi, Provisioning): SELECT executes immediately.

**Auto-sleep:** Configure via Settings → Sleep H / Sleep M / Sleep S. All three at 0 disables auto-sleep.

Battery status is displayed in the top-right corner.

---

## Menuconfig (Required Settings)

After cloning or setting up the project for the first time, run:

```bash
pio run -t menuconfig
```

Ensure the following settings are configured:

| Setting          | Path                                                        | Value       |
|------------------|-------------------------------------------------------------|-------------|
| PSRAM Mode       | Component config → ESP PSRAM → SPI RAM config → Mode        | **Octal**   |
| PSRAM Speed      | Component config → ESP PSRAM → SPI RAM config → Speed       | **80 MHz**  |
| CPU Frequency    | Component config → ESP System Settings → CPU frequency      | **240 MHz** |
| FreeRTOS Tick    | Component config → FreeRTOS → Kernel → Tick rate (Hz)       | **1000**    |
| Flash Size       | Serial flasher config → Flash size                          | **16 MB**   |
| LVGL Color Depth | Component config → LVGL → Display → Color depth             | **16**      |
| Montserrat 14    | Component config → LVGL → Font usage → Enable Montserrat 14 | **✓**       |
| Montserrat 48    | Component config → LVGL → Font usage → Enable Montserrat 48 | **✓**       |

> ⚠️ **Never set PSRAM to Quad mode** — this will cause a boot loop on the T-Display-S3.

### Tailscale Credentials

> 🔴 **`sdkconfig.lilygo-t-display-s3` IS tracked in git.** `pio run -t menuconfig` writes your auth key
> straight into it, so committing after a menuconfig run leaks the key. Read this whole section first.

Tailscale auth key and device name are Kconfig options, set via menuconfig:

```bash
pio run -t menuconfig
# → MicroLink V2 Configuration → Credentials
#   ML_TAILSCALE_AUTH_KEY  = tskey-auth-xxxxxxxxxxxx
#   ML_DEVICE_NAME         = zen-clock   (or leave empty for MAC-based default)
```

**Then keep the key out of your commits.** PlatformIO builds ESP-IDF with a *single* sdkconfig file
(`board_build.esp-idf.sdkconfig_path` is passed as `-DSDKCONFIG=`, it **replaces** the file rather than
layering onto it), so there is no supported "extra credentials file" to point at. Two working options:

1. **Guard script (default).** Before every commit, run:
   ```bash
   python3 scripts/check_secrets.py
   ```
   It exits non-zero if a credential Kconfig in the tracked sdkconfig is non-empty. Wire it up as a
   pre-commit hook:
   ```bash
   printf '#!/bin/sh\nexec python3 scripts/check_secrets.py\n' > .git/hooks/pre-commit
   chmod +x .git/hooks/pre-commit
   ```
   To build with your key but commit unrelated changes, blank the key again before staging — or use:
   ```bash
   git update-index --skip-worktree sdkconfig.lilygo-t-display-s3
   ```

2. **Local-only sdkconfig.** Point the build at an untracked copy that holds your key:
   ```ini
   ; platformio.ini — local edit, do not commit
   board_build.esp-idf.sdkconfig_path = sdkconfig.local
   ```
   ```bash
   cp sdkconfig.lilygo-t-display-s3 sdkconfig.local   # then set the key in sdkconfig.local
   ```
   `sdkconfig.local` must be a *complete* sdkconfig, not a fragment. Add it to `.gitignore`.

> An earlier version of this README recommended `board_build.esp-idf.sdkconfig_extra` — **that option does
> not exist**; PlatformIO silently ignored it, so the key was never applied from that file.

> Use **reusable** auth keys for development — single-use keys expire after the first registration.

---

## Known Gotchas & Troubleshooting

### Display Garbled / Smeared

The ST7789 display orientation must be configured in **two places** with matching values. See [
`components/bsp/README.md`](components/bsp/README.md#display-orientation) for details.

### Rendering Artifacts

LVGL buffer must be set to **full screen** (`LCD_H_RES * LCD_V_RES`) in `bsp_display.c`. Smaller buffers cause smearing
when labels update text dynamically.

### Panel Container Crash

Do **not** use `lv_obj_create(parent)` as a container/panel in the UI — it causes a `StoreProhibited` crash during font
rendering. Place all widgets directly on the screen object. See [
`components/ui/README.md`](components/ui/README.md#design-constraints) for details.

---

## Credits

- Display driver architecture based on [hiruna/esp-idf-t-display-s3](https://github.com/hiruna/esp-idf-t-display-s3)
- Backlight driver adapted from [hiruna/esp-idf-aw9364](https://github.com/hiruna/esp-idf-aw9364)
- [LVGL](https://lvgl.io/) — Light and Versatile Graphics Library
- [Espressif esp_lvgl_port](https://github.com/espressif/esp-bsp)
- [MicroLink](https://github.com/fudio101/microlink) — Tailscale/WireGuard client for ESP32
- [smartalock/wireguard-lwip](https://github.com/smartalock/wireguard-lwip) — WireGuard for lwIP
  (BSD 3-Clause, Daniel Hope / Floorsense Ltd), vendored via MicroLink

See [THIRD_PARTY.md](THIRD_PARTY.md) for the full provenance and license inventory.

## License

MIT — Copyright (c) 2026 fudio101. See [LICENSE](LICENSE).

Third-party components ship under their own terms; see [THIRD_PARTY.md](THIRD_PARTY.md).
