# Third-Party Components

ZenClock itself is MIT (see README → License). This file records every piece of third-party code that
ships in a ZenClock build, where it came from, and under what terms — so provenance does not get lost
when a vendored copy drifts from its upstream.

**Keep this file updated whenever a submodule pin moves or a dependency is added.**

---

## Vendored source (in-tree, compiled into the firmware)

### `components/wireguard_lwip/` → `vendor/microlink/components/wireguard_lwip/`

| | |
|---|---|
| **Upstream** | https://github.com/smartalock/wireguard-lwip |
| **Author** | Daniel Hope &lt;daniel.hope@smartalock.com&gt; (Floorsense Ltd / Agile Workspace Ltd) |
| **License** | BSD 3-Clause — full text at `vendor/microlink/components/wireguard_lwip/LICENSE` |
| **Reached via** | `vendor/microlink` submodule → symlinked into `components/wireguard_lwip` |
| **Upstream checked** | 2026-08-10 — up to date on substance, see below |

⚠️ **This copy has diverged substantially from upstream. Any future sync is a manual merge, not a
fast-forward.** The most visible divergence: the fork replaces upstream's single
`allowed_ip`/`allowed_mask` pair with an `allowed_source_ips[WIREGUARD_MAX_SRC_IPS]` array, plus
ESP-IDF 6.x / GCC 15 patches (documented in `vendor/microlink/ESP_IDF_6X_COMPAT.md`) and `WG_DEBUG`
logging throughout. **2026-08-14:** also carries a fix absorbed from
[CamM2325/microlink#20](https://github.com/CamM2325/microlink/pull/20) (open, unmerged upstream —
see the microlink section below): decrypted RX is handed to `device->netif->input` instead of
calling `ip_input()` directly, so the lwIP core is only ever entered from `tcpip_thread` — fixes a
pbuf double-free crash under sustained TCP load through the tunnel.

**Upstream review, 2026-08-10** — upstream has three commits after the 2022 work. All substance is
already present in our copy; nothing to port:

| Upstream commit | Subject | Status here |
|---|---|---|
| `ac84f4c` (2026-04-15) | fix: check source IP in cryptokey routing, not destination | ✅ **Already fixed** — `src/wireguardif.c` checks `iphdr->src` against `peer->allowed_source_ips[]`; the fork fixed this independently and added a drop-path debug log |
| `6e65ae6` (2026-04-16) | Rename variables | ➖ Cosmetic, not applicable to the diverged code |
| `0c44df3` (2022-11-12) | Fix replay detection drops first packet per rekey | ✅ Present — `src/wireguard.c`, `seq++` with the same rationale comment |
| `63b5865` (2022-11-12) | Calculate replay window size bits correctly | ✅ Present — `src/wireguard.c`, `sizeof(keypair->replay_bitmap) * CHAR_BIT` |

`ac84f4c` is a security fix (validating the wrong IP field lets a peer inject traffic that the
cryptokey-routing whitelist should have dropped) — worth re-verifying it stays fixed after any merge.

**To check upstream again:**

```bash
cd vendor/microlink
git remote add smartalock https://github.com/smartalock/wireguard-lwip.git   # one-time
git fetch smartalock
git log --oneline smartalock/master -- src/
```

> Also documented directly in `vendor/microlink/components/wireguard_lwip/README.md` as of
> 2026-08-12, so it survives independently of this file for anyone working from the microlink repo.

### `components/microlink/` → `vendor/microlink/components/microlink/`

| | |
|---|---|
| **Upstream** | https://github.com/CamM2325/microlink |
| **Fork used** | https://github.com/fudio101/microlink (branch `main`) |
| **Author** | Cameron Malone |
| **License** | MIT — `vendor/microlink/LICENSE` |
| **Pinned at** | `bac7d62` |

**2026-08-12:** the fork's `esp-idf-6x-compat` branch (`main` + one ESP-IDF 6.x/mbedTLS 4.x/GCC 15
compat commit) was fast-forward merged into `main` and never diverged again — `.gitmodules` now
tracks `main` directly instead of a side branch. Two more fixes landed on `main` the same day: the
Kconfig credentials comment no longer claims sdkconfig "should be git-ignored" (true for the fork's
own examples, false for this project, which tracks sdkconfig and uses `scripts/check_secrets.py`
instead), and `wireguard_lwip/README.md` gained the divergence warning + upstream-check procedure
directly in the fork. A third fix landed the same day: `ESP_IDF_6X_COMPAT.md` no longer documents
`board_build.esp-idf.sdkconfig_extra`, which is not a real PlatformIO option.

**2026-08-14:** upstream (`CamM2325/microlink`) has been inactive for ~5 months (last commit
2026-03-17) with zero maintainer engagement on any of its 6 open PRs. Absorbed three of them as
cherry-picks (author attribution preserved via `git cherry-pick -x`), since ZenClock is an
inbound-only tailnet member and these are exactly the reachability/registration-diagnostic paths
that matter for that use case:

- [#21](https://github.com/CamM2325/microlink/pull/21) (snowpaper) — a CGNAT peer (e.g. phone on
  mobile data reaching this device) got a one-way tunnel: inbound over DERP, every outbound packet
  fired at an unvalidated advertised endpoint into a black hole. Now blank at peer-add, DERP until
  a DISCO pong validates a direct path — matches Tailscale's own semantics.
- [#20](https://github.com/CamM2325/microlink/pull/20) (snowpaper) — see the `wireguard_lwip`
  section above.
- [#23](https://github.com/CamM2325/microlink/pull/23) (letalvoj) — a rejected Tailscale auth key
  used to fail silently into a misleading `MapResponse ... node not found`; now logs the actual
  `RegisterResponse` error.

Full absorption record, including what was deliberately skipped and why, lives in
`vendor/microlink/UPSTREAM_PRS.md`.

---

## Managed components (fetched by the IDF Component Manager into `managed_components/`)

Declared in `src/idf_component.yml` and `components/ble_provisioning/idf_component.yml`; exact
versions and hashes are pinned in `dependencies.lock`.

| Component | Version | Upstream | License |
|---|---|---|---|
| `lvgl/lvgl` | 9.5.0 | https://github.com/lvgl/lvgl | MIT |
| `espressif/esp_lvgl_port` | 2.9.0 | https://github.com/espressif/esp-bsp | Apache-2.0 |
| `espressif/network_provisioning` | ^1.2.4 | https://github.com/espressif/idf-extra-components | Apache-2.0 |
| `espressif/cjson` | 1.7.19~2 | https://github.com/DaveGamble/cJSON | MIT |

ESP-IDF itself (6.0.1, via PlatformIO `espressif32@7.0.1`) is Apache-2.0.

---

## Code adapted into first-party components

Credited in README → Credits; repeated here so the list is complete:

| Where | Source | Notes |
|---|---|---|
| `components/bsp/` display init | https://github.com/hiruna/esp-idf-t-display-s3 | Display driver architecture |
| `components/lcd_backlight/` | https://github.com/hiruna/esp-idf-aw9364 | Backlight driver, adapted |

Crypto primitives inside `wireguard_lwip` carry their own upstream attributions in-file
(BLAKE2S from RFC 7693, CHACHA20 from the eSTREAM reference, POLY1305 from
[poly1305-donna](https://github.com/floodyberry/poly1305-donna), X25519 from the STROBE project).
See `vendor/microlink/components/wireguard_lwip/README.md` for the full list.

---

ZenClock's own license: MIT, Copyright (c) 2026 fudio101 — see [`LICENSE`](LICENSE).
Nothing in this file changes those terms; each component above keeps its own.
