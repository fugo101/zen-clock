# UI Component

LVGL-based UI for ZenClock. Thin public surface: callers only touch `ui.h` and `nav.h`.

## Architecture

```
app_handlers.c
      │
      ▼
   ui_init()          ← lvgl theme + nav_init()
      │
      ▼
   nav.c              ← navigation state machine
   ├── clock screen       → clock_face_text.c + status_bar.c
   ├── menu screen        → menu_screen.c + status_bar.c
   ├── settings screen    → settings_screen.c + status_bar.c
   └── system info screen → device_info_screen.c + status_bar.c
                                  ↕ (overlay)
                             prov_screen.c
```

Nav owns all screen creation and teardown. Nothing outside `nav.c` calls `clock_face_create`, `status_bar_create`, or
the other widget constructors.

## File Organization

```
ui/
├── ui.h / ui.c                           ← Public entry point + theme state
├── nav.h / nav.c                         ← Navigation state machine
├── clock_face.h / clock_face_text.c      ← Clock face widget (DS-Digital font)
├── status_bar.h / status_bar.c           ← Status bar widget
├── prov_screen.h / prov_screen.c         ← BLE provisioning overlay
├── menu_screen.h / menu_screen.c         ← Menu screen
├── settings_screen.h / settings_screen.c ← Settings screen with inline edit
├── device_info_screen.h / device_info_screen.c ← System Info (12 rows, scrollable)
├── ui_list.h / ui_list.c                 ← Shared LVGL scroll/timer helpers + layout constants
│                                            (kept out of ui_utils.h — see below)
├── ui_utils.h / ui_utils.c               ← Pure helpers (fmt_bytes, ui_circ_next/prev) — no LVGL,
│                                            no ESP-IDF; symlinked into test/test_pure_logic/'s
│                                            host-side Unity tests, so nothing here may include LVGL
├── fonts/
│   ├── DS-DIGIT.TTF                      ← DS-Digital source font (7-segment style)
│   ├── DS-DIGI.TTF / DS-DIGIB.TTF / DS-DIGII.TTF ← other weights (unused)
│   ├── DIGITAL.TXT                       ← font license
│   ├── lv_font_ds_digital_48.h/.c        ← digits/colon/slash/space at 48px (4bpp)
│   └── lv_font_ds_digital_16.h/.c        ← A/M/P letters at 16px (4bpp, for AM/PM label)
└── CMakeLists.txt
```

The clock face is swappable: replace `clock_face_text.c` with another implementation in `CMakeLists.txt` without
touching any header.

### Regenerating the fonts

Two separate font files — one for digits, one for AM/PM letters:

```bash
# 48px — digits, colon, slash, space (clock display)
lv_font_conv --bpp 4 --size 48 \
  --font components/ui/fonts/DS-DIGIT.TTF \
  --range 0x30-0x39,0x3A,0x2F,0x20 \
  --format lvgl \
  --output components/ui/fonts/lv_font_ds_digital_48.c \
  --no-compress

# 16px — A, M, P (AM/PM label, 12H mode only)
lv_font_conv --bpp 4 --size 16 \
  --font components/ui/fonts/DS-DIGIT.TTF \
  --range 0x41,0x4D,0x50 \
  --format lvgl \
  --output components/ui/fonts/lv_font_ds_digital_16.c \
  --no-compress
```

**Post-generation fixes required every time** (`lv_font_conv` always produces broken output):

1. **`.c` file** — replace the broken include block at the top:
   ```c
   // Remove this:
   #ifdef LV_LVGL_H_INCLUDE_SIMPLE
   #include "lvgl.h"
   #else
   #include "lvgl/lvgl.h"
   #endif
   // Replace with:
   #include "lvgl.h"
   ```

2. **`.h` file** — wrap with `__cplusplus` guard (required for C++ consumers):
   ```c
   #pragma once

   #ifdef __cplusplus
   extern "C" {
   #endif

   #include "lvgl.h"

   extern const lv_font_t lv_font_ds_digital_48; // or _16, match the size

   #ifdef __cplusplus
   }
   #endif
   ```

## Public APIs

### `ui.h` — entry point

```c
void ui_init(bool is_light);             // init LVGL theme + shared bg style, call nav_init()
void ui_set_theme(bool is_light);        // switch light/dark theme; updates bg on all screens instantly
bool ui_is_light_theme(void);            // query current theme state
void ui_apply_screen_bg(lv_obj_t *scr); // attach shared bg style to a new screen object
```

Screen background colors are managed via a shared `lv_style_t` so `ui_set_theme()` calls
`lv_obj_report_style_change()` once and LVGL refreshes all screens automatically — no manual per-screen updates needed.

**Theme color palette:**

