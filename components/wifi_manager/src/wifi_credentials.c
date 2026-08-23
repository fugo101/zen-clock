// SPDX-License-Identifier: MIT
// ZenClock WiFi Manager — Single credential NVS storage
//
// NVS namespace: "wifi_cred"
// Keys: "ssid" (string), "pass" (string)
//
// NVS must already be initialized by settings_init() before any call here.

#include "wifi_manager.h"
#include "wifi_priv.h"

#include <string.h>
#include <esp_log.h>
#include <nvs.h>

static const char *const tag = "WiFiCred";
#define NVS_NAMESPACE   "wifi_cred"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"
#define NVS_KEY_BSSID   "bssid"
#define NVS_KEY_CHANNEL "ch"

// ============================================================
// Internal loader — called only from wifi_manager.c (wifi_task)
// ============================================================

// NOLINTNEXTLINE(misc-unused-parameters) - out_ssid is used below via nvs_get_str
bool wifi_cred_load(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len)
{
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
  {
    return false;
  }

  size_t sl = ssid_len;
  bool ok = (nvs_get_str(h, NVS_KEY_SSID, out_ssid, &sl) == ESP_OK && sl > 1);

  if (ok)
  {
    size_t pl = pass_len;
    esp_err_t ret = nvs_get_str(h, NVS_KEY_PASS, out_pass, &pl);
    if (ret != ESP_OK)
    {
      out_pass[0] = '\0'; // open network
    }
  }

  nvs_close(h);
  return ok;
}

// ============================================================
// Public API
// ============================================================

bool wifi_manager_has_credential(void)
{
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
  {
    return false;
  }
  size_t len = WIFI_SSID_MAX_LEN;
  char ssid[WIFI_SSID_MAX_LEN];
  bool has = (nvs_get_str(h, NVS_KEY_SSID, ssid, &len) == ESP_OK && len > 1);
  nvs_close(h);
  return has;
}

esp_err_t wifi_manager_set_credential(const char *ssid, const char *pass)
{
  if (!ssid || ssid[0] == '\0')
  {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t h;
  esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (ret != ESP_OK)
  {
    return ret;
  }

  ret = nvs_set_str(h, NVS_KEY_SSID, ssid);
  if (ret == ESP_OK)
  {
    ret = nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
  }
  if (ret == ESP_OK)
  {
    ret = nvs_commit(h);
  }

  nvs_close(h);
  ESP_LOGI(tag, "Credential saved: \"%s\"", ssid);
  return ret;
}

esp_err_t wifi_manager_clear_credential(void)
{
  nvs_handle_t h;
  esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (ret != ESP_OK)
  {
    return ret;
  }

  nvs_erase_key(h, NVS_KEY_SSID);
  nvs_erase_key(h, NVS_KEY_PASS);
  nvs_erase_key(h, NVS_KEY_BSSID);
  nvs_erase_key(h, NVS_KEY_CHANNEL);
  ret = nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(tag, "Credential cleared");
  return ret;
}

// ============================================================
// AP hint — BSSID + channel of last successful connection.
// Enables fast single-channel scan on next boot.
// ============================================================

void wifi_cred_save_ap_hint(const uint8_t *bssid, uint8_t channel)
{
  // Idempotent by design. NVS is append-only — updating a key appends a new entry and marks the
  // old one erased — and the caller sits on the LINK_UP → CONNECTED transition, which every
  // backoff reconnect passes through. Skipping an identical write means no future caller has to
  // reason about how often that transition fires before adding a call.
  uint8_t cur_bssid[6];
  uint8_t cur_channel = 0;
  if (wifi_cred_load_ap_hint(cur_bssid, &cur_channel) && cur_channel == channel && memcmp(cur_bssid, bssid, 6) == 0)
  {
    return;
  }

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
  {
    return;
  }
  nvs_set_blob(h, NVS_KEY_BSSID, bssid, 6);
  nvs_set_u8(h, NVS_KEY_CHANNEL, channel);
  nvs_commit(h);
  nvs_close(h);
  // INFO because ESP_LOGD is compiled out (see CLAUDE.md, Non-Architectural Notes): this line is
  // the only on-device evidence the hint was written, and #100 is unverifiable off-device.
  ESP_LOGI(tag, "AP hint saved: ch=%d bssid=%02X:%02X:%02X:%02X:%02X:%02X", channel, bssid[0], bssid[1], bssid[2],
           bssid[3], bssid[4], bssid[5]);
}

bool wifi_cred_load_ap_hint(uint8_t *bssid, uint8_t *channel)
{
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
  {
    return false;
  }
  size_t len = 6;
  bool ok = (nvs_get_blob(h, NVS_KEY_BSSID, bssid, &len) == ESP_OK && len == 6 &&
             nvs_get_u8(h, NVS_KEY_CHANNEL, channel) == ESP_OK && *channel > 0);
  nvs_close(h);
  return ok;
}

void wifi_cred_clear_ap_hint(void)
{
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
  {
    return;
  }
  nvs_erase_key(h, NVS_KEY_BSSID);
  nvs_erase_key(h, NVS_KEY_CHANNEL);
  nvs_commit(h);
  nvs_close(h);
}
