// SPDX-License-Identifier: MIT
// ZenClock Wi-Fi Manager — State-machine driven, persistent-task architecture
//
// Features:
//   - 5-state machine: IDLE → SCANNING → CONNECTING → VERIFYING → CONNECTED
//   - Single credential from NVS (namespace "wifi_cred")
//   - No auto-retry on disconnect — fires DISCONNECTED event for BLE re-provisioning
//   - DNS probe after Got IP to verify internet connectivity

#include "wifi_manager.h"
#include "wifi_priv.h"

#include <string.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <lwip/netdb.h>

static const char *tag = "WiFiMgr";

// ============================================================
// Event group bits — set by event handler, consumed by task
// ============================================================
#define BIT_GOT_IP       BIT0
#define BIT_STA_START    BIT2
#define BIT_DISCONNECTED BIT3
#define BIT_STOP         BIT4

// ============================================================
// Module state
// ============================================================
static EventGroupHandle_t s_event_group = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_task_handle = NULL;
static wifi_event_cb_t s_callback = NULL;
static wifi_state_t s_state = WIFI_ST_IDLE;

static char s_ssid[SSID_MAX_LEN] = {0};
static char s_pass[PASS_MAX_LEN] = {0};
static char s_connected_ssid[SSID_MAX_LEN] = {0};
static char s_ip_str[16] = {0};

// Matched AP record — set in SCANNING, used in CONNECTING
static wifi_ap_record_t s_match_ap;

// AP list buffer — allocated once in wifi_task, reused across scans
static wifi_ap_record_t *s_ap_list = NULL;

// True only while the task is blocked in ulTaskNotifyTake() in the IDLE state — i.e. genuinely
// parked, not merely passing through IDLE. Written by the wifi task, polled by wifi_manager_stop()
// from another task, hence volatile.
static volatile bool s_task_parked = false;

// ============================================================
// State helpers (thread-safe)
// ============================================================

static const char *state_name(const wifi_state_t st)
{
  static const char *names[] = {"IDLE", "SCANNING", "CONNECTING", "VERIFYING", "CONNECTED"};
  return (st <= WIFI_ST_CONNECTED) ? names[st] : "?";
}

static wifi_state_t get_state(void)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const wifi_state_t st = s_state;
  xSemaphoreGive(s_mutex);
  return st;
}

static void set_state(const wifi_state_t new_state)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const wifi_state_t old = s_state;
  s_state = new_state;
  xSemaphoreGive(s_mutex);
  if (old != new_state)
  {
    ESP_LOGI(tag, "[%s → %s]", state_name(old), state_name(new_state));
  }
}

// ============================================================
// Callback helper
// ============================================================
static void fire_event(wifi_manager_event_t event)
{
  if (s_callback)
  {
    s_callback(event);
  }
}

// ============================================================
// Check if stop was requested (non-blocking)
// ============================================================
// Peek at BIT_STOP without consuming it. Used to suppress failure events on paths that were
// interrupted by a deliberate stop: firing NO_MATCH / ALL_FAILED there would make the app
// schedule a reconnect that later fights BLE provisioning for the radio.
static bool stop_requested(void)
{
  return (xEventGroupGetBits(s_event_group) & BIT_STOP) != 0;
}

static bool check_stop_signal(void)
{
  const EventBits_t bits = xEventGroupGetBits(s_event_group);
  if (bits & BIT_STOP)
  {
    xEventGroupClearBits(s_event_group, BIT_STOP | BIT_GOT_IP | BIT_DISCONNECTED);
    ESP_LOGI(tag, "Stop signal received");
    set_state(WIFI_ST_IDLE);
    return true;
  }
  return false;
}