| Element      | Light (`#e6e6e6` bg) | Dark (`#0d0d0d` bg) |
|--------------|----------------------|---------------------|
| Clock digits | `#333333`            | `#01ddff`           |
| Date label   | `#666666`            | `#007a99`           |
| Screen bg    | `#e6e6e6`            | `#0d0d0d`           |

### `nav.h` — navigation

```c
typedef enum {
    NAV_ACTION_UP,      // BOOT short — move up / increase
    NAV_ACTION_DOWN,    // IO14 short — move down / decrease
    NAV_ACTION_SELECT,  // BOOT long  — select / enter
    NAV_ACTION_BACK,    // IO14 long  — back
} nav_action_t;

typedef void (*nav_action_cb_t)(void);

void nav_init(void);

// Returns deferred work, or NULL. Call it only AFTER releasing the LVGL lock —
// action items block for seconds (WiFi stop, NVS, BLE bring-up).
nav_action_cb_t nav_handle_action(nav_action_t action);

void nav_register_reset_wifi_cb(nav_action_cb_t cb);
void nav_register_sleep_cb(nav_action_cb_t cb);
void nav_register_ntp_resync_cb(nav_action_cb_t cb);
void nav_register_provisioning_cb(nav_action_cb_t cb);
```

### `clock_face.h` — clock face widget

```c
void clock_face_create(lv_obj_t *parent); // HH:MM:SS + DD/MM/YYYY, 1s LVGL timer
void clock_face_destroy(void);            // stop timer before deleting parent
```

### `status_bar.h` — status bar widget

```c
void status_bar_create(lv_obj_t *parent);
void status_bar_destroy(void);
void status_bar_set_wifi_status(wifi_status_t status); // incl. NO_INTERNET and PROVISIONING
void status_bar_set_sntp_status(sntp_status_t status); // SYNCING orange, FAILED red, otherwise hidden
void status_bar_set_ts_status(ts_status_t status);     // Tailscale: idle/connecting/connected/error
void status_bar_register_battery_cb(status_bar_battery_cb_t cb); // cb(battery_view_t) — one, last writer wins
```

Icon chain (right-to-left): `[TS ⇄] [NTP ↻ — syncing or failed] [WiFi] [BatIcon] [BatPct]`

| WiFi state | Colour |
|---|---|
| `DISCONNECTED` | dim, no colour |
| `SCANNING` | 70% opacity, no colour |
| `CONNECTING` | orange |
| `VERIFYING` | light blue |
| `CONNECTED` | green |
| `NO_INTERNET` | **yellow** — associated with an IP, but the DNS probe failed |
| `PROVISIONING` | cyan |

`NO_INTERNET` clears when NTP next succeeds — reaching a time server proves the internet is back.

`status_bar_set_wifi_status()`, `..._sntp_status()` and `..._ts_status()` **publish**: they write a
value, take no lock, and paint nothing. `status_bar_reconcile(force)` — driven by `ui.c`'s 250 ms
tick and by `status_bar_create()` with `force=true` — repaints any icon whose published value
differs from what is on screen.

Internal 30-second LVGL timer refreshes the battery view automatically, and backstops the reconcile
tick should its timer fail to allocate. It is the only ADC poll in the
UI; the reading is turned into a `battery_view_t` by `components/battery_view/` and both the painting
here and the low-battery brightness clamp in `src/app_handlers.c` consume that same view. Nothing in
this file decides what "low" means.

### `prov_screen.h` — BLE provisioning overlay

```c
void prov_screen_show(const char *device_name, const char *password); // publish: QR overlay up
void prov_screen_hide(void);                    // publish: overlay down (BLE keeps advertising)
bool prov_screen_is_visible(void);              // is the overlay *meant* to be up?
void prov_screen_reconcile(void);               // LVGL task only — build/tear down to match
```

Show and hide **publish an intent**; they take no LVGL lock and are callable from any task
(the BLE default event loop, `btn_worker`). `ui.c`'s reconcile tick builds or tears down the
overlay within one tick, and rebuilds it if a screen transition deletes it while the intent is
still on. `prov_screen_is_visible()` reports that intent, not the widget, so it leads what is on
screen by up to one tick in both directions — which is what its callers (the deep-sleep inhibit
and the nav-action guard) want. See `docs/adr/0007-published-ui-state.md`.

While the overlay is up, `on_button_press` swallows nav actions so no screen transition can delete it — except `BACK`,
which hides it. Provisioning continues in the background; Settings → Network → Provisioning re-opens it. An unprovisioned
device sits here indefinitely and must stay usable as a clock, so this is a dismissable overlay, not a modal.

### `menu_screen.h`

Row order is `menu_row_t` (`MENU_ROW_SETTINGS`, `MENU_ROW_DEVICE_INFO`) — the single source of
truth `nav.c` compares focus against, instead of raw `0`/`1` literals.

