# Published UI state

**A task that is not the LVGL task publishes what it wants on screen; it does not paint it.**
`status_bar_set_{wifi,sntp,ts}_status()`, `prov_screen_show()/hide()` and
`device_info_screen_set_ml()` all write a value and return, taking no LVGL lock from any task. A
250 ms `lv_timer` in `components/ui/ui.c` runs inside `lv_timer_handler()` — so it holds the lock
by construction — and repaints whatever has changed. `status_bar_create()` replays the published
values when the bar is rebuilt on a screen change.

**This replaced a bounded 50 ms lock whose stated justification was false.** The rule had been:
callbacks on the wifi task and the shared `esp_timer` task take `lvgl_port_lock(50)`, skip the
paint on timeout, and rely on a later event to repaint. That holds only where a later event
actually comes. It did not for the two terminal WiFi states: after `WIFI_MGR_CONNECTED` the wifi
task parks in `xEventGroupWaitBits(..., portMAX_DELAY)` until a disconnect or a stop, and a skipped
paint also skipped the status bar's own cache — so the restore-on-screen-change path then replayed
the stale value on every transition. A healthy connection could show "verifying" blue for hours.
The bounded lock was the right call and the reasoning behind it stands; only the claim that nothing
was lost was wrong.

**Reconciling compares against what is on screen rather than consuming a dirty flag.** A flag is
the obvious encoding and was written first. It is wrong here: `volatile` constrains the compiler,
not the ESP32-S3's two cores, so a flag can become visible before the value it refers to. The
reconcile would clear it, paint the stale value, and — the terminal states having no follow-up
event — never repaint, which is the original bug in a rarer and equally permanent form. Comparing
makes a torn read cost one tick: the next tick still sees a difference and corrects it. The cost is
three enum comparisons every 250 ms, which is why the flag bought nothing worth the hazard.

**The reconcile tick lives in `ui.c`, not inside each widget.** The status bar is destroyed and
rebuilt on every screen change, and the provisioning overlay may need building when no screen owns
one yet; a per-widget timer would not exist at the moment its own publish arrives. One tick
outliving every widget is what makes a publish impossible to lose. `battery_timer_cb()` also calls
`status_bar_reconcile()` as a 30 s backstop, so a failed `lv_timer_create()` degrades the icons
rather than freezing them.

**The provisioning overlay is intent, not a paint, and that is the case a bounded lock could never
have served.** A skipped `prov_screen_show()` means no QR at all and a device that looks dead
during setup — there is no later event to repeat it and no honest way to call the loss free. As
published intent it cannot be skipped: the tick builds the overlay, and rebuilds it if a screen
transition deletes it while the intent is still on. `prov_screen_is_visible()` now reports that
intent rather than the widget, which also closes an unsynchronized read — the deep-sleep inhibit
callback used to touch `s_overlay` from the deep-sleep timer with no lock at all. Between a publish
and the next tick it leads what is on screen by up to one tick; that is the answer the callers
want, since an input arriving in that window belongs to the overlay, not to the screen behind it.

**One unbounded lock survives, deliberately: `nav_handle_action()` in `on_button_press()`.**
Everything else that wanted the screen changed was reporting state, so it could publish. A nav
action is not state — the button task needs the transition to have happened before it can know
what deferred work came out of it, and the screen must move on the press rather than up to a tick
later. The task it blocks is the button task, whose responsiveness `btn_worker` already exists to
protect.

**Reconciling by comparison also coalesces: a status published and superseded inside one tick is
never painted.** This is a real behaviour change, not just latency. The visible case is Settings →
Network → NTP Resync, which publishes `SNTP_STATUS_SYNCING` and then `SNTP_STATUS_SYNCED`; against a
LAN time server that round trip can finish inside 250 ms, and the orange syncing icon — the only
feedback that the action did anything — never appears. The same collapses
`SCANNING → CONNECTING → VERIFYING` on a fast associate. Accepted because the icons exist to report
the state the device is *in*, not to animate the path it took, and the terminal state is always
painted. If the resync indicator is ever wanted as an acknowledgement of the button press, that is a
minimum-visible-duration on one icon, not a reason to go back to painting from foreign tasks.

**The accepted cost is up to one tick (~250 ms) of latency on anything published.** Invisible for
status icons and for a QR overlay that a human is about to photograph, and it buys the removal of
nine hand-discharged `lvgl_port_unlock()` obligations — each one a place where unlocking a mutex
the caller did not hold was one edit away.
