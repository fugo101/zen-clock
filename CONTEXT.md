# ZenClock

Firmware for a WiFi-connected desk clock (LilyGo T-Display-S3) that stays usable offline, provisions over BLE, and optionally tunnels home over Tailscale.

## Language

**Nav action**:
One of the four abstract inputs (`UP`, `DOWN`, `SELECT`, `BACK`) the navigation state machine consumes, decoupled from which physical button produced it.
_Avoid_: Button press, key event

**Screen**:
One full-screen LVGL view the navigation state machine can show — Clock, Menu, Settings, System Info, or the provisioning QR overlay.
_Avoid_: Page, view, activity

**Edit mode**:
The in-place state a Settings row enters on `SELECT`, where `UP`/`DOWN` change the row's value instead of moving the cursor. The effect the user can see is applied on every press; the NVS write is debounced behind a 1 s timer and flushed on exit, so holding a button costs one flash write rather than one per press.
_Avoid_: Edit screen, focus mode

**Setting descriptor**:
The purely domain-level record of one persisted setting — its storage key, default value, valid range, and, for a boolean, whether the displayed option order runs opposite to the stored value. One descriptor serves both reading and writing, so a value outside the range cannot exist in either direction. It deliberately knows nothing about the setting's display label, its edit step, or what changing it does to the hardware.
_Avoid_: Setting, config entry, settings row

**Deep sleep**:
The device's low-power state, entered by inactivity timeout or the two-button combo, that cuts the LCD power rail and wakes only on a wake-pin interrupt.
_Avoid_: Standby, hibernate

**Wake-pin hold latch**:
The persisted-through-reboot GPIO hold applied to the LCD power rail pin so it stays off across a deep-sleep cycle, released explicitly at the next boot.
_Avoid_: GPIO lock, pin state

**Battery view**:
The complete derived state for one battery reading — icon symbol, tint, blink, label text, and the low flag — computed once by a single pure mapping and consumed by every reader, so the status bar's appearance and the low-battery clamp can never disagree about what a reading means.
_Avoid_: Battery state, battery status, battery level

**Low-battery clamp**:
The edge-triggered brightness reduction applied the moment the battery view crosses from not-low to low, restored on recovery, never persisted to NVS and never applied on USB power.
_Avoid_: Low-power mode, brightness limit

**WiFi manager**:
The state machine (`IDLE → SCANNING → CONNECTING → LINK_UP → CONNECTED`) that owns the device's single stored WiFi credential and all connection attempts. It owns the link and only the link — it makes no claim about whether the internet is reachable.
_Avoid_: Network manager, connection handler

**Provisioning**:
The BLE-based flow (via `network_provisioning` / Espressif's BLE Prov app) that lets a phone hand the device a WiFi credential over Security 2 (SRP6a), shown to the user as a QR overlay.
_Avoid_: Setup, pairing, onboarding. Never use the bare word for whether the QR overlay is on screen — the overlay can be dismissed while the session runs on, and conflating the two is what made the device's power policy read a widget pointer.

**Provisioning session**:
One run of the provisioning lifecycle, from the moment the BLE service is asked to start until the service confirms it has stopped. It is not the QR overlay: the user can dismiss the overlay and the session keeps advertising, which is what lets a never-provisioned device stay usable as a clock. The session has exactly one phase at a time, and one of those phases is terminal — once the BLE controller's memory has been released the device cannot host another session until it reboots.
_Avoid_: Provisioning mode, provisioning state, BLE session

**Session outcome**:
Whether a provisioning session ended because a credential was verified or because it was cancelled. Latched the moment verification succeeds and independent of how the session then stops, so a cancel racing a successful verification still reports the success. Only the verified outcome permits the one-way release of the BLE controller's memory, which makes this the single most consequential bit in the firmware.
_Avoid_: Success flag, provisioning result

**No-credential state**:
The specific WiFi manager outcome (`WIFI_MGR_NO_CRED`) that is the only trigger for starting provisioning — every other failure mode instead schedules a backoff reconnect, because losing coverage must never look like an unprovisioned device.
_Avoid_: Connection failure, disconnected

**AP hint**:
The remembered identity — BSSID and channel — of the access point the device last actually reached, used to skip the all-channel scan on the next connect in favour of a single targeted one. It records where the device *got to*, not what it was told to look for, so it is written on reaching the connected state and discarded the moment a targeted scan fails to find it there. Being a fact about the last success rather than an event, re-asserting an unchanged one changes nothing.
_Avoid_: Cached BSSID, last known AP, fast-scan config

**Internet proof**:
A successful NTP sync — the only evidence the firmware accepts that the internet works, and the reason it is accepted is that it *is* the thing that needed the internet. Association, an IP lease and a resolvable hostname each look like proof and are not: a DNS server answering with a portal address satisfies all three while nothing is reachable. The device therefore never predicts reachability at connect time; it reports the outcome, and the failure to obtain it is what the user sees.
_Avoid_: Online, connectivity check, internet status, verified connection

**VPN rebind**:
The lightweight MicroLink/Tailscale reconnect path used after a WiFi reconnect, which reopens sockets while preserving the existing VPN session and WireGuard keys, as opposed to the full `microlink_init()`/`microlink_start()` registration done only on first connect.
_Avoid_: Reconnect, re-init

**Published state**:
Desired UI state written by whichever task learns it, with no lock, and read by the task that owns
what it describes. The writer never touches the thing itself — it says what should be true and
returns. Every status icon, the provisioning overlay's visibility, and the Tailscale handle are
published this way.
_Avoid_: Set, update, notify

**Reconcile**:
The owning task's periodic step that brings what is on screen into line with published state.
Idempotent by construction and driven by comparison, not by a change flag, so a missed or torn
round costs latency and never correctness.
_Avoid_: Refresh, redraw, sync

**Foreign task**:
Any task that is not the owner of a piece of state. Every event callback in the firmware is foreign
to the UI: the WiFi task, the NTP task, the BLE event loop, the button task and the shared timer
task all learn things the screen should show, and none of them may block on the UI to say so.
_Avoid_: Background task, caller, other thread

**Retry backoff**:
The single shared retry-pacing policy (`components/backoff/`) — 30 s, doubling, capped at 5 minutes, reset on success — that both the WiFi reconnect timer and the NTP re-sync loop arm from. A step is only consumed when a retry was actually armed, so a burst of failures can never inflate the delay while leaving nothing pending.
_Avoid_: Retry timer, reconnect delay, exponential backoff (as if each caller had its own)

**Status bar**:
The persistent LVGL bar shown on every screen carrying the Tailscale, NTP, WiFi, and battery indicators — the single place battery/connection state is displayed, so no other screen duplicates it.
_Avoid_: Header, top bar

## Avoid globally

- **Traps** — DECISIONS.md's old term for "a decision that looks wrong until you know why." Superseded by ADRs in `docs/adr/`; don't reintroduce the word as a section heading.
