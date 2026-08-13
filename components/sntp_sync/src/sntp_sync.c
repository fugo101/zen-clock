// SPDX-License-Identifier: MIT
// ZenClock — SNTP time synchronization implementation
//
// menuconfig required:
//   Component config → LWIP → SNTP:
//     - Maximum number of NTP servers = 3
//     (optional) Request NTP servers from DHCP = Yes

#include "sntp_sync.h"

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <time.h>
#include <sys/time.h>

static const char *const tag = "SNTP";

#define SNTP_RESYNC_INTERVAL_S 3600 // Re-sync every 1 hour (ESP32 RTC drift ~72ms/hr)

// A failed sync must not wait out the full hour. The clock is showing the wrong time — often
// 01/01/1970 on a first boot with no internet — and an hour is a long time to display that before
// even trying again. Same shape as the WiFi reconnect backoff in app_handlers.c so the two behave
// alike: start at 30 s, double, cap at 5 minutes, and reset once a sync lands.
#define SNTP_RETRY_START_S 30
#define SNTP_RETRY_MAX_S   300

static bool s_synced = false;
static sntp_sync_cb_t s_on_sync = NULL;
static TaskHandle_t s_sntp_task = NULL;
static EventGroupHandle_t s_eg = NULL;
#define SNTP_BIT_RESYNC ((EventBits_t) (1 << 0))

// Persists through deep sleep; 0 on first power-on
RTC_DATA_ATTR static time_t s_last_sync_rtc = 0;

// ============================================================
// Notification callback — fired by SNTP internally on time set
// ============================================================
static void time_sync_notification_cb(struct timeval *tv) // NOLINT(readability-non-const-parameter)
{
  ESP_LOGI(tag, ">>> NTP time received! tv_sec=%lld", (long long) tv->tv_sec);
}

// ============================================================
// Log current synchronized time
// ============================================================
static void log_synced_time(void)
{
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  char buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  ESP_LOGI(tag, "Time synchronized: %s", buf);
}

// ============================================================
// Wait for SNTP sync with timeout (used for both initial and re-sync)
// Returns true if synced, false if timed out.
// ============================================================
static bool wait_for_sync(int max_retries)
{
  for (int retry = 0; retry < max_retries; retry++)
  {
    esp_err_t ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
    if (ret == ESP_OK)
    {
      return true;
    }
    ESP_LOGI(tag, "Sync poll %d/%d (ret=%s)", retry + 1, max_retries, esp_err_to_name(ret));
  }
  return false;
}

// ============================================================
// SNTP task — initial sync + periodic re-sync
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void sntp_task(void *arg) // NOLINT(readability-non-const-parameter)
{
  (void) arg;
  bool skip_initial = false;

  // --- Skip initial sync if waking from deep sleep within the resync interval ---
  if (s_last_sync_rtc != 0)
  {
    const time_t now = time(NULL);
    const time_t elapsed = now - s_last_sync_rtc;
    if (elapsed >= 0 && elapsed < (time_t) SNTP_RESYNC_INTERVAL_S)
    {
      ESP_LOGI(tag, "Deep sleep wake: last sync %lds ago, skip initial sync", (long) elapsed);
      s_synced = true;
      if (s_on_sync)
      {
        s_on_sync(SNTP_EVENT_SYNCED);
      }
      // Wait remainder of current interval, then fall into re-sync loop
      const int64_t remain_s = (int64_t) SNTP_RESYNC_INTERVAL_S - (int64_t) elapsed;
      if (remain_s > 0)
      {
        xEventGroupWaitBits(s_eg, SNTP_BIT_RESYNC, pdTRUE, pdFALSE, pdMS_TO_TICKS((uint32_t) (remain_s * 1000)));
      }
      skip_initial = true;
    }
  }

  if (!skip_initial)
  {
    // --- Initial sync ---
    if (s_on_sync)
    {
      s_on_sync(SNTP_EVENT_SYNCING);
    }
    ESP_LOGI(tag, "Waiting for NTP time sync...");

    bool ok = wait_for_sync(15); // 15 × 2s = 30s max wait
    if (ok)
    {
      s_synced = true;
      s_last_sync_rtc = time(NULL);
      log_synced_time();
    }
    else
    {
      ESP_LOGW(tag, "NTP sync timeout after 30 seconds");
    }

    if (s_on_sync)
    {
      s_on_sync(ok ? SNTP_EVENT_SYNCED : SNTP_EVENT_FAILED);
    }
  }

  // How long to wait before the next attempt. Full interval after a success, short backoff after
  // a failure — including the initial sync above, so a boot with no internet retries in 30 s.
  uint32_t wait_s = s_synced ? SNTP_RESYNC_INTERVAL_S : SNTP_RETRY_START_S;

  // --- Periodic re-sync loop ---
  // On first iteration after deep sleep wake, skip_initial=true means we already
  // waited out the remaining interval above, so go straight to re-sync.
  while (1) // NOLINT
  {
    if (!skip_initial)
    {
      // Still the same wait primitive, so sntp_sync_notify_connected() can cut it short on a
      // WiFi reconnect exactly as before — only the timeout differs.
      xEventGroupWaitBits(s_eg, SNTP_BIT_RESYNC, pdTRUE, pdFALSE, pdMS_TO_TICKS(wait_s * 1000));
    }
    skip_initial = false;

    if (s_on_sync)
    {
      s_on_sync(SNTP_EVENT_SYNCING);
    }

    ESP_LOGI(tag, "Re-syncing NTP...");
    esp_sntp_restart();

    bool ok = wait_for_sync(5); // 5 × 2s = 10s max for re-sync
    if (ok)
    {
      s_synced = true;
      s_last_sync_rtc = time(NULL);
      log_synced_time();
      wait_s = SNTP_RESYNC_INTERVAL_S;
    }
    else
    {
      s_synced = false;
      wait_s = (wait_s >= SNTP_RESYNC_INTERVAL_S) ? SNTP_RETRY_START_S : wait_s * 2;
      if (wait_s > SNTP_RETRY_MAX_S)
      {
        wait_s = SNTP_RETRY_MAX_S;
      }
      ESP_LOGW(tag, "NTP re-sync timeout — retrying in %lus", (unsigned long) wait_s);
    }

    if (s_on_sync)
    {
      s_on_sync(ok ? SNTP_EVENT_SYNCED : SNTP_EVENT_FAILED);
    }
  }
}

