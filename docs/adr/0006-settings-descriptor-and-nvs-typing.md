# Settings descriptor table and NVS typing

**The settings descriptor table owns persistence and validation only — never labels, edit step, or
hardware effect.** The table (`components/settings/settings_table.c`) holds each setting's NVS key,
default, valid range and boolean polarity, and nothing else. An earlier sketch put `.label` and
`.step` in it too. They were rejected: a display string and a button-press granularity are
presentation, not domain rules, and including them would have forced the table to carry entries for
the eight section-header and action rows that have no key, default or range at all. Keeping it
narrow is also what keeps it free of `nvs.h`, `esp_log.h` and `lvgl.h`, which is what lets it build
under `[env:native]` — the clamp and polarity rules are now the first non-trivial settings logic in
this repo with host-side tests.

**`settings_key_t` is a separate enum from `settings_row_t`, not a replacement for it.** ADR-0004's
rule that `settings_row_t` is the single source of truth for row order still holds. The screen's row
table points into `settings_key_t` with a `.skey` field rather than restating the list, so there is
still exactly one encoding of row order. Collapsing the two enums would have meant either giving
headers and actions fake descriptors or giving the descriptor array holes indexed by UI concerns.

**All settings moved from `u8` to `i8` in NVS with no migration, accepting a one-time reset.** Before
the descriptor table, seven of the eight keys were written with `nvs_set_u8()` and only `tz_offset`
with `nvs_set_i8()`. A single-typed generic accessor has to pick one, and `i8` is the only choice
that holds the timezone's negative range; every other value (brightness ≤ 100, sleep ≤ 59) fits it
comfortably.

ESP-IDF type-checks on read. With `CONFIG_NVS_LEGACY_DUP_KEYS_COMPATIBILITY` off — it is off in
`sdkconfig.lilygo-t-display-s3` — `nvs_get_i8()` on a key written by `nvs_set_u8()` returns
`ESP_ERR_NVS_TYPE_MISMATCH` (`nvs_page.cpp`), leaving the output untouched rather than
reinterpreting it. So the seven affected settings read as their defaults exactly once after
upgrade, and the first write of each replaces the stale `u8` item (`nvs_storage.cpp` finds the old
item as `ItemType::ANY` and erases it, so no duplicate key is left behind).

Two alternatives were considered and rejected:

- **A boot-time migration** (read `u8`, rewrite as `i8`) preserves user values but adds a code path
  that runs exactly once per device in its lifetime, cannot be exercised on the host test bench,
  and fails silently if wrong.
- **A permanent read fallback** (try `i8`, fall back to `u8`) avoids the reset but keeps the dual
  encoding forever, which is the drift this change exists to remove.

What is actually lost is small and self-correcting: brightness returns to 100 (brighter, not darker
— it cannot lock a user out), theme to Dark, time format to 24H, show-seconds to On, and the
auto-sleep timeout to disabled. The timezone, the one setting a user would most resent re-entering,
was already `i8` and survives untouched. `settings_get()` logs a warning on the mismatch so the
one-time reset is visible in the serial log rather than looking like a fault.

**Clamping happens on read, on write, and in `settings_apply_timezone()`.** The first two follow
ADR-0004's brightness rule. The third is there because `settings_apply_timezone()` is public and is
the only function that actually reaches `setenv("TZ", ...)`: an out-of-range offset there builds a
TZ string newlib cannot parse and silently falls back to UTC from, producing a wrong clock with
nothing on screen explaining it — reached by the one path that never touches NVS.

**Boolean polarity is one `.invert` flag, and `settings_option_index()` is involutive.** Theme's
option array is `{Dark, Light}` so its index equals the stored boolean, while Time Format's
`{24H, 12H}` and Show Secs' `{On, Off}` are inverted. That inversion used to be written by hand at
three sites per field with no compiler check, so reordering an options array silently flipped the
stored value. Because the mapping is its own inverse, the load path and the persist path now call
the same function instead of maintaining two expressions that must stay opposite.

**`app_main()` keeps its hand-written boot sequence rather than looping over the table.** Only one
of the four settings with a hardware effect applies identically at boot and on edit
(`settings_apply_timezone`). The other three differ deliberately: `ui_init()` creates the screen
where `ui_set_theme()` restyles it, `deep_sleep_init()` creates the timer where
`deep_sleep_update_timeout()` adjusts it, and boot fades the backlight in over 2 s where an edit
applies it instantly. Driving both from one apply function would have cost the boot fade and hidden
the load-bearing ordering in `app_main()` (display, then NVS, then the LVGL lock, then the
backlight).