// ============================================================
// Event handler — ONLY sets bits, zero logic
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void wifi_event_handler(void *arg, // NOLINT(readability-non-const-parameter)
                               const esp_event_base_t base,
                               const int32_t id,
                               void *data)
{
  (void) arg;
  if (base == WIFI_EVENT)
  {
    switch (id)
    {
    case WIFI_EVENT_STA_START:
      ESP_LOGI(tag, "STA interface started");
      xEventGroupSetBits(s_event_group, BIT_STA_START);
      break;

    case WIFI_EVENT_STA_DISCONNECTED:
    {
      const wifi_event_sta_disconnected_t *evt = data;
      ESP_LOGW(tag, "Disconnected (reason=%d)", evt->reason);
      s_ip_str[0] = '\0';
      xEventGroupSetBits(s_event_group, BIT_DISCONNECTED);
      break;
    }

    default:
      break;
    }
  }
  else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
  {
    const ip_event_got_ip_t *evt = (ip_event_got_ip_t *) data;
    ESP_LOGI(tag, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
    xEventGroupSetBits(s_event_group, BIT_GOT_IP);
  }
}

// ============================================================
// Compare function for sorting AP records by RSSI (descending)
// ============================================================
static int rssi_compare(const void *a, const void *b) // NOLINT(misc-unused-parameters) - a is used via the cast below
{
  const auto ap_a = (const wifi_ap_record_t *) a;
  const wifi_ap_record_t *ap_b = b;
  return ap_b->rssi - ap_a->rssi;
}

// ============================================================
// Fast scan — single-channel targeted scan by BSSID + channel.
// Returns 1 if the target SSID was found on that channel, 0 otherwise.
// Used on boot when AP hint is cached in NVS (~0.5s vs ~8s full scan).
// ============================================================
static int do_fast_scan(
    wifi_ap_record_t *out, int max_aps, const uint8_t *bssid, const uint8_t channel, const char *target_ssid) // NOLINT
{
  const wifi_scan_config_t scan_cfg = {
      .bssid = (uint8_t *) bssid,
      .channel = channel,
      .show_hidden = true,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time.active.min = 100,
      .scan_time.active.max = FAST_SCAN_TIMEOUT_MS,
  };

  if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK)
  {
    return 0;
  }

  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count == 0)
  {
    return 0;
  }

  wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
  if (!records)
  {
    esp_wifi_scan_get_ap_records(&ap_count, NULL);
    return 0;
  }
  esp_wifi_scan_get_ap_records(&ap_count, records);

  int found = 0;
  for (int i = 0; i < ap_count && found < max_aps; i++)
  {
    if (strcmp((const char *) records[i].ssid, target_ssid) == 0)
    {
      out[found++] = records[i];
    }
  }
  free(records);
  return found;
}

// ============================================================
// Aggregated scan — run multiple rounds, merge by BSSID
// Returns number of unique APs in the merged buffer.
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static int do_aggregated_scan(wifi_ap_record_t *merged, int max_aps) // NOLINT
{
  int total = 0;

  for (int round = 0; round < SCAN_ROUNDS; round++)
  {
    // esp_wifi_scan_start() below is blocking and there is no scan_stop() anywhere, so a stop
    // request can only be honoured between rounds — this caps the wait at one round (~4s)
    // instead of all three (~12s), after which BLE provisioning would race the live radio.
    if (xEventGroupGetBits(s_event_group) & BIT_STOP)
    {
      ESP_LOGI(tag, "Stop requested — aborting scan after %d round(s)", round);
      break;
    }

    ESP_LOGI(tag, "Scan round %d/%d...", round + 1, SCAN_ROUNDS);

    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = SCAN_MIN_TIME_MS,
        .scan_time.active.max = SCAN_MAX_TIME_MS,
    };

    const esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != ESP_OK)
    {
      ESP_LOGW(tag, "Scan round %d failed: %s", round + 1, esp_err_to_name(ret));
      continue;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(tag, "  Round %d: %d APs", round + 1, ap_count);

    if (ap_count == 0)
    {
      continue;
    }

    wifi_ap_record_t *round_aps = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!round_aps)
    {
      ESP_LOGW(tag, "  Failed to allocate round buffer, skipping");
      esp_wifi_scan_get_ap_records(&ap_count, NULL);
      continue;
    }
    esp_wifi_scan_get_ap_records(&ap_count, round_aps);

    // Merge: if BSSID already seen, keep best RSSI; otherwise append
    for (int i = 0; i < ap_count; i++)
    {
      bool found = false;
      for (int j = 0; j < total; j++)
      {
        if (memcmp(merged[j].bssid, round_aps[i].bssid, 6) == 0)
        {
          if (round_aps[i].rssi > merged[j].rssi)
          {
            merged[j] = round_aps[i];
          }
          found = true;
          break;
        }
      }
      if (!found && total < max_aps)
      {
        merged[total++] = round_aps[i];
      }
    }

    free(round_aps);

    if (round < SCAN_ROUNDS - 1)
    {
      vTaskDelay(pdMS_TO_TICKS(SCAN_INTER_DELAY_MS));
    }
  }

  if (total > 0)
  {
    qsort(merged, total, sizeof(wifi_ap_record_t), rssi_compare);
  }

  return total;
}

