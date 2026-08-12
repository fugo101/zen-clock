# SNTP Sync Component

## 1. Overview

The `sntp_sync` component provides NTP-based time synchronization for the ZenClock firmware running on the ESP32-S3.
It handles the initial sync and automatic periodic re-synchronization (default: every 1 hour) to combat
ESP32 RTC drift, using primary (VNIX) and fallback (Pool NTP, Google) servers.

**A failed sync does not wait out the hour.** The interval applies only after a success; a failure retries on a
backoff of 30 s doubling to a 5-minute ceiling, resetting to the hourly cadence once a sync lands. Without this, a
boot with no internet displayed `01/01/1970` for a full hour before trying again — and, since a successful sync is
what clears the status bar's "no internet" state, the yellow WiFi icon would have persisted just as long.

On wake from deep sleep, the component checks `RTC_DATA_ATTR` memory for the last successful sync timestamp.
If the elapsed time is within the 1-hour resync interval, the initial NTP sync is skipped and `SNTP_EVENT_SYNCED`
is reported immediately — reducing reconnect latency after short sleep cycles.

## 2. API Summary

```c
// Start SNTP sync. Call once after first WiFi connection.
esp_err_t sntp_sync_start(sntp_sync_cb_t on_sync);

// Notify SNTP that WiFi just reconnected.
// Wakes periodic-resync task immediately if last sync is stale (>1h) or never occurred.
// Safe to call multiple times; no-op if sync is still fresh.
void sntp_sync_notify_connected(void);

// Check if time has been synchronized this session.
bool sntp_sync_is_synced(void);

// Stop SNTP client and free resources.
void sntp_sync_stop(void);

// Force immediate NTP resync regardless of last sync time (manual user trigger).
void sntp_sync_force_resync(void);

// Return timestamp (time_t) of last successful NTP sync.
// Returns 0 if never synced. Persists across deep sleep via RTC_DATA_ATTR.
time_t sntp_sync_get_last_sync_time(void);
```

### Events

| Event                | Description                                                    |
|----------------------|----------------------------------------------------------------|
| `SNTP_EVENT_SYNCING` | Sync in progress (initial or periodic re-sync).                |
| `SNTP_EVENT_SYNCED`  | Time successfully synchronized (or restored from RTC on wake). |
| `SNTP_EVENT_FAILED`  | Sync attempt timed out with no response from any server.       |

## 2b. Offline Reconnect Behavior

When WiFi drops and later reconnects, `sntp_sync_notify_connected()` checks whether a resync is due:

| Condition on reconnect                 | Behavior                                                                            |
|----------------------------------------|-------------------------------------------------------------------------------------|
| Last sync < 1 hour ago                 | No-op — resync not due yet, task keeps sleeping until interval expires.             |
| Last sync ≥ 1 hour ago or never synced | Wakes the periodic-resync task immediately via event group bit (`SNTP_BIT_RESYNC`). |

The task uses `xEventGroupWaitBits` (not `vTaskDelay`) for its sleep intervals, so it responds to the wake signal
without polling.

## 2c. Deep Sleep Wake Behavior

The last successful sync time is stored in RTC memory (`RTC_DATA_ATTR`) which persists through deep sleep:

| Condition on wake                    | Behavior                                                                                                      |
|--------------------------------------|---------------------------------------------------------------------------------------------------------------|
| Last sync < 1 hour ago               | Skip initial NTP sync, report `SNTP_EVENT_SYNCED` immediately. Wait remaining interval then re-sync normally. |
| Last sync ≥ 1 hour ago or first boot | Perform full initial sync (up to 30s wait).                                                                   |

> **Note:** The ESP32 RTC oscillator keeps running during deep sleep with ~72ms/hr drift. `time(NULL)` post-wake
> is accurate enough to evaluate the elapsed time correctly.

## 3. Configuration

| Parameter                | Default            | Description                                    |
|--------------------------|--------------------|------------------------------------------------|
| `SNTP_RESYNC_INTERVAL_S` | `3600`             | Seconds between periodic re-syncs.             |
| `SNTP_SYNC_TIMEOUT_S`    | `30`               | Max seconds to wait for initial sync response. |
| Primary server           | `time.vnix.gov.vn` | Primary NTP server.                            |
| Fallback server 1        | `pool.ntp.org`     | First fallback.                                |
| Fallback server 2        | `time.google.com`  | Second fallback.                               |

## 4. Usage

```c
static void on_sntp_sync(sntp_sync_event_t event) {
    if (event == SNTP_EVENT_SYNCED) {
        ESP_LOGI(TAG, "Time synchronized");
    }
}

// On first WiFi connect (WIFI_MGR_CONNECTED):
sntp_sync_start(on_sntp_sync);

// On subsequent WiFi reconnects (WIFI_MGR_CONNECTED with SNTP already running):
sntp_sync_notify_connected();  // triggers immediate resync if stale
```

Do **not** call `sntp_sync_start()` again on reconnect — it will fail with `ESP_ERR_INVALID_STATE`. Use
`sntp_sync_notify_connected()` instead.