```c
void menu_screen_create(lv_obj_t *parent);
void menu_screen_destroy(void);      // nulls widgets; deliberately does NOT reset focus — nav.c owns that
void menu_screen_focus_prev(void);   // stops at first item
void menu_screen_focus_next(void);   // stops at last item
int  menu_screen_get_focus(void);
void menu_screen_set_focus(int index);
```

### `device_info_screen.h` — System Info screen

```c
void device_info_screen_create(lv_obj_t *parent);
void device_info_screen_destroy(void);
void device_info_screen_scroll_up(void);
void device_info_screen_scroll_down(void);
void device_info_screen_set_ml(microlink_t *ml); // pass s_ml after microlink_start(); NULL = disabled
```

11 read-only rows (5 visible, UP/DOWN to scroll):
`Chip`, `Firmware`, `MAC`, `Free Heap`, `Total Heap`, `Uptime`, `SSID`, `IP`, `Last NTP`, `TS Status`, `TS IP`

No Battery row: the status bar is mounted on every screen including this one, so it was showing the
same fact twice — and, reading the ADC on its own offset timer, sometimes disagreeing. See
`docs/adr/0001-battery-percentage-source.md`.

Refresh: static on create (Chip/Firmware/MAC/Total Heap) · 1s (Uptime) · 10s (Heap/SSID/IP/Last NTP/TS)

### `settings_screen.h`

Row order is `settings_row_t` (16 enumerators — 4 section headers + 12 items, `SETTINGS_ROW_COUNT`
doubles as the array bound) — the single source of truth every index-based lookup uses.

```c
void settings_screen_create(lv_obj_t *parent);
void settings_screen_destroy(void);  // flushes any pending debounced NVS write, then nulls widgets
void settings_screen_focus_prev(void);
void settings_screen_focus_next(void);
int  settings_screen_get_focus(void);
void settings_screen_set_focus(int index);
bool settings_screen_is_action_item(int index);
nav_action_cb_t settings_screen_resolve_action(int index, nav_action_cb_t cb);
void settings_screen_enter_edit(int index);
void settings_screen_exit_edit(void);
void settings_screen_edit_increase(void);
void settings_screen_edit_decrease(void);
```

## Navigation Flow

```
Clock screen
  └─ BOOT long press (SELECT) ────────────→ Menu screen
                                               ├─ UP/DOWN: navigate items
                                               ├─ SELECT:  enter Settings or System Info
                                               └─ BACK:    → Clock screen

Settings screen (16 rows = 4 section headers + 12 items, scrollable — 5 visible at a time)
  Row order: see `settings_row_t` in settings_screen.h — the source of truth, not restated here
  ├─ UP/DOWN: navigate items (section headers are skipped automatically)
  ├─ SELECT:  enter edit mode (TOGGLE/RANGE) or execute action (ACTION items)
  └─ BACK:    → Menu screen

Settings edit mode (TOGGLE/RANGE items only)
  ├─ UP/DOWN: change value (auto-saved to NVS + applied live)
  ├─ SELECT:  exit edit mode
  └─ BACK:    exit edit mode

Action items (Sleep Now, NTP Resync, Reset WiFi, Provisioning): SELECT fires callback, no edit mode

System Info screen (11 rows, 5 visible at a time)
  Rows: Chip, Firmware, MAC, Free Heap, Total Heap, Uptime, SSID, IP, Last NTP, TS Status, TS IP
  ├─ UP/DOWN: scroll
  └─ BACK:    → Menu screen
```

## Key Constraints

- **LVGL timers only** for UI updates — never FreeRTOS tasks.
- **Shared styles for theme colors** — screen bg uses a single `lv_style_t`; inline `lv_obj_set_style_*` calls
  per-object won't auto-update on theme switch.
- **LVGL lock required** — all external LVGL calls need `lvgl_port_lock(0)` / `lvgl_port_unlock()`.
  The exception is the six **publish** functions — `status_bar_set_{wifi,sntp,ts}_status()`,
  `prov_screen_show()/hide()`, `device_info_screen_set_ml()` — which touch no LVGL object and must
  be called *without* wrapping them in a lock from a foreign task. `*_reconcile()` is the opposite:
  LVGL lock required, and only `ui.c`'s tick and `status_bar_create()` may call it.
- **Nav owns screen lifecycle** — do not call widget constructors from outside `nav.c`. Always call
  `ui_apply_screen_bg(scr)` after `lv_obj_create(NULL)` in `nav.c`.
- **Clock face is swappable** — swap `clock_face_text.c` in `CMakeLists.txt`; `clock_face.h` interface stays the same.
- **Clock text colors** are set at `clock_face_create()` time via `ui_is_light_theme()` — they update on next screen
  creation after a theme change, not live.