// ============================================================
// Try connecting to a single AP with the stored password
// Returns true if connected (got IP), false otherwise.
// ============================================================
static bool try_connect_candidate(const wifi_ap_record_t *ap, const char *password)
{
  const char *ssid = (const char *) ap->ssid;
  fire_event(WIFI_MGR_CONNECTING);

  wifi_config_t wifi_cfg = {0};
  size_t ssid_len = strlen(ssid);
  if (ssid_len > sizeof(wifi_cfg.sta.ssid))
  {
    ssid_len = sizeof(wifi_cfg.sta.ssid);
  }
  memcpy(wifi_cfg.sta.ssid, ssid, ssid_len);

  if (password && password[0] != '\0')
  {
    size_t pass_len = strlen(password);
    if (pass_len > sizeof(wifi_cfg.sta.password))
    {
      pass_len = sizeof(wifi_cfg.sta.password);
    }
    memcpy(wifi_cfg.sta.password, password, pass_len);
  }

  wifi_cfg.sta.bssid_set = false;
  wifi_cfg.sta.channel = 0;

  esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
  if (ret != ESP_OK)
  {
    ESP_LOGE(tag, "set_config failed: %s", esp_err_to_name(ret));
    return false;
  }

  // Only disconnect if currently associated (e.g. after BLE provisioning left Wi-Fi connected).
  // Unconditional disconnect + delay on a fresh boot wastes 300ms unnecessarily.
  wifi_ap_record_t existing;
  if (esp_wifi_sta_get_ap_info(&existing) == ESP_OK)
  {
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
  }

  xEventGroupClearBits(s_event_group, BIT_GOT_IP | BIT_DISCONNECTED);
  esp_wifi_connect();

  // BIT_STOP is in the wait mask so a stop request cuts the 15s wait short, but clearOnExit
  // is pdFALSE: consuming BIT_STOP here would hide it from check_stop_signal() at the loop
  // top and the task would carry on as if no stop had been requested.
  EventBits_t bits = xEventGroupWaitBits(s_event_group, BIT_GOT_IP | BIT_DISCONNECTED | BIT_STOP, pdFALSE, pdFALSE,
                                         pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
  xEventGroupClearBits(s_event_group, BIT_GOT_IP | BIT_DISCONNECTED);

  if (bits & BIT_STOP)
  {
    esp_wifi_disconnect();
    return false;
  }

  if (bits & BIT_GOT_IP)
  {
    strncpy(s_connected_ssid, ssid, SSID_MAX_LEN - 1);
    s_connected_ssid[SSID_MAX_LEN - 1] = '\0';
    return true;
  }

  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(500));
  return false;
}

