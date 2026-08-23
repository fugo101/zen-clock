# Internet proof belongs to NTP

**The WiFi manager owns the link and makes no claim about the internet.** `WIFI_MGR_CONNECTED` now
means associated with an IP lease, nothing more. The only evidence the firmware accepts that the
internet works is a successful NTP sync, and it is reported by the icon of the component that
obtained it. This reverses the paragraph in ADR-0002 that accepted a DNS probe as a good-enough
connectivity check.

**The probe was a prediction of an answer the system already receives.** `do_dns_probe()` ran up to
five `getaddrinfo("pool.ntp.org")` attempts on entry to `VERIFYING`, and its verdict had exactly one
consumer: the colour of the WiFi icon. Nothing gated on it — SNTP and MicroLink started either way.
Roughly thirty seconds later `sntp_sync` produced the real answer, from a real attempt, and already
had its own icon for it: a red `LV_SYMBOL_REFRESH` that exists precisely because "the clock is
showing the wrong time and nothing else on screen says so".

**It also predicted wrong in the case that mattered (#74).** A DNS server answering the probe host
with a portal or null address satisfies the probe while NTP times out completely; a captive portal
defeats it the same way. So the check passed exactly when the user most needed it to fail, and the
device painted a confident green over a clock that was about to be wrong.

**And it was not free.** A failing probe blocked `WIFI_MGR_CONNECTED` — and therefore the start of
NTP and Tailscale — for up to five DNS timeouts plus four two-second waits, on precisely the
networks that needed the retry soonest. That same unbounded `getaddrinfo()` was ADR-0002's stated
reason for refusing a periodic re-probe, which put the constraint in the odd position of forbidding
the cheap repetition of a call it permitted once.

**Considered and rejected: a stronger check** (a TCP connect after resolving) — it buys accuracy by
adding a second unbounded socket operation to the one state `wifi_manager_stop()` must be able to
interrupt, in order to predict, a little better, something that gets measured for real moments
later. **Also rejected: keeping the yellow icon and driving it from `SNTP_EVENT_FAILED`** — under
this decision the yellow WiFi icon and the red NTP icon would derive from a single event, and one
fact deserves one pixel. Distinguishing a captive portal from a LAN-only network is deliberately out
of scope: this device cannot sign into a portal, so both states lead the user to the same action.

**Consequences.** Removed: `do_dns_probe()`, `WIFI_MGR_GOT_IP`, `WIFI_MGR_NO_INTERNET`,
`WIFI_STATUS_VERIFYING`, `WIFI_STATUS_NO_INTERNET`, and the `s_unverified` field in
`src/app_handlers.c`. That field is why the published link state needed a spinlock deriving one icon
from two independently-owned fields; the critical section remains, but now it guards only a store
and its publish. `WIFI_ST_VERIFYING` is renamed `WIFI_ST_LINK_UP` and keeps its structural job — it
is the single join point of the two paths into `CONNECTED` (the scan path, and the
post-provisioning shortcut straight from `IDLE`), which is what keeps `wifi_cred_save_ap_hint()` at
exactly one call site (#100). The user-visible cost, accepted: on a network with no working uplink
the WiFi icon now goes green immediately and the warning arrives with the first failed NTP attempt
instead of before `CONNECTED` — later, but true.

**One path delays it much further, and is accepted.** On a deep-sleep wake with a sync less than an
hour old, `sntp_sync` reports `SNTP_EVENT_SYNCED` without attempting a sync at all and waits out the
remainder of the interval (`components/sntp_sync/src/sntp_sync.c`). A device that wakes onto a
LAN-only or captive network therefore shows a green WiFi icon and no NTP icon, with no attempt for
up to an hour, where the removed probe would have painted yellow within seconds. It is accepted
because the skip exists precisely for the case where the time is still known to be right: the RTC
carried it across the sleep, so the icons are not hiding a wrong clock — only a network fact the
device has no current use for. It becomes visible the moment anything actually needs the network,
including the next re-sync.
