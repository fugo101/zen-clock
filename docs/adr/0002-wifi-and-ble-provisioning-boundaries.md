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

**There is deliberately no periodic DNS re-probe once `WIFI_ST_CONNECTED`.** `do_dns_probe()`
contains an unbounded `getaddrinfo()`, and `wifi_manager_stop()` must be able to interrupt the
CONNECTED state within its stop timeout — a background re-probe loop would risk blocking that. A
successful NTP sync clears the no-internet state instead, since reaching a time server is itself
proof the internet works. The accepted cost: `do_dns_probe()` only proves DNS resolves, not that
anything is reachable (a DNS server returning a null/portal IP for the probe host passes it while
NTP times out completely) — known and not fixed.

**`WIFI_MGR_NO_INTERNET` fires *after* `WIFI_MGR_CONNECTED`, not instead of it.** The association and
IP lease are real and a LAN-only network is genuinely usable, so the state machine still enters
CONNECTED; the no-internet event just paints a different status-bar color over it. Reversing the
order would silently disable the "still usable, just no internet" signal entirely.

**`on_wifi_event()` uses a bounded 50ms LVGL lock, not the default unbounded one.** `fire_event()`
runs synchronously on the wifi task, so an unbounded `lvgl_port_lock(0)` there could stall the wifi
task invisibly to `wifi_manager_stop()`'s timeout. The tradeoff: a lock timeout skips that one
paint, but every WiFi status is repainted on the next event, so nothing is lost. The same pattern is
used for the Tailscale poll timer callback, for the same reason (both run outside LVGL-timer
context).

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
`prov_screen_is_visible()` and not `ble_provisioning_is_active()`: the latter would mean such a
device could never auto-sleep at all.

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
