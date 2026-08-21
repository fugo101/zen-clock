# Device UI list and safety invariants

**`SETTINGS_BRIGHTNESS_MIN` is clamped on both read and write, not just write.** Clamping only the
writer would leave an already-bricked device (brightness saved below the floor by some earlier bug)
stuck at 0 — a black screen with no visible way to reach the Settings item that would fix it. The
same constant feeds the Brightness item's `.min`, so the read clamp and the edit-range floor can't
drift apart.

**NVS writes from Settings edit mode are debounced behind a 1s one-shot LVGL timer, flushed early on
exit.** Holding UP on Brightness used to issue roughly 11 blocking flash commits in 2 seconds. The
debounce reduces write frequency; it deliberately does not move the write off the LVGL task — that
remaining cost is known and accepted, not fixed. `settings_screen_destroy()` must flush the pending
write itself: any future caller that deletes the settings screen a different way will silently lose
the last unsaved edit, since nothing else triggers that flush.

**Timezone is whole hours only, and this is a scope decision, not an oversight.** Supporting
half-hour/45-minute offsets would mean changing the NVS-stored representation from hours to
15-minute units, migrating already-saved values, and touching the RANGE item plus `timezone_fmt()`
and its unit tests — a feature-sized change, not a cleanup. The device's only deployment today is
UTC+7, which is a whole-hour zone.

**`settings_row_t` and `menu_row_t` are the single source of truth for row order, on purpose.**
Before they existed, row order was independently encoded up to six different ways across the
codebase — named defines, two unrelated `case` ladders, raw array subscripts, and comments assuming
"row 0 is a header." Reordering the visible list silently misrouted actions to the wrong row.
Nothing that touches row order may restate the list in prose; it must reference the enum.

This survived the descriptor table of ADR-0006 intact. `settings_key_t` identifies *which
persisted setting* a row edits and is a separate, shorter enum — eight of `settings_row_t`'s
sixteen entries are section headers and action rows with no stored value at all. The two are
joined by a single `.skey` field on each row, so the visible list still has exactly one encoding.
A row's edit range is no longer written down here either: `.min`/`.max` are filled from the
descriptor when the screen is created, which is why the same two numbers now bound both what the
user can dial in and what may be stored.

**The header-skip loop in Settings' focus-prev/next navigation carries an explicit iteration cap.**
The underlying `ui_circ_next`/`ui_circ_prev` helpers are pure modular arithmetic with no built-in
termination guarantee, so a future table edit that leaves zero non-header rows would spin the LVGL
task forever. `bugprone-infinite-loop` is disabled repo-wide (it has false positives elsewhere in
this codebase), so static analysis would never catch a regression here — the cap is the only
safeguard.

**`show_screen()`'s `content_before_status_bar` flag is explicit, not inferred.** LVGL z-order
follows creation order, and the clock screen genuinely needs its content created before the status
bar while the other three screens need the reverse. Normalizing this silently (always one order or
the other) would be a real, easy-to-miss visual regression on whichever screen didn't match the
chosen default.

**The five per-screen `*_destroy()` functions are deliberately not unified into one shape.** They
were traced individually and are genuinely different: only Settings has a `flush_pending()` side
effect, only Settings deletes a child object via `hide_edit_box()`, and only Menu carries a
load-bearing "don't reset focus" invariant that `nav.c` depends on. Forcing a common destroy shape
would hide exactly the part each one exists to do — the one piece of real duplication that was
worth extracting is `ui_timer_delete()`, which they all use.

**A failed NTP sync is a `switch` over the sync-status enum with no `default:` case, not an
if/else chain.** `SNTP_STATUS_FAILED` once fell into an `else` branch and hid the status-bar icon
entirely — the clock kept showing a wrong time (`00:00:00 01/01/1970`) with nothing on screen
indicating why. The no-default `switch` means `-Werror` catches the next status value that isn't
explicitly handled, instead of silently swallowing it the way the `else` branch did.

**The button ISR's post-release drain intentionally discards the other button's queued events, and
both buttons share one queue.** This is deliberate anti-bounce, not a missed case — no current UX
depends on two fast sequential presses across both buttons. It has no effect on the BOOT+IO14
deep-sleep combo, which polls `gpio_get_level()` directly rather than going through the queue at
all, so the drain can't accidentally suppress that path.