// ============================================================
// DNS probe — verify internet connectivity after getting IP
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static bool do_dns_probe(void)
{
  ESP_LOGI(tag, "Verifying internet connectivity...");
  fire_event(WIFI_MGR_GOT_IP);

  for (int probe = 0; probe < DNS_PROBE_MAX; probe++)
  {
    const EventBits_t bits = xEventGroupGetBits(s_event_group);
    if (bits & (BIT_DISCONNECTED | BIT_STOP))
    {
      ESP_LOGW(tag, "Connection lost during DNS probe");
      return false;
    }

    struct addrinfo hints = {.ai_family = AF_INET};
    struct addrinfo *res = NULL;
    const int err = getaddrinfo(DNS_PROBE_HOST, "123", &hints, &res);
    if (err == 0 && res != NULL)
    {
      freeaddrinfo(res);
      ESP_LOGI(tag, "DNS probe OK (attempt %d/%d)", probe + 1, DNS_PROBE_MAX);
      return true;
    }
    ESP_LOGD(tag, "DNS probe %d/%d failed", probe + 1, DNS_PROBE_MAX);

    // Wait instead of vTaskDelay so a stop or a disconnect cuts the pause short. A plain delay
    // here meant wifi_manager_stop() had to sit through up to DNS_PROBE_INTERVAL_MS per attempt
    // on top of getaddrinfo(). Skipped after the last attempt — it bought nothing but latency.
    if (probe + 1 < DNS_PROBE_MAX)
    {
      const EventBits_t waited = xEventGroupWaitBits(s_event_group, BIT_STOP | BIT_DISCONNECTED, pdFALSE, pdFALSE,
                                                     pdMS_TO_TICKS(DNS_PROBE_INTERVAL_MS));
      if (waited & (BIT_STOP | BIT_DISCONNECTED))
      {
        ESP_LOGW(tag, "Connection lost during DNS probe");
        return false;
      }
    }
  }

  // No internet confirmed. The caller still treats the link as usable — a LAN-only or
  // captive-portal network is a working network for everything except NTP — but this is a
  // genuine negative result, not a success.
  ESP_LOGW(tag, "DNS probe failed after %d attempts — no confirmed internet", DNS_PROBE_MAX);
  return false;
}

