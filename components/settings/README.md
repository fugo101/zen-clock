# Settings Manager

> **[AI Context]** This component provides an interface for reading and writing persistent application configurations to
> the Non-Volatile Storage (NVS).
> It initializes the NVS system at boot and acts as a central hub for all persistent settings: UI theme, brightness,
> sleep timeout, time format, show-seconds, and timezone offset.

## Architecture

```
components/settings/
├── settings.h         ← Public API for NVS operations
├── settings.c         ← NVS initialization and read/write logic
├── timezone_fmt.h/.c  ← Pure TZ string formatting, split out of settings.c so it can build for
│                          the host-side `[env:native]` unit tests (settings.c pulls in
│                          esp_log.h/nvs.h and can't)
└── CMakeLists.txt     ← ESP-IDF component registration (requires nvs_flash)
```

- **NVS Flash Init**: Handled completely inside `settings_init()`. Automatically formats the NVS partition if it gets
  corrupted or contains an incompatible version.
- **Keys and Namespaces**: Hardcoded internally (`zenclock` namespace) to keep the public API clean and type-safe.

## Public API

Header: [`settings.h`](settings.h)

| Function                                | Description                                                                                               |
|------------------------------------------|-----------------------------------------------------------------------------------------------------------|
| `settings_init()`                        | Initializes the NVS flash partition. Must be called early in `app_main()` before any reads/writes.        |
| `settings_get_theme_light()`             | Returns the saved theme mode (`true` for light, `false` for dark). Returns `false` by default if not set. |
| `settings_set_theme_light(is_light)`     | Saves the specified theme mode to NVS and commits the change.                                             |
| `settings_get_brightness()`              | Returns the saved brightness percentage (`SETTINGS_BRIGHTNESS_MIN`–100). Returns `100` by default if not set. |
| `settings_set_brightness(percent)`       | Saves brightness percentage to NVS (clamped to `SETTINGS_BRIGHTNESS_MIN`–100) and commits.                |
| `settings_get_sleep_h()`                 | Returns saved sleep timeout hours (0–23). Default 0.                                                      |
| `settings_set_sleep_h(h)`                | Saves sleep hours to NVS (clamped to 0–23).                                                               |
| `settings_get_sleep_m()`                 | Returns saved sleep timeout minutes (0–59). Default 0.                                                    |
| `settings_set_sleep_m(m)`                | Saves sleep minutes to NVS (clamped to 0–59).                                                             |
| `settings_get_sleep_s()`                 | Returns saved sleep timeout seconds (0–59). Default 0.                                                    |
| `settings_set_sleep_s(s)`                | Saves sleep seconds to NVS (clamped to 0–59).                                                             |
| `settings_get_time_format_24h()`         | Returns `true` for 24H (default), `false` for 12H.                                                        |
| `settings_set_time_format_24h(is_24h)`   | Saves time format to NVS.                                                                                 |
| `settings_get_show_seconds()`            | Returns `true` to show seconds (default), `false` to hide.                                                |
| `settings_set_show_seconds(show)`        | Saves show-seconds setting to NVS.                                                                        |
| `settings_get_timezone_offset()`         | Returns UTC offset in hours (−12..+14). Default `+7`.                                                     |
| `settings_set_timezone_offset(offset)`   | Saves timezone offset to NVS.                                                                             |
| `settings_apply_timezone(offset)`        | Applies the offset to the running system (`setenv`+`tzset`, via `timezone_fmt()`). Call on boot and whenever the setting changes. |

`SETTINGS_BRIGHTNESS_MIN` (10) is defined in `settings.h` — the backlight is the only way to read
the settings screen, so 0% is not a valid setting; it's enforced on both read and write, and shared
with the Brightness item's `.min` in the UI so the two can't drift apart.

## Flow

1. `app_main` calls `settings_init()`, then `settings_apply_timezone(settings_get_timezone_offset())`
2. `app_main` calls `settings_get_theme_light()`, `settings_get_brightness()`, and `settings_get_sleep_h/m/s()` to
   compute initial state
3. UI or hardware buttons trigger a theme change via `settings_set_theme_light(new_theme)`

## Rules for AI Agents

1. **Never call `nvs_open`/`nvs_set_*` directly** from `main.c` or other components. Use this API to maintain a clean
   abstraction.
2. **Add new settings as explicit getters/setters** in `settings.h` (e.g., a hypothetical
   `settings_get_locale()`/`settings_set_locale()`). Do not expose raw NVS handles.
3. **Always handle default values** gracefully inside the getter functions if the NVS key is not found (
   `ESP_ERR_NVS_NOT_FOUND`).
