# WiFi Manager

A lightweight, state-machine-based WiFi connection manager for ESP32-S3, designed for ZenClock.

## Overview

WiFi Manager handles the complete WiFi lifecycle:

1. Load stored credentials from NVS
2. Scan for matching networks
3. Connect and obtain IP
4. Report connection state via callback events

It owns the link and only the link. Whether the internet is reachable is not this component's
question and it makes no claim about it — the firmware's only proof of internet is a successful NTP
sync, reported by `sntp_sync`. See `docs/adr/0008-internet-proof-belongs-to-ntp.md`.

The component uses a persistent FreeRTOS task to manage state transitions and integrates with the rest of the system via
event callbacks.

## Architecture

### State Machine

The WiFi Manager operates with five states (no RECONNECTING state):

```
IDLE ──start()──► SCANNING ──match──► CONNECTING ──got IP──► LINK_UP ──────────► CONNECTED
  ▲                  │                     │                      │                    │
  │                  │ no match            │ fail                 │ disconnect         │ disconnect
  └──────────────────┴─────────────────────┴──────────────────────┴────────────────────┘
```

**State transitions:**

- **IDLE** → **SCANNING**: `wifi_manager_start()` called with credentials loaded
- **SCANNING** → **CONNECTING**: Stored SSID found in scan results
- **CONNECTING** → **LINK_UP**: Got IP address (DHCP)
- **LINK_UP** → **CONNECTED**: immediate, unless a stop or a disconnect landed first
- Any state → **IDLE**: Disconnect event or `wifi_manager_stop()` called
- Any state → **IDLE** with event: Failure condition (no match, connection failed, etc.)

**Key characteristics:**

- No explicit RECONNECTING state — on disconnect, WiFi Manager returns to IDLE and fires an event
- Caller (`main.c`) decides whether to retry or start BLE provisioning
- LINK_UP does no waiting: it is the single join point of the two paths into CONNECTED (the scan
  path, and the post-provisioning shortcut straight from IDLE), so the stop/disconnect re-check, the
  CONNECTED event and the one AP-hint save happen in exactly one place
- Persistent task created once in `wifi_manager_init()`, never destroyed

### Credential Storage

Credentials are stored in NVS under namespace `"wifi_cred"`:

| Key      | Value                           |
|----------|---------------------------------|
| `"ssid"` | Network SSID (max 32 bytes)     |
| `"pass"` | Network password (max 64 bytes) |

**Important:**

- **Single credential only** — no multi-credential support or hash map
- NVS must be initialized before `wifi_manager_init()` (typically via `settings_init()`)
- Empty credentials → IDLE state fires `WIFI_MGR_NO_CRED` event immediately
- Caller manages credential lifecycle via `wifi_manager_set_credential()` and `wifi_manager_clear_credential()`

### Files

```
wifi_manager/
├── include/
│   └── wifi_manager.h            ← Public API (all functions here)
├── priv_include/
│   └── wifi_priv.h               ← Internal constants, wifi_cred_load()
├── src/
│   ├── wifi_manager.c            ← State machine, persistent task
│   └── wifi_credentials.c        ← NVS credential read/write/delete
└── CMakeLists.txt
```

**File responsibilities:**

- `wifi_manager.h` — All public API functions and event enums
- `wifi_priv.h` — Internal task handle, state enum, `wifi_cred_load()` function
- `wifi_manager.c` — Event loop, state machine, WiFi driver integration, persistent task
- `wifi_credentials.c` — NVS read/write operations for SSID/pass

## Public API

### Initialization & Lifecycle

```c
/**
 * @brief Initialize WiFi Manager.
 * Creates netif, initializes WiFi driver, creates persistent task.
 * Call once at startup.
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Start WiFi connection process.
 * Loads credentials → scans → connects → verifies (blocks task, not caller).
 * Returns immediately; use callback for state updates.
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief Stop WiFi and return the task to IDLE.
 * Fires NO event — not for the stop, not for what it interrupts. The caller owns restarting
 * WiFi. ESP_ERR_TIMEOUT is a normal outcome. See wifi_manager.h for the full five-point
 * contract; it is the source of truth, not this file.
 */
esp_err_t wifi_manager_stop(void);
```

### Status Queries

```c
/**
 * @brief Get connected SSID.
 * @return Pointer to an unlocked static buffer, or NULL if not connected. Copy before use.
 */
const char *wifi_manager_get_ssid(void);

/**
 * @brief Get connected IP address as a string.
 * @return Pointer to an unlocked static buffer, or NULL if not connected. Copy before use.
 */
const char *wifi_manager_get_ip_str(void);
```