// ============================================================
// Persistent Wi-Fi task — state machine loop, never exits
// ============================================================
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void wifi_task(void *arg) // NOLINT(readability-non-const-parameter)
{
  (void) arg;
  // ── One-time setup ──

  // Start Wi-Fi driver
  xEventGroupClearBits(s_event_group, BIT_STA_START);
  const esp_err_t ret = esp_wifi_start();
  if (ret != ESP_OK)
  {
    ESP_LOGE(tag, "esp_wifi_start failed: %s — task halted", esp_err_to_name(ret));
    s_task_parked = true; // halted for good — let wifi_manager_stop() return at once
    for (;;)              // NOLINT
    {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
  }
  xEventGroupWaitBits(s_event_group, BIT_STA_START, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));

  // Allocate merged AP buffer (reused across all scan cycles)
  s_ap_list = calloc(MAX_UNIQUE_APS, sizeof(wifi_ap_record_t));
  if (!s_ap_list)
  {
    ESP_LOGE(tag, "Failed to allocate AP buffer — task halted");
    s_task_parked = true; // halted for good — let wifi_manager_stop() return at once
    for (;;)              // NOLINT
    {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
  }

  // ── State machine loop ──

  for (;;) // NOLINT
  {
    if (check_stop_signal())
    {
      continue;
    }

    switch (get_state())
    {

    // ────────────────────────────────────────────
    // IDLE — wait for wifi_manager_start()
    // ────────────────────────────────────────────
    case WIFI_ST_IDLE:
    {
      s_connected_ssid[0] = '\0';

      // s_task_parked is what wifi_manager_stop() actually waits on. The state alone is not
      // enough: it still reads IDLE from here until set_state() below, across an NVS read and
      // esp_wifi_sta_get_ap_info() — a stop landing in that window would otherwise be told the
      // task had settled while it was in fact about to start a full scan.
      s_task_parked = true;
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      s_task_parked = false;

      // Load single credential from NVS
      if (!wifi_cred_load(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass)))
      {
        ESP_LOGW(tag, "No credential in NVS");
        fire_event(WIFI_MGR_NO_CRED);
        // Stay in IDLE — BLE provisioning will call wifi_manager_start() again
        break;
      }

      ESP_LOGI(tag, "Loaded credential: SSID=\"%s\"", s_ssid);

      // After BLE provisioning, Wi-Fi is already connected (network_prov_mgr verified it).
      // Skip scan+connect and go straight to VERIFYING.
      wifi_ap_record_t ap_info;
      if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && strcmp((const char *) ap_info.ssid, s_ssid) == 0)
      {
        ESP_LOGI(tag, "Already connected to \"%s\" — skipping scan+connect", s_ssid);
        strncpy(s_connected_ssid, s_ssid, SSID_MAX_LEN - 1);
        s_connected_ssid[SSID_MAX_LEN - 1] = '\0';
        xEventGroupClearBits(s_event_group, BIT_GOT_IP | BIT_DISCONNECTED);
        set_state(WIFI_ST_VERIFYING);
        break;
      }

      set_state(WIFI_ST_SCANNING);
      break;
    }

    // ────────────────────────────────────────────
    // SCANNING — fast targeted scan, then full aggregated fallback
    // ────────────────────────────────────────────
    case WIFI_ST_SCANNING:
    {
      fire_event(WIFI_MGR_SCANNING);
      memset(s_ap_list, 0, MAX_UNIQUE_APS * sizeof(wifi_ap_record_t));

      int ap_count = 0;
      uint8_t hint_bssid[6];
      uint8_t hint_channel;
      if (wifi_cred_load_ap_hint(hint_bssid, &hint_channel))
      {
        ap_count = do_fast_scan(s_ap_list, MAX_UNIQUE_APS, hint_bssid, hint_channel, s_ssid);
        if (ap_count > 0)
        {
          ESP_LOGI(tag, "Fast scan hit: \"%s\" ch=%d", s_ssid, hint_channel);
        }
        else
        {
          ESP_LOGI(tag, "Fast scan miss (ch=%d) — falling back to full scan", hint_channel);
          wifi_cred_clear_ap_hint(); // stale hint — clear it
        }
      }

      if (ap_count == 0)
      {
        ap_count = do_aggregated_scan(s_ap_list, MAX_UNIQUE_APS);
        ESP_LOGI(tag, "Aggregated scan: %d unique APs after %d rounds", ap_count, SCAN_ROUNDS);
      }

      // Interrupted by wifi_manager_stop(): fall through to the loop top, which consumes
      // BIT_STOP and parks in IDLE. Firing NO_MATCH here would schedule a reconnect.
      if (stop_requested())
      {
        break;
      }

      fire_event(WIFI_MGR_SCAN_DONE);

      if (ap_count == 0)
      {
        ESP_LOGW(tag, "No APs found in scan");
        fire_event(WIFI_MGR_NO_MATCH);
        set_state(WIFI_ST_IDLE);
        break;
      }

      // Find AP matching stored SSID (list already sorted by RSSI descending)
      int match_idx = -1;
      for (int i = 0; i < ap_count; i++)
      {
        if (strcmp((const char *) s_ap_list[i].ssid, s_ssid) == 0)
        {
          match_idx = i;
          break;
        }
      }

      if (match_idx < 0)
      {
        ESP_LOGW(tag, "AP \"%s\" not found in scan", s_ssid);
        fire_event(WIFI_MGR_NO_MATCH);
        set_state(WIFI_ST_IDLE);
        break;
      }

      ESP_LOGI(tag, "Found \"%s\" (RSSI: %d, CH: %d)", s_ssid, s_ap_list[match_idx].rssi, s_ap_list[match_idx].primary);
      s_match_ap = s_ap_list[match_idx];
      set_state(WIFI_ST_CONNECTING);
      break;
    }

    // ────────────────────────────────────────────
    // CONNECTING — single attempt with stored pass
    // ────────────────────────────────────────────
    case WIFI_ST_CONNECTING:
    {
      ESP_LOGI(tag, "Connecting to \"%s\"...", s_ssid);

      if (try_connect_candidate(&s_match_ap, s_pass))
      {
        ESP_LOGI(tag, "Connected to \"%s\"!", s_ssid);
        wifi_cred_save_ap_hint(s_match_ap.bssid, s_match_ap.primary);
        set_state(WIFI_ST_VERIFYING);
      }
      else if (stop_requested())
      {
        // Stopped mid-attempt, not a genuine failure — stay quiet and let the loop top park us.
        ESP_LOGI(tag, "Connect aborted by stop request");
      }
      else
      {
        ESP_LOGW(tag, "Failed to connect to \"%s\"", s_ssid);
        fire_event(WIFI_MGR_ALL_FAILED);
        set_state(WIFI_ST_IDLE);
      }
      break;
    }

    // ────────────────────────────────────────────
    // VERIFYING — DNS probe to confirm internet
    // ────────────────────────────────────────────
    case WIFI_ST_VERIFYING:
    {
      const bool internet_ok = do_dns_probe();

      EventBits_t bits = xEventGroupGetBits(s_event_group);

      // BIT_STOP first: wifi_manager_stop() calls esp_wifi_disconnect(), so a deliberate stop
      // always raises BIT_DISCONNECTED too. Checking DISCONNECTED first reported a spurious
      // WIFI_MGR_DISCONNECTED and had the app schedule a reconnect against its own stop.
      // (The CONNECTED state below already gets this order right.)
      if (bits & BIT_STOP)
      {
        break;
      }
      if (bits & BIT_DISCONNECTED)
      {
        xEventGroupClearBits(s_event_group, BIT_DISCONNECTED);
        ESP_LOGW(tag, "Disconnected during verification");
        fire_event(WIFI_MGR_DISCONNECTED);
        set_state(WIFI_ST_IDLE);
        break;
      }

      // Deliberately still CONNECTED when the probe failed: the association and the IP lease are
      // real, and demoting here would stop SNTP and MicroLink from ever starting on a LAN-only
      // network.
      set_state(WIFI_ST_CONNECTED);
      fire_event(WIFI_MGR_CONNECTED);

      if (!internet_ok)
      {
        ESP_LOGW(tag, "Entering CONNECTED without confirmed internet access");
        // After CONNECTED, not before: the handler for that event paints the icon green, so this
        // one has to arrive second to paint over it.
        fire_event(WIFI_MGR_NO_INTERNET);
      }
      break;
    }

    // ────────────────────────────────────────────
    // CONNECTED — wait for disconnect or stop
    // ────────────────────────────────────────────
    case WIFI_ST_CONNECTED:
    {
      EventBits_t bits =
          xEventGroupWaitBits(s_event_group, BIT_DISCONNECTED | BIT_STOP, pdTRUE, pdFALSE, portMAX_DELAY);

      // Both bits were consumed by clearOnExit above, so this state sets IDLE itself rather
      // than falling through to check_stop_signal() at the loop top.
      if (bits & BIT_STOP)
      {
        set_state(WIFI_ST_IDLE);
      }
      else if (bits & BIT_DISCONNECTED)
      {
        fire_event(WIFI_MGR_DISCONNECTED);
        set_state(WIFI_ST_IDLE); // No auto-retry — the app schedules the reconnect
      }
      break;
    }

    default:
      ESP_LOGE(tag, "Invalid state %d — resetting to IDLE", (int) get_state());
      set_state(WIFI_ST_IDLE);
      break;

    } // switch
  } // for(;;)
}

