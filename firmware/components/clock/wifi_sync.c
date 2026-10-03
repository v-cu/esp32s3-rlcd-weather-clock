#include "wifi_sync.h"
#include "clock_config.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WifiSync";

/* Wi-Fi regulatory domain. Default "01" (world safe mode) only actively scans channels 1-11,
 * so an AP that auto-selects channel 12/13 (legal in PL/EU) may not be found at all. */
#ifndef CLOCK_WIFI_COUNTRY
#define CLOCK_WIFI_COUNTRY "PL"
#endif

#define WIFI_STARTED_BIT   BIT0
#define WIFI_CONNECTED_BIT BIT1
#define WIFI_FAIL_BIT      BIT2

#define ATTEMPT_TIMEOUT_MS     12000   /* one association + DHCP attempt */
#define HINT_ATTEMPT_TIMEOUT_MS 5000   /* first attempt using cached channel/BSSID */

static EventGroupHandle_t s_event_group = NULL;
static bool s_initialized = false;
static bool s_started = false;

static bool    s_wifi_hint_valid = false;
static uint8_t s_wifi_channel = 0;
static uint8_t s_wifi_bssid[6];

static volatile uint8_t s_last_reason = 0;
static int  s_last_rssi = 0;
static char s_last_error[32] = "";
static WifiSyncInfo s_info;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* No auto-connect here - connection attempts are driven only by WifiSync_Connect(),
         * otherwise two concurrent esp_wifi_connect() calls abort each other. */
        xEventGroupSetBits(s_event_group, WIFI_STARTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        s_last_reason = ev ? ev->reason : 0;
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        if (ev) s_info.ip = ev->ip_info.ip.addr;
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

static const char *reason_to_text(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:                 return "no AP found";
        case WIFI_REASON_AUTH_FAIL:                   return "auth failed";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:           return "handshake (pass?)";
        case WIFI_REASON_ASSOC_FAIL:                  return "assoc failed";
        case WIFI_REASON_AUTH_EXPIRE:                 return "auth expired";
        case WIFI_REASON_BEACON_TIMEOUT:              return "beacon timeout";
        case WIFI_REASON_CONNECTION_FAIL:             return "connection failed";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "security mismatch";
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:     return "signal too weak";
        default: break;
    }
    return NULL;
}

static void set_error(const char *text)
{
    strncpy(s_last_error, text, sizeof(s_last_error) - 1);
    s_last_error[sizeof(s_last_error) - 1] = '\0';
}

