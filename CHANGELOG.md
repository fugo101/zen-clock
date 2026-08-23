# Changelog

## [0.6.0](https://github.com/fugo101/zen-clock/compare/v0.5.0...v0.6.0) (2026-08-23)


### Features

* **ble:** measure heap across BLE provisioning teardown ([#106](https://github.com/fugo101/zen-clock/issues/106)) ([b9e1aa2](https://github.com/fugo101/zen-clock/commit/b9e1aa2ff71e7472d1b14bfbcb6d28bc635bedd9))


### Bug Fixes

* **ble:** reset the provisioning state machine on a failed credential ([#108](https://github.com/fugo101/zen-clock/issues/108)) ([faf9e93](https://github.com/fugo101/zen-clock/commit/faf9e931e272a34e3c0c127427b9240e9ff265fb))
* **wifi:** cancel a pending reconnect before handing the radio to BLE ([#110](https://github.com/fugo101/zen-clock/issues/110)) ([c93bd45](https://github.com/fugo101/zen-clock/commit/c93bd45fc7cfaba185cf1e8410c29e41226d0eab))
* **wifi:** report the link only, let NTP prove the internet ([#103](https://github.com/fugo101/zen-clock/issues/103)) ([ccf60b7](https://github.com/fugo101/zen-clock/commit/ccf60b7978f831a1723f6d5fbc37ff0b4eece6f1))
* **wifi:** save the AP hint on the post-provisioning path ([#101](https://github.com/fugo101/zen-clock/issues/101)) ([7a1fabf](https://github.com/fugo101/zen-clock/commit/7a1fabfa3cfb9ffba7a39c39e295ca04ffb12941))


### Refactoring

* **bsp:** replace hand-written button timing with espressif/button ([#105](https://github.com/fugo101/zen-clock/issues/105)) ([d0f6070](https://github.com/fugo101/zen-clock/commit/d0f6070708cc2dde2e5d861fb58a635f2f9bc755))

## [0.5.0](https://github.com/fugo101/zen-clock/compare/v0.4.2...v0.5.0) (2026-08-23)


### ⚠ BREAKING CHANGES

* **settings:** collapse NVS access behind one descriptor table ([#91](https://github.com/fugo101/zen-clock/issues/91))

### Bug Fixes

* **settings:** clamp timezone offset on read, write and apply ([#90](https://github.com/fugo101/zen-clock/issues/90)) ([bf654f0](https://github.com/fugo101/zen-clock/commit/bf654f008a4a7f879e537cd3d092f6fa38cd87e2))
* **ui:** publish the WiFi status icon instead of painting it under a lock ([#93](https://github.com/fugo101/zen-clock/issues/93)) ([ca136fd](https://github.com/fugo101/zen-clock/commit/ca136fdfaece667a9adfadf22c860fded6238ab4))


### Refactoring

* **backoff:** extract the shared retry policy into one module ([#85](https://github.com/fugo101/zen-clock/issues/85)) ([c6d8516](https://github.com/fugo101/zen-clock/commit/c6d85166401138907747fe80ab1c6418d740990d))
* **ble:** name the provisioning session, replace five flags with a tested machine ([#99](https://github.com/fugo101/zen-clock/issues/99)) ([f91f5d2](https://github.com/fugo101/zen-clock/commit/f91f5d2148fd73aa9c97704dcba68b14743fce8f))
* **bsp:** put button input policy behind a pure decision seam ([#88](https://github.com/fugo101/zen-clock/issues/88)) ([82fc249](https://github.com/fugo101/zen-clock/commit/82fc24963d711b3888575c01053e6382ad98c3cd))
* derive the battery icon and brightness clamp from one view ([#89](https://github.com/fugo101/zen-clock/issues/89)) ([1b96ecb](https://github.com/fugo101/zen-clock/commit/1b96ecb132d9ea43fe25e5dff6edeadedad1e6a5))
* **settings:** collapse NVS access behind one descriptor table ([#91](https://github.com/fugo101/zen-clock/issues/91)) ([ed873c9](https://github.com/fugo101/zen-clock/commit/ed873c9c55cc13107ea1eb4de87286b50fc44e87))
* **ui:** drive the settings screen from the descriptor table ([#92](https://github.com/fugo101/zen-clock/issues/92)) ([93d75ba](https://github.com/fugo101/zen-clock/commit/93d75ba53569c18b76c630a266ab02f579c906f7))
* **ui:** publish the provisioning overlay and the MicroLink handle ([#95](https://github.com/fugo101/zen-clock/issues/95)) ([1a89d64](https://github.com/fugo101/zen-clock/commit/1a89d64a1049fe7555f00a11487406e256a0ded2))
* **ui:** publish the SNTP and Tailscale status icons ([#94](https://github.com/fugo101/zen-clock/issues/94)) ([7981583](https://github.com/fugo101/zen-clock/commit/7981583f0d4a6205f0a825adfacaa0f7f7ca8b99))
* **wifi:** move connect-time work off the WiFi task ([#96](https://github.com/fugo101/zen-clock/issues/96)) ([bbece22](https://github.com/fugo101/zen-clock/commit/bbece22539d7ff6bc4ecc44564e3a61bd9ec5a4a))
* **wifi:** state wifi_manager_stop()'s contract in its public header ([#87](https://github.com/fugo101/zen-clock/issues/87)) ([3dd3b47](https://github.com/fugo101/zen-clock/commit/3dd3b47d232f95c16013f9b90fdb45b5fb8da9eb))

## [0.4.2](https://github.com/fugo101/zen-clock/compare/v0.4.1...v0.4.2) (2026-08-21)


### Bug Fixes

* **bsp:** replace hand-rolled battery curve with adc_battery_estimation ([#60](https://github.com/fugo101/zen-clock/issues/60)) ([943c774](https://github.com/fugo101/zen-clock/commit/943c774f8c8177dd1b404a8ddac5154570a0e643))


### Documentation

* replace DECISIONS.md with ADRs and CONTEXT.md glossary ([#76](https://github.com/fugo101/zen-clock/issues/76)) ([d0ab8bf](https://github.com/fugo101/zen-clock/commit/d0ab8bf616afd63385d84265b4c229e108db4c41))

## [0.4.1](https://github.com/fugo101/zen-clock/compare/v0.4.0...v0.4.1) (2026-08-20)


### Documentation

* configure agent skills for GitHub issue tracker and domain docs ([#56](https://github.com/fugo101/zen-clock/issues/56)) ([8350cd7](https://github.com/fugo101/zen-clock/commit/8350cd74bc8edc1c092dc7eb93e728a5afd404e1))

## [0.4.0](https://github.com/fugo101/zen-clock/compare/v0.3.0...v0.4.0) (2026-08-18)


### ⚠ BREAKING CHANGES

* migrate microlink + wireguard_lwip to the ESP Component Registry

### Features

* migrate microlink + wireguard_lwip to the ESP Component Registry ([a56a8e9](https://github.com/fugo101/zen-clock/commit/a56a8e912b8a92542e61c3dca4c28177127ec412))


### Bug Fixes

* **microlink:** absorb upstream reachability, RX-path and registration-log fixes ([#39](https://github.com/fugo101/zen-clock/issues/39)) ([82bb587](https://github.com/fugo101/zen-clock/commit/82bb58780ab065164a66300019e9bd6244c244be))


### Documentation

* add the microlink registry migration plan ([73fdd9d](https://github.com/fugo101/zen-clock/commit/73fdd9d1a5d1e001ac922536ab1c856749202ba0))
* compact Phase 2 in the microlink registry migration plan ([4fe886a](https://github.com/fugo101/zen-clock/commit/4fe886a2c57eb0d8a5dfd3aa4159885bdd3c0fc2))
* point the migration plan at its GitHub issues ([#51](https://github.com/fugo101/zen-clock/issues/51)) ([87b26f0](https://github.com/fugo101/zen-clock/commit/87b26f091f97dce5ce2135bdf20e4ea42550db79))
* record Phase 3 findings in the microlink registry migration plan ([#43](https://github.com/fugo101/zen-clock/issues/43)) ([4ec7bee](https://github.com/fugo101/zen-clock/commit/4ec7beea06bb4ade56d14472a99ac98b22def603))
* remove the microlink registry migration plan ([#53](https://github.com/fugo101/zen-clock/issues/53)) ([f534524](https://github.com/fugo101/zen-clock/commit/f53452457348bfe4d8d2dcf4607887d04a6d59aa))

## [0.3.0](https://github.com/fugo101/zen-clock/compare/v0.2.3...v0.3.0) (2026-08-13)


### Features

* **deep-sleep:** make sleep cancellable and cut the LCD rail ([#20](https://github.com/fugo101/zen-clock/issues/20)) ([4158b2a](https://github.com/fugo101/zen-clock/commit/4158b2a149dd3fe0e127d8109a51a6ea1314b70f))
* **power:** warn on low battery, move heavy nav actions off the button task ([#22](https://github.com/fugo101/zen-clock/issues/22)) ([a395c24](https://github.com/fugo101/zen-clock/commit/a395c24d6023cd14905bfd26ef53ee26174df366))


### Bug Fixes

* **critical:** repair WiFi stop/start wedge, provisioning UAF and BLE teardown ([#15](https://github.com/fugo101/zen-clock/issues/15)) ([51c0972](https://github.com/fugo101/zen-clock/commit/51c0972846c83fd01f9a50950323ba3f8ab9da81))
* **deps:** upgrade esp_lvgl_port to 2.9.0, pin platform, bump checkout ([#11](https://github.com/fugo101/zen-clock/issues/11)) ([c504b9d](https://github.com/fugo101/zen-clock/commit/c504b9df9e0aaf99d00e966cb4f57fa6aeadb291))
* **provisioning:** restore WiFi when the QR overlay is dismissed ([#19](https://github.com/fugo101/zen-clock/issues/19)) ([2514b0e](https://github.com/fugo101/zen-clock/commit/2514b0eda411a247ac5e5c7c218f7df72edec1b4))
* **robustness:** coalesce NVS writes, stop aborting on recoverable errors ([#18](https://github.com/fugo101/zen-clock/issues/18)) ([a358b88](https://github.com/fugo101/zen-clock/commit/a358b8815e0144628eebc72ddf3edb5ba61f583e))
* **security:** stop plaintext password leak via unterminated SSID cast ([#29](https://github.com/fugo101/zen-clock/issues/29)) ([b1d9575](https://github.com/fugo101/zen-clock/commit/b1d95753d9f84472bef688766c62cc97e7bb3d4e))
* **ui:** read firmware version from app descriptor instead of hardcoded literal ([#13](https://github.com/fugo101/zen-clock/issues/13)) ([d51068a](https://github.com/fugo101/zen-clock/commit/d51068a417db30a44d115c3eba6b3b8c3a7f0feb))
* **ui:** run nav action callbacks outside the LVGL lock ([#16](https://github.com/fugo101/zen-clock/issues/16)) ([a89a066](https://github.com/fugo101/zen-clock/commit/a89a066f871d4974a7492cba329a708ea6547f4f))
* **ui:** show when the clock is wrong, and stop waiting an hour to fix it ([#21](https://github.com/fugo101/zen-clock/issues/21)) ([e48301e](https://github.com/fugo101/zen-clock/commit/e48301e4c1dd0c988e02a8b3ca2464314e89acc9))
* **wifi:** bound the LVGL lock inside on_wifi_event ([#24](https://github.com/fugo101/zen-clock/issues/24)) ([8ac47d3](https://github.com/fugo101/zen-clock/commit/8ac47d3e3406e8a857b3697a9443892f68b6ff8e))
* **wifi:** keep the clock alive offline, close a race PR [#15](https://github.com/fugo101/zen-clock/issues/15) introduced ([#17](https://github.com/fugo101/zen-clock/issues/17)) ([1762f68](https://github.com/fugo101/zen-clock/commit/1762f68d62fd45a98a6a41aa55e8b7d6174543ef))

## [0.2.3](https://github.com/fugo101/zen-clock/compare/v0.2.2...v0.2.3) (2026-05-21)


### Bug Fixes

* **release:** revert skip-github-release now that immutable releases are disabled ([#8](https://github.com/fugo101/zen-clock/issues/8)) ([6c83c70](https://github.com/fugo101/zen-clock/commit/6c83c7030f0785b7e156e2855e7b053ba116fe4e))

## [0.2.2](https://github.com/fugo101/zen-clock/compare/v0.2.1...v0.2.2) (2026-05-21)


### Bug Fixes

* **release:** skip release-please's GitHub Release; let build-release… ([#6](https://github.com/fugo101/zen-clock/issues/6)) ([583249a](https://github.com/fugo101/zen-clock/commit/583249ac8c6b816c39ea67bd29cd2655ffcb92c7))

## [0.2.1](https://github.com/fugo101/zen-clock/compare/v0.2.0...v0.2.1) (2026-05-21)


### Bug Fixes

* **release:** create release as draft so firmware assets can be attached ([#3](https://github.com/fugo101/zen-clock/issues/3)) ([312bc2e](https://github.com/fugo101/zen-clock/commit/312bc2ea5727bf6a0e6611b5fd86f1f5ccd404ed))

## [0.2.0](https://github.com/fugo101/zen-clock/commits/v0.2.0) (2026-05-21)


### Features

* add deep sleep with auto-sleep timer, scroll fix, and RTC pull-up wakeup ([9785ee4](https://github.com/fugo101/zen-clock/commit/9785ee4bfe50e592b01632d3e3716ac7a2318eb0))
* implement BLE provisioning and introduce app-level event handling system ([a9181d1](https://github.com/fugo101/zen-clock/commit/a9181d1b72cadb03ee92622219aadb576994cb55))
* implement initial Board Support Package with display, battery, and input drivers ([033f806](https://github.com/fugo101/zen-clock/commit/033f8060593dea7aabcb6a63d219ffc81bd451ed))
* implement modular LVGL UI with status bar and clock face components ([e9ebcf3](https://github.com/fugo101/zen-clock/commit/e9ebcf333cab23717abce9ecce0d1a21f9ba833f))
* implement settings manager and UI modularization with persistent NVS storage and theme switching support ([493f2dc](https://github.com/fugo101/zen-clock/commit/493f2dc0fa532e4ad71f79df188c6d32367ddc3b))
* implement UI navigation system and update button event handling for menu interactions ([de275ba](https://github.com/fugo101/zen-clock/commit/de275ba46f81cd76e47a8a0a23ac42f272a4493b))
* initialize project structure with WiFi management, SNTP synchronization, and UI components ([1556d58](https://github.com/fugo101/zen-clock/commit/1556d58807f948065c345a0e96be49001d504c6d))
* initialize ZenClock project structure with ESP-IDF and basic LCD graphics drivers ([34b66c9](https://github.com/fugo101/zen-clock/commit/34b66c9c646b88075521634e3f1e673b1e16a5a8))
* initialize ZenClock project structure with LCD driver and backlight components ([3f21dc4](https://github.com/fugo101/zen-clock/commit/3f21dc4a6a73ea1f25ad0f5adc927a8c5b20b28a))
* initialize ZenClock project with SquareLine UI, T-Display S3 board support, and LCD backlight control ([b217ea9](https://github.com/fugo101/zen-clock/commit/b217ea980ccf13e73f40acdbf4a9f460ad9a09e2))
* **settings:** expand settings with grouped layout, clock options, and NTP resync ([6f6f453](https://github.com/fugo101/zen-clock/commit/6f6f45392c2c6cc2c577872524d67844c4a84b88))
* **tailscale:** integrate MicroLink Tailscale client via vendor submodule ([7fbc4f2](https://github.com/fugo101/zen-clock/commit/7fbc4f27812055cd8930b1549ddb223f8bfef392))
* **ui:** add System Info screen with scrollable device info ([5a93c0f](https://github.com/fugo101/zen-clock/commit/5a93c0f65489821b182a1826da04f8453d7f1767))
* **ui:** add Tailscale status to status bar and System Info screen ([db323ee](https://github.com/fugo101/zen-clock/commit/db323ee8476aacbd8ececb9997f17d8fc0e16812))
* **ui:** replace Montserrat with DS-Digital font and add theme-aware colors ([964c477](https://github.com/fugo101/zen-clock/commit/964c477b8914f3afff09f139895a3bf122f8bd4b))
* upgrade BLE provisioning to Security 2 (SRP6a) ([94b3321](https://github.com/fugo101/zen-clock/commit/94b3321329b483e3c5c1f20a0266350be5cc083b))
* **wifi:** offline mode, auto-reconnect, and SNTP resync on reconnect ([80abfa8](https://github.com/fugo101/zen-clock/commit/80abfa822aee616032d47ede018f6e340765c6d5))


### Bug Fixes

* **bsp:** raise USB detection threshold to 4600mV ([46b3946](https://github.com/fugo101/zen-clock/commit/46b3946877f3985ed98fb0c7a095ae3b7ea00c7c))
* **ui:** add pixel orbital shift to prevent LCD image retention ([d65a28f](https://github.com/fugo101/zen-clock/commit/d65a28f7eec6ff07eecb4e24dbaf03255e97bc69))


### Performance

* add BSSID+channel cache for fast WiFi scan on subsequent boots ([979bbbb](https://github.com/fugo101/zen-clock/commit/979bbbb538ff18165632852737d3eeb07148b29e))