// ============================================================
// Public API
// ============================================================

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
esp_err_t wifi_manager_init(void)
{
  ESP_LOGI(tag, "Initializing WiFi manager...");

  // Network interface
  ESP_ERROR_CHECK(esp_netif_init());

  // Tolerate ESP_ERR_INVALID_STATE: the default loop is process-wide, and whoever creates it
  // first wins. Aborting because someone else got there is a boot loop over nothing.
  const esp_err_t loop_ret = esp_event_loop_create_default();
  if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE)
  {
    ESP_LOGE(tag, "esp_event_loop_create_default failed: %s", esp_err_to_name(loop_ret));
    return loop_ret;
  }

  // Return deliberately discarded: nothing here ever needs the handle back, and there is no
  // deinit path to hand it to. If one is ever added, esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")
  // retrieves it — no need to carry a file-scope pointer that nothing reads.
  (void) esp_netif_create_default_wifi_sta();

  // Wi-Fi driver
  const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // Synchronization primitives. Both are dereferenced unguarded throughout this file, so a
  // silent NULL here would surface much later as a FreeRTOS assert inside an unrelated call.
  s_event_group = xEventGroupCreate();
  s_mutex = xSemaphoreCreateMutex();
  if (!s_event_group || !s_mutex)
  {
    ESP_LOGE(tag, "Failed to allocate WiFi synchronization primitives");
    if (s_event_group)
    {
      vEventGroupDelete(s_event_group);
      s_event_group = NULL;
    }
    if (s_mutex)
    {
      vSemaphoreDelete(s_mutex);
      s_mutex = NULL;
    }
    return ESP_ERR_NO_MEM;
  }

  // Event handlers — bits only
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

  // Station mode
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  // Create persistent task (starts in IDLE, waits for notification)
  BaseType_t xret = xTaskCreatePinnedToCore(wifi_task, "wifi_mgr",
                                            4096 * 2, // 8KB stack
                                            NULL, 3,  // Priority 3
                                            &s_task_handle,
                                            0); // Pin to core 0 (WiFi core)

  if (xret != pdPASS)
  {
    ESP_LOGE(tag, "Failed to create WiFi task");
    return ESP_FAIL;
  }

  ESP_LOGI(tag, "WiFi manager initialized");
  return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
  if (get_state() != WIFI_ST_IDLE)
  {
    ESP_LOGW(tag, "Cannot start — state is %s", state_name(get_state()));
    return ESP_ERR_INVALID_STATE;
  }

  // Clear leftover bits before waking the task. BIT_STOP in particular: while the task sits
  // in IDLE it is blocked on ulTaskNotifyTake() and never reaches check_stop_signal(), so a
  // stop requested in IDLE stays latched. Without this, the first loop iteration after the
  // notification would consume that stale bit, drop straight back to IDLE and fire no event
  // at all — leaving the device offline until reboot, with no reconnect scheduled.
  xEventGroupClearBits(s_event_group, BIT_STOP | BIT_GOT_IP | BIT_DISCONNECTED);

  xTaskNotifyGive(s_task_handle);
  return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
  return get_state() == WIFI_ST_CONNECTED;
}