void WifiSync_Init(void)
{
    if (s_initialized) return;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Config is set on every sync - keep it in RAM, don't wear the flash. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    esp_err_t cc_err = esp_wifi_set_country_code(CLOCK_WIFI_COUNTRY, true);
    if (cc_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set country code: %s", esp_err_to_name(cc_err));
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    s_event_group = xEventGroupCreate();

    setenv("TZ", CLOCK_TIMEZONE, 1);
    tzset();

    s_initialized = true;
}

static void apply_config(const char *ssid, const char *password, bool use_hint)
{
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, password, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    /* Scan every channel and pick the strongest AP with this SSID (mesh / repeaters). */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    if (use_hint) {
        wc.sta.channel = s_wifi_channel;
        memcpy(wc.sta.bssid, s_wifi_bssid, 6);
        wc.sta.bssid_set = true;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
    }
}

bool WifiSync_Connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!s_initialized) {
        WifiSync_Init();
    }

    set_error("");
    s_last_reason = 0;
    xEventGroupClearBits(s_event_group, WIFI_STARTED_BIT | WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        set_error("wifi driver error");
        return false;
    }

    bool use_hint = s_wifi_hint_valid;
    apply_config(ssid, password, use_hint);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        set_error("wifi start error");
        return false;
    }
    s_started = true;

    xEventGroupWaitBits(s_event_group, WIFI_STARTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));

    /* Short session, so modem sleep only adds latency (and some routers drop DHCP/TLS packets). */
    esp_wifi_set_ps(WIFI_PS_NONE);

    const int64_t start_us = esp_timer_get_time();
    const int64_t deadline_us = start_us + (int64_t)timeout_ms * 1000LL;
    int attempt = 0;
    uint32_t backoff_ms = 500;

    while (esp_timer_get_time() < deadline_us) {
        attempt++;
        int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000LL;
        if (remaining_ms <= 0) break;

        uint32_t attempt_timeout = use_hint ? HINT_ATTEMPT_TIMEOUT_MS : ATTEMPT_TIMEOUT_MS;
        if ((int64_t)attempt_timeout > remaining_ms) attempt_timeout = (uint32_t)remaining_ms;

        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        ESP_LOGI(TAG, "Connect attempt %d (%s), %u ms...", attempt,
                 use_hint ? "cached channel/BSSID" : "full scan", (unsigned)attempt_timeout);

        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        }

        EventBits_t bits = xEventGroupWaitBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(attempt_timeout));

        if (bits & WIFI_CONNECTED_BIT) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                s_wifi_channel = ap_info.primary;
                memcpy(s_wifi_bssid, ap_info.bssid, 6);
                s_wifi_hint_valid = true;
                s_last_rssi = ap_info.rssi;
                s_info.rssi = ap_info.rssi;
                s_info.channel = ap_info.primary;
                memcpy(s_info.bssid, ap_info.bssid, 6);
                ESP_LOGI(TAG, "Connected: ch %d, RSSI %d dBm (attempt %d)", ap_info.primary, ap_info.rssi, attempt);
            }
            strncpy(s_info.ssid, ssid, sizeof(s_info.ssid) - 1);
            s_info.ssid[sizeof(s_info.ssid) - 1] = '\0';
            s_info.connect_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000LL);
            s_info.attempts = attempt;
            s_info.ok_count++;
            s_info.valid = true;
            return true;
        }

        if (bits & WIFI_FAIL_BIT) {
            const char *txt = reason_to_text(s_last_reason);
            char buf[32];
            if (!txt) {
                snprintf(buf, sizeof(buf), "wifi reason %u", (unsigned)s_last_reason);
                txt = buf;
            }
            set_error(txt);
            ESP_LOGW(TAG, "Attempt %d failed: reason %u (%s)", attempt, (unsigned)s_last_reason, txt);
        } else {
            /* Association went through but no IP, or nothing happened at all. Abort the pending
             * attempt so that the next esp_wifi_connect() starts from a clean state. */
            wifi_ap_record_t ap_probe;
            bool associated = (esp_wifi_sta_get_ap_info(&ap_probe) == ESP_OK);
            set_error(associated ? "no IP (DHCP)" : "connect timeout");
            ESP_LOGW(TAG, "Attempt %d timed out", attempt);
            esp_wifi_disconnect();
        }

        if (use_hint) {
            /* Router may have changed channel (auto-channel) or BSSID - forget the hint. */
            ESP_LOGW(TAG, "Dropping cached channel/BSSID, using full scan.");
            s_wifi_hint_valid = false;
            use_hint = false;
            apply_config(ssid, password, false);
        }

        /* Let any late DISCONNECTED event from the aborted attempt arrive before clearing bits. */
        int64_t left_ms = (deadline_us - esp_timer_get_time()) / 1000LL;
        if (left_ms <= (int64_t)backoff_ms + 1000) break;
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (backoff_ms < 2000) backoff_ms *= 2;
    }

    if (s_last_error[0] == '\0') set_error("connect timeout");
    s_info.attempts = attempt;
    s_info.fail_count++;
    ESP_LOGW(TAG, "Wi-Fi connection failed after %d attempts: %s", attempt, s_last_error);
    return false;
}

bool WifiSync_SyncNtp(struct tm *out_time, uint32_t timeout_ms)
{
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CLOCK_NTP_SERVER);
    if (esp_netif_sntp_init(&sntp_config) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed");
        return false;
    }

    bool ok = false;
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) == ESP_OK) {
        time_t now;
        time(&now);
        localtime_r(&now, out_time);
        ok = true;
        ESP_LOGI(TAG, "Time fetched: %02d:%02d:%02d %02d-%02d-%04d",
                 out_time->tm_hour, out_time->tm_min, out_time->tm_sec,
                 out_time->tm_mday, out_time->tm_mon + 1, out_time->tm_year + 1900);
    } else {
        ESP_LOGW(TAG, "NTP sync timeout");
    }
    esp_netif_sntp_deinit();
    return ok;
}

void WifiSync_Disconnect(void)
{
    if (!s_started) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_started = false;
}

const char *WifiSync_LastError(void)
{
    return s_last_error[0] ? s_last_error : "unknown";
}

int WifiSync_LastRssi(void)
{
    return s_last_rssi;
}

void WifiSync_GetInfo(WifiSyncInfo *out)
{
    *out = s_info;
}
