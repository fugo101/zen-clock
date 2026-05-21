# Changelog

## [0.2.3](https://github.com/fugo101/zen-clock/compare/v0.2.2...v0.2.3) (2026-05-21)


### Bug Fixes

* **release:** revert skip-github-release now that immutable releases are disabled ([#8](https://github.com/fugo101/zen-clock/issues/8)) ([6c83c70](https://github.com/fugo101/zen-clock/commit/6c83c7030f0785b7e156e2855e7b053ba116fe4e))

## [0.2.2](https://github.com/fugo101/zen-clock/compare/v0.2.1...v0.2.2) (2026-05-21)


### Bug Fixes

* **release:** skip release-please's GitHub Release; let build-release… ([#6](https://github.com/fugo101/zen-clock/issues/6)) ([583249a](https://github.com/fugo101/zen-clock/commit/583249ac8c6b816c39ea67bd29cd2655ffcb92c7))

## [0.2.1](https://github.com/fugo101/zen-clock/compare/v0.2.0...v0.2.1) (2026-05-21)


### Bug Fixes

* **release:** create release as draft so firmware assets can be attached ([#3](https://github.com/fugo101/zen-clock/issues/3)) ([312bc2e](https://github.com/fugo101/zen-clock/commit/312bc2ea5727bf6a0e6611b5fd86f1f5ccd404ed))

## [0.2.0](https://github.com/fugo101/zen-clock/compare/v0.1.0...v0.2.0) (2026-05-21)


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
