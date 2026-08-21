# Settings Manager

> **[AI Context]** This component provides an interface for reading and writing persistent application configurations to
> the Non-Volatile Storage (NVS).
> It initializes the NVS system at boot and acts as a central hub for all persistent settings: UI theme, brightness,
> sleep timeout, time format, show-seconds, and timezone offset.

## Architecture

```
components/settings/
├── settings.h         ← Public API (7 symbols) for NVS operations
├── settings.c         ← NVS initialization and generic read/write logic
├── settings_table.h/.c ← Descriptor table: NVS key, default, range and boolean polarity for
│                          every persisted setting. Pure — no nvs.h/esp_log.h/lvgl.h — so it
│                          builds for the host-side `[env:native]` tests
├── timezone_fmt.h/.c  ← Pure TZ string formatting, split out of settings.c for the same reason
└── CMakeLists.txt     ← ESP-IDF component registration (requires nvs_flash)
```

- **NVS Flash Init**: Handled completely inside `settings_init()`. Automatically formats the NVS partition if it gets
  corrupted or contains an incompatible version.
- **Keys and Namespaces**: Hardcoded internally (`zenclock` namespace) to keep the public API clean and type-safe.

## Public API

Header: [`settings.h`](settings.h)

| Function                          | Description                                                                                                    |
|-----------------------------------|----------------------------------------------------------------------------------------------------------------|
| `settings_init()`                 | Initializes the NVS flash partition. Must be called early in `app_main()` before any reads/writes.             |
| `settings_get(key)`               | Reads one setting. Falls back to the descriptor's default when absent/unreadable, and always clamps into range. |
| `settings_get_bool(key)`          | Reads a boolean setting. Convenience over `settings_get(key) != 0`.                                            |
| `settings_set(key, val)`          | Clamps and writes one setting, logging `<nvs_key> = <value>`. A failed write is logged and swallowed.          |
| `settings_get_sleep_seconds()`    | Total stored auto-sleep timeout in seconds.                                                                    |
| `settings_apply_timezone(offset)` | Applies the offset to the running system (`setenv`+`tzset`, via `timezone_fmt()`). Clamps first.               |

`settings_table.h` additionally exports the pure helpers the table owns — `settings_desc()`,
`settings_clamp()`, `settings_option_index()` and `settings_sleep_seconds()` — all covered by
`pio test -e native`.

`SETTINGS_BRIGHTNESS_MIN` (10) and `SETTINGS_TZ_MIN`/`SETTINGS_TZ_MAX` (−12/+14) live in
`settings_table.h` alongside the descriptors that use them, and are shared with the matching UI
items' `.min`/`.max` so the stored range and the edit range can't drift apart. Clamping is applied
on read as well as on write — see ADR-0004 and ADR-0006.

## Flow

1. `app_main` calls `settings_init()`, then `settings_apply_timezone(settings_get(SETTINGS_KEY_TZ_OFFSET))`
2. `app_main` reads theme, brightness and `settings_get_sleep_seconds()` to compute initial state.
   This sequence is deliberately hand-written rather than a loop over the table — see ADR-0006
3. The Settings screen writes through `settings_set(key, value)` behind its 1s debounce timer

## Rules for AI Agents

1. **Never call `nvs_open`/`nvs_set_*` directly** from `main.c` or other components. Use this API to maintain a clean
   abstraction.
2. **Add a new setting as a descriptor**, not as a new getter/setter pair: one `settings_key_t`
   entry and one row in `s_desc[]`. Do not add typed accessors back, and do not expose raw NVS
   handles.
3. **Defaults, ranges and NVS keys belong in the descriptor** — never restated at a call site.
   `settings_get()` already handles a missing key, an unopenable partition, and a value stored by
   an older build.
4. **Never put a label, edit step or hardware effect in the descriptor.** Those belong to
   `s_items[]` and `s_apply[]` in `components/ui/settings_screen.c`. See ADR-0006.
5. **NVS key strings are frozen.** They name data on every deployed device; renaming one silently
   reverts that setting to its default.