esp_err_t wifi_manager_stop(void)
{
  ESP_LOGI(tag, "Stopping WiFi...");

  // Always set BIT_STOP, even when the state reads IDLE. An earlier version skipped it to avoid
  // leaving a stale bit behind, but IDLE is also the state the task passes through on its way out
  // of ulTaskNotifyTake(), so skipping it let the task walk straight into a full scan while the
  // caller believed WiFi had stopped. A latched bit is harmless: wifi_manager_start() clears it.
  xEventGroupSetBits(s_event_group, BIT_STOP);
  esp_wifi_disconnect();

  // Called from the wifi task itself — fire_event(WIFI_MGR_NO_CRED) reaches on_wifi_event, which
  // stops WiFi before starting provisioning. Waiting here would be waiting on ourselves; the task
  // consumes BIT_STOP at the loop top as soon as the callback returns.
  if (xTaskGetCurrentTaskHandle() == s_task_handle)
  {
    return ESP_OK;
  }

  // Block until the task has actually parked. Callers (do_reset_wifi) start BLE provisioning
  // immediately afterwards, and network_prov_mgr conflicts with a radio that is still scanning
  // or associating.
  for (int waited = 0; waited < STOP_TIMEOUT_MS; waited += STOP_POLL_MS)
  {
    if (get_state() == WIFI_ST_IDLE && s_task_parked)
    {
      return ESP_OK;
    }
    vTaskDelay(pdMS_TO_TICKS(STOP_POLL_MS));
  }

  ESP_LOGW(tag, "Stop timed out after %dms — task still in %s", STOP_TIMEOUT_MS, state_name(get_state()));
  return ESP_ERR_TIMEOUT;
}

void wifi_manager_set_callback(wifi_event_cb_t cb)
{
  s_callback = cb;
}

wifi_state_t wifi_manager_get_state(void)
{
  return get_state();
}

const char *wifi_manager_get_ssid(void)
{
  return s_connected_ssid[0] ? s_connected_ssid : NULL;
}

const char *wifi_manager_get_ip_str(void)
{
  return s_ip_str[0] ? s_ip_str : NULL;
}