### Credential Management

```c
/**
 * @brief Check if credentials are stored in NVS.
 * @return true if SSID/pass exist in NVS namespace "wifi_cred"
 */
bool wifi_manager_has_credential(void);

/**
 * @brief Save SSID and password to NVS.
 * Called by BLE provisioning callback after user enters WiFi details.
 * @param ssid Network SSID (max 32 bytes)
 * @param pass Network password (max 64 bytes)
 * @return ESP_OK on success, or an NVS error.
 */
esp_err_t wifi_manager_set_credential(const char *ssid, const char *pass);

/**
 * @brief Clear stored credentials from NVS.
 * Called on Reset WiFi (IO14 emergency hold, routed through do_reset_wifi()) and on
 * BLE_PROV_FAILED (wrong-password retry — clears so the next provisioning attempt starts clean).
 * @return ESP_OK on success, or an NVS error.
 */
esp_err_t wifi_manager_clear_credential(void);
```

### Event Callback

```c
/**
 * @brief Set callback function for WiFi Manager events.
 * Callback fires synchronously on the wifi task when state changes.
 * The callback must not block: anything it does inline is time wifi_manager_stop() cannot
 * interrupt. It must not take the LVGL lock at all — publish the state and let the UI's
 * reconcile tick paint it (see docs/adr/0007-published-ui-state.md) — and long work belongs on
 * a worker task (see net_worker in app_handlers.c).
 * @param callback Function pointer, or NULL to disable
 */
void wifi_manager_set_callback(wifi_event_cb_t callback);

typedef void (*wifi_event_cb_t)(wifi_manager_event_t event);
```

## Events

WiFi Manager fires events via the callback when state changes:

| Event                   | When                                                    | Expected Action                                                 |
|-------------------------|---------------------------------------------------------|-----------------------------------------------------------------|
| `WIFI_MGR_SCANNING`     | Scan started                                            | Update UI: show "Scanning..."                                   |
| `WIFI_MGR_CONNECTING`   | Attempting to connect to matched SSID                   | Update UI: show "Connecting to [SSID]..."                       |
| `WIFI_MGR_CONNECTED`    | Associated with an IP lease — says nothing about internet reachability | Update UI: show WiFi icon; start anything that needs the network |
| `WIFI_MGR_DISCONNECTED` | Lost WiFi connection while connected                    | Update UI to disconnected, schedule auto-reconnect with backoff |
| `WIFI_MGR_NO_CRED`      | No credentials stored in NVS at start                   | Call `ble_provisioning_start()` to enter setup mode             |
| `WIFI_MGR_NO_MATCH`     | Stored SSID not found in scan results                   | Update UI to disconnected, schedule auto-reconnect with backoff |
| `WIFI_MGR_ALL_FAILED`   | Connection attempt failed (bad password, timeout, etc.) | Update UI to disconnected, schedule auto-reconnect with backoff |
| `WIFI_MGR_SCAN_DONE`    | Scan complete (informational)                           | (Generally not used in UI)                                      |

## Integration & Rules

### Caller Responsibilities

1. **Call in correct order:**
   ```c
   wifi_manager_init();        // Once at startup
   wifi_manager_set_callback(on_wifi_event);  // Set callback
   wifi_manager_start();       // Start connection process
   ```

2. **Handle failure events:**
    - `WIFI_MGR_NO_CRED` (no credentials): call `wifi_manager_stop()` + `ble_provisioning_start()`
    - `WIFI_MGR_DISCONNECTED`, `WIFI_MGR_NO_MATCH`, `WIFI_MGR_ALL_FAILED` (has credentials): update UI to
      disconnected and schedule a reconnect timer — device operates offline, retries with exponential backoff
      (30 s → 60 s → … → 300 s max). **Do not start BLE provisioning** for these events.

3. **Never block in the callback — publish, then return.** It runs synchronously on the wifi task,
   so everything it does inline is time `wifi_manager_stop()` cannot interrupt: `get_state()` still
   reads the old state and `s_task_parked` stays false, so a concurrent stop burns its whole
   `STOP_TIMEOUT_MS` and returns `ESP_ERR_TIMEOUT`. That applies to the LVGL lock **and** to slow
   work. Take no LVGL lock here (not even a bounded one) and post anything unbounded to a worker:
   ```c
   static void on_wifi_event(wifi_manager_event_t event) {
       switch (event) {
       case WIFI_MGR_CONNECTED:
           set_link(WIFI_STATUS_CONNECTED, true); // publish; the UI reconcile tick paints it
           net_worker_notify();                   // NTP + Tailscale run off this task
           break;
       // ...
       }
   }
   ```
   An earlier version of this file prescribed a *bounded* 50 ms LVGL lock here, on the grounds that
   a skipped paint would be repainted by the next event. That was wrong for the terminal states —
   after `WIFI_MGR_CONNECTED` there is no next event — and it left the status icon stale for the
   life of the connection. See `docs/adr/0007-published-ui-state.md` and ADR-0002.

