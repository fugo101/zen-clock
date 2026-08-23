# Third-Party Components

ZenClock itself is MIT (see README → License). This file records every piece of third-party code that
ships in a ZenClock build, where it came from, and under what terms — so provenance does not get lost
when a vendored copy drifts from its upstream.

**Keep this file updated whenever a submodule pin moves or a dependency is added.**

---

## Managed components (fetched by the IDF Component Manager into `managed_components/`)

Declared in `src/idf_component.yml` and `components/ble_provisioning/idf_component.yml`; exact
versions and hashes are pinned in `dependencies.lock`.

| Component | Version | Upstream | License |
|---|---|---|---|
| `lvgl/lvgl` | 9.5.0 | https://github.com/lvgl/lvgl | MIT |
| `espressif/esp_lvgl_port` | 2.9.0 | https://github.com/espressif/esp-bsp | Apache-2.0 |
| `espressif/button` | ^4.2.0 | https://github.com/espressif/esp-iot-solution | Apache-2.0 (see note) |
| `espressif/network_provisioning` | ^1.2.4 | https://github.com/espressif/idf-extra-components | Apache-2.0 |
| `espressif/cjson` | 1.7.19~2 | https://github.com/DaveGamble/cJSON | MIT |
| `fugo101/microlink` | ^3.0.0 | https://github.com/fugo101/microlink (fork of [CamM2325/microlink](https://github.com/CamM2325/microlink)) | MIT |
| `fugo101/wireguard_lwip` | ^1.0.0 (transitive, via microlink) | https://github.com/fugo101/wireguard-lwip (real fork of [smartalock/wireguard-lwip](https://github.com/smartalock/wireguard-lwip), Daniel Hope) | BSD-3-Clause |

**On `espressif/button`'s license:** the Component Registry's metadata labels version 4.2.0 as
`Custom` (4.1.7 and every earlier version are labelled `Apache-2.0`). The package's own
`license.txt` at 4.2.0 is verbatim Apache-2.0 — the label is wrong, not the terms. This was checked
against the published artifact before adopting; re-check it if the pinned version moves.

ESP-IDF itself (6.0.1, via PlatformIO `espressif32@7.0.1`) is Apache-2.0.

**On the `wireguard_lwip` fork:** until 2026-08, this dependency was vendored in-tree as a
symlinked, manually-diverged copy with no git history back to Daniel Hope's original work — this
file used to carry a hand-maintained divergence log for exactly that reason. It is now `fugo101/
wireguard-lwip`, a real GitHub fork with the changes replayed as actual commits on top of upstream
history, published on its own release cadence to the ESP Component Registry. `git log`/`git blame`
on that repo now answers the provenance question this file used to answer by hand — see its
`README.md` and `UPSTREAM_PRS.md`-equivalent history directly. Absorbed/skipped upstream PRs for
`microlink` itself (against `CamM2325/microlink`) are tracked the same way, in `fugo101/microlink`'s
own `UPSTREAM_PRS.md` — not duplicated here.

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
See [fugo101/wireguard-lwip's README](https://github.com/fugo101/wireguard-lwip) for the full list.

---

ZenClock's own license: MIT, Copyright (c) 2026 fudio101 — see [`LICENSE`](LICENSE).
Nothing in this file changes those terms; each component above keeps its own.
