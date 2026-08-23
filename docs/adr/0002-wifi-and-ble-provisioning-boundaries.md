# WiFi and BLE provisioning boundaries

The device is a clock first: it must stay usable with no WiFi, out of coverage, or on a LAN-only
network. Losing the network is normal operation, never a stuck state. Several decisions below exist
only to enforce that.

**Only the no-credential state starts BLE provisioning.** `DISCONNECTED`, `NO_MATCH`, and
`ALL_FAILED` all schedule a backoff reconnect instead (30s → ×2 → 300s ceiling). The alternative —
re-provisioning on any connection failure — would drop a device that merely lost coverage into
provisioning mode, which looks indistinguishable from a factory-reset device to the user. A retry
must always stay armed instead.

**`wifi_manager_stop()` fires no events, so whoever stops WiFi owns restarting it.** That is what
lets a deliberate stop hand the radio to `network_prov_mgr` without the app scheduling a reconnect
against itself — and it is why `dismiss_provisioning()` and the `BLE_PROV_STOPPED` handler restart
WiFi by hand. The full contract (five numbered invariants) is stated once, on the declaration in
`components/wifi_manager/include/wifi_manager.h`; that header is the source of truth, not this file.

**~~There is deliberately no periodic DNS re-probe once `WIFI_ST_CONNECTED`.~~ Superseded by
ADR-0008.** The constraint was real — `do_dns_probe()` contained an unbounded `getaddrinfo()`, and
`wifi_manager_stop()` must be able to interrupt the CONNECTED state within its stop timeout — and
enforcing it on `on_wifi_event()`'s own CONNECTED branch, which had been blocking the same task far
longer on `sntp_sync_start()`, Tailscale registration, DERP and a WireGuard handshake, was the right
fix; that work now runs on `net_worker` (`src/app_handlers.c`) and the handler publishes the link
state and returns. What did not survive is the premise underneath it: this paragraph recorded as an
accepted cost that "`do_dns_probe()` only proves DNS resolves, not that anything is reachable", and
then kept the probe anyway for a verdict whose sole consumer was an icon colour. The probe is gone
(#74); a successful NTP sync is now the only claim of internet the firmware makes. See
`docs/adr/0008-internet-proof-belongs-to-ntp.md`.

**~~`WIFI_MGR_NO_INTERNET` fires *after* `WIFI_MGR_CONNECTED`, not instead of it.~~ Superseded by
ADR-0008.** The ordering reasoning was sound and is preserved in spirit: the state machine still
enters CONNECTED on a network with no working uplink, because the association and the IP lease are
real and a LAN-only network is genuinely usable. Only the second event is gone, along with the
status-bar state it painted — the WiFi icon now reports the link alone and the NTP icon reports
whether the internet was reached.

**~~`on_wifi_event()` uses a bounded 50ms LVGL lock, not the default unbounded one.~~ Superseded by
ADR-0007.** The reasoning behind the bounded lock was right — `fire_event()` runs synchronously on
the wifi task, so an unbounded `lvgl_port_lock(0)` there could stall it invisibly to
`wifi_manager_stop()`'s timeout. The recorded tradeoff was not: *"a lock timeout skips that one
paint, but every WiFi status is repainted on the next event, so nothing is lost."* No next event
comes in the two terminal states. After `WIFI_MGR_CONNECTED` the wifi task parks in
`xEventGroupWaitBits(..., portMAX_DELAY)` until a disconnect or a stop, and a skipped paint also
skipped the status bar's cache — so a healthy connection could show "verifying" blue for hours, and
every screen change repainted the stale value. `on_wifi_event()` now publishes the link state and
takes no LVGL lock at all; see `docs/adr/0007-published-ui-state.md`.

**All BLE provisioning teardown happens in the `NETWORK_PROV_END` handler, never eagerly.**
`network_prov_mgr_stop_provisioning()` is documented as asynchronous (measured ~1.2s cleanup delay
on device); calling `network_prov_mgr_deinit()` inside that window hands protocomm's shallow copy of
the SRP salt/verifier back to the allocator and panics the device shortly after. Every stop —
success or cancel — is reported through `NETWORK_PROV_END`, and only a `BLE_PROV_SUCCESS` outcome
may call `ble_provisioning_release_memory()`, which is one-way: after it, `ble_provisioning_start()`
returns `ESP_ERR_INVALID_STATE` forever, and re-provisioning requires `esp_restart()`. This
constraint drove the whole BLE provisioning lifecycle design, not just one code path.

**The QR overlay is dismissible on purpose, and the deep-sleep inhibit predicate deliberately checks
screen visibility, not manager activity.** Nav actions are swallowed while the overlay is up so no
screen transition can delete it out from under BLE, but `BACK` hides it and returns to the clock
with BLE still advertising — a device that has never been provisioned must remain usable as a clock
indefinitely, not stuck showing a QR code forever. That's why the deep-sleep inhibit check uses
`prov_screen_is_visible()` and not the session phase: the latter would mean such a
device could never auto-sleep at all.

**The named provisioning-session phase deliberately excludes dismissal.** The session lifecycle is
now an enumerated phase plus a latched outcome (`components/ble_provisioning/src/prov_session.c`),
which replaced five loose booleans and gave the one-way 110 KB release a host-tested transition
table. `DISMISSED` was proposed as a phase of that machine and rejected: overlay visibility is
published UI intent owned by `prov_screen.c` (ADR-0007), and the two are genuinely independent —
dismissed-but-advertising and visible-and-advertising are the same session phase. Folding the
overlay into the phase would give the deep-sleep inhibit exactly one predicate to read, and it
would be the wrong one, re-creating the sleep-forever bug the paragraph above exists to prevent.
The refactor renamed and tested the machine; it did not move the predicate.

Two properties of that machine are asserted as host tests rather than left to review, because both
are irreversible in the wrong direction: no input sequence may report a successful session without
an outcome having been latched, and no input sequence may leave the terminal
memory-released phase.

**On a wrong-password retry, the BLE manager is never stopped and restarted.** Instead, the stored
NVS credential is cleared and the QR overlay is re-shown so the phone retries over the existing BLE
connection — restarting the manager would drop the phone's connection and force a fresh BLE pairing
for what should be a one-field correction.

**After `BLE_PROV_SUCCESS`, the code explicitly disconnects and waits 300ms before reconnecting.**
`network_prov_mgr` leaves WiFi already connected when provisioning succeeds, but reconnecting
without first calling `esp_wifi_disconnect()` + a short delay causes `ASSOC_LEAVE` to trigger a
15-second timeout that surfaces as `WIFI_MGR_ALL_FAILED` — an entirely avoidable failure right after
a successful provisioning flow.

**The WiFi SSID is never logged or copied with `%s`/`strlen()`.** `wifi_sta_config_t` places
`password[64]` immediately after `ssid[32]` with no guaranteed NUL terminator between them; a naive
string read runs straight into the password bytes. This previously leaked the plaintext WiFi
password to the serial log and corrupted the stored SSID in NVS badly enough to break reconnection
after reboot. The fix — a bounded `strnlen`+`memcpy` copy — is deliberate, not incidental, and
matches the same contract `network_provisioning`'s own handler code uses for the same field.