4. **Never call `esp_wifi_*` directly** — use this API only

### WiFi Initialization Dependencies

WiFi Manager depends on:

- **NVS** (initialized by `settings_init()` before `wifi_manager_init()`)
- **network interface** (created in `wifi_manager_init()`)
- **event loop** (from IDF, used internally for WiFi events)

### Network Provisioning Integration

BLE provisioning is only started when the device has **no credentials** (`WIFI_MGR_NO_CRED`):

1. Caller receives `WIFI_MGR_NO_CRED` event
2. Caller calls `wifi_manager_stop()` + `ble_provisioning_start()`
3. BLE provisioning receives credentials, calls `wifi_manager_set_credential()`
4. On `BLE_PROV_SUCCESS`, caller calls `wifi_manager_start()` to connect with new credentials

For all other failures (`DISCONNECTED`, `NO_MATCH`, `ALL_FAILED`) when credentials **exist**, the caller
schedules an auto-reconnect via `wifi_manager_start()` with exponential backoff. BLE is not involved.

**Critical:** Stop WiFi before starting BLE provisioning to avoid driver conflicts.

### Internet reachability is not checked here

The component reports a link, not connectivity. `WIFI_MGR_CONNECTED` means associated with an IP
lease — on a LAN-only network or behind a captive portal it fires exactly the same, because the
association and the lease are real and the device is still a usable clock.

There used to be a DNS probe here that ran before `CONNECTED` and reported "verified online". It
only ever proved a name resolved (a DNS server answering with a portal address passed it while
nothing was reachable, #74), its verdict fed nothing but an icon colour, and a failing probe delayed
`CONNECTED` — and so NTP and Tailscale — by seconds on the networks that needed them soonest. The
proof now comes from the component that actually needs the internet: a successful NTP sync. See
`docs/adr/0008-internet-proof-belongs-to-ntp.md`.

## Configuration

WiFi Manager uses hardcoded timeouts (see `wifi_priv.h`):

| Timeout            | Value      | Purpose                |
|--------------------|------------|------------------------|
| Scan timeout       | 10 seconds | WiFi scan duration     |
| Connection timeout | 15 seconds | Time to get IP address |

To adjust, edit `wifi_priv.h` and rebuild.

## Constraints & Anti-Patterns

**DO:**

- Call `wifi_manager_init()` once at startup
- Set callback before calling `wifi_manager_start()`
- Lock LVGL in callback before updating UI
- Call `wifi_manager_stop()` before `ble_provisioning_start()`
- Handle all failure events (NO_CRED, NO_MATCH, ALL_FAILED, DISCONNECTED)

**DO NOT:**

- Include `wifi_priv.h` outside this component
- Call `esp_wifi_*` functions directly
- Update UI without `lvgl_port_lock()` in callback
- Call blocking operations in callback (blocks task)
- Assume WiFi stays connected indefinitely (listen for DISCONNECTED event)

## Testing

WiFi Manager is designed for real hardware (ESP32-S3) and WiFi networks. Test scenarios:

1. **First boot, no credentials** → IDLE fires NO_CRED → BLE provisioning
2. **Credentials set, network found** → SCANNING → CONNECTING → LINK_UP → CONNECTED
3. **Credentials set, network not found** → SCANNING → NO_MATCH → caller schedules reconnect (no BLE)
4. **Connection fails** → CONNECTING fails → ALL_FAILED → caller schedules reconnect (no BLE)
5. **WiFi drops while in use** → CONNECTED → DISCONNECTED → caller schedules reconnect (no BLE)
6. **Reconnect succeeds** → `wifi_manager_start()` fires again → SCANNING → CONNECTED

## Related Components

- **ble_provisioning** — Sets credentials via `wifi_manager_set_credential()`
- **sntp_sync** — Runs after WiFi CONNECTED event to sync time
- **ui** — Updates status bar in WiFi event callback
- **settings** — Initializes NVS before `wifi_manager_init()`