// ============================================================
// Public API
// ============================================================

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
esp_err_t sntp_sync_start(const sntp_sync_cb_t on_sync)
{
  s_on_sync = on_sync;
  s_synced = false;

  // Small delay to let DNS from DHCP propagate
  vTaskDelay(pdMS_TO_TICKS(500));

  // Use the modern esp_netif_sntp API (ESP-IDF 5.1+ / 6.0)
  // Old esp_sntp_* API is NOT thread-safe per official docs.
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_PRIMARY_SERVER);
  config.smooth_sync = false; // Immediate time set (not gradual adjust)
  config.sync_cb = time_sync_notification_cb;
  // Accept NTP from DHCP only if enabled in menuconfig
  // Path: Component config → LWIP → SNTP → Request NTP servers from DHCP
#if CONFIG_LWIP_DHCP_GET_NTP_SRV
  config.server_from_dhcp = true;
#endif

  esp_err_t ret = esp_netif_sntp_init(&config);
  if (ret != ESP_OK)
  {
    ESP_LOGE(tag, "esp_netif_sntp_init failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // Log configured servers
  // Note: CONFIG_LWIP_SNTP_MAX_SERVERS must be >= 3 in menuconfig
  // for fallback servers to take effect.
  // Path: Component config → LWIP → SNTP → Maximum number of NTP servers
  ESP_LOGI(tag, "SNTP server [0]: %s", SNTP_PRIMARY_SERVER);

#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
  esp_sntp_setservername(1, "pool.ntp.org");
  ESP_LOGI(tag, "SNTP server [1]: pool.ntp.org");
#endif
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 2
  esp_sntp_setservername(2, "time.google.com");
  ESP_LOGI(tag, "SNTP server [2]: time.google.com");
#endif

#if CONFIG_LWIP_SNTP_MAX_SERVERS <= 1
  ESP_LOGW(tag, "Only 1 NTP server slot! Set CONFIG_LWIP_SNTP_MAX_SERVERS=3 in menuconfig");
#endif

  // Created before the task, because the task body waits on it. Bailing out here leaves
  // s_sntp_task NULL, which sntp_sync_notify_connected() already checks.
  s_eg = xEventGroupCreate();
  if (!s_eg)
  {
    ESP_LOGE(tag, "Failed to create SNTP event group — time sync disabled");
    return ESP_ERR_NO_MEM;
  }

  // Spawn persistent SNTP task (initial sync + periodic re-sync)
  BaseType_t xret = xTaskCreate(sntp_task, "sntp_sync", 3072, NULL, 2, &s_sntp_task);
  if (xret != pdPASS)
  {
    ESP_LOGE(tag, "Failed to create SNTP task — time sync disabled");
    vEventGroupDelete(s_eg);
    s_eg = NULL;
    s_sntp_task = NULL;
    return ESP_FAIL;
  }

  return ESP_OK;
}

void sntp_sync_notify_connected(void)
{
  if (!s_eg || !s_sntp_task)
  {
    return;
  }
  const time_t now = time(NULL);
  if (s_last_sync_rtc == 0 || difftime(now, s_last_sync_rtc) >= SNTP_RESYNC_INTERVAL_S)
  {
    ESP_LOGI(tag, "WiFi reconnected — waking SNTP for immediate resync");
    xEventGroupSetBits(s_eg, SNTP_BIT_RESYNC);
  }
}

void sntp_sync_force_resync(void)
{
  if (!s_eg || !s_sntp_task)
  {
    return;
  }
  ESP_LOGI(tag, "Manual resync requested — forcing immediate NTP sync");
  xEventGroupSetBits(s_eg, SNTP_BIT_RESYNC);
}

time_t sntp_sync_get_last_sync_time(void)
{
  return s_last_sync_rtc;
}
