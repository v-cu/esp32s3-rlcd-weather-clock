#pragma once

#include <time.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time init of NVS, netif, event loop and the Wi-Fi driver. Also sets TZ. */
void WifiSync_Init(void);

/* Start Wi-Fi and keep retrying until an IP address is obtained or timeout_ms elapses.
 * Returns true when connected (GOT_IP). Always call WifiSync_Disconnect() afterwards. */
bool WifiSync_Connect(const char *ssid, const char *password, uint32_t timeout_ms);

/* Fetch time over SNTP (must be connected). On success system time is set and
 * out_time holds local time. */
bool WifiSync_SyncNtp(struct tm *out_time, uint32_t timeout_ms);

/* Disconnect and stop the Wi-Fi driver (radio off). Safe to call when not connected. */
void WifiSync_Disconnect(void);

/* Short human readable reason of the last failure (e.g. "no AP found"). */
const char *WifiSync_LastError(void);

/* RSSI of the AP from the last successful connection (dBm), 0 if unknown. */
int WifiSync_LastRssi(void);

/* Diagnostics of the last connection. */
typedef struct {
    bool     valid;             /* at least one successful connection since boot */
    char     ssid[33];
    int      rssi;              /* dBm */
    uint8_t  channel;
    uint8_t  bssid[6];
    uint32_t ip;                /* IPv4, network byte order (esp_ip4_addr_t.addr) */
    uint32_t connect_ms;        /* duration of the last successful connect */
    int      attempts;          /* attempts used by the last WifiSync_Connect() */
    uint32_t ok_count;          /* WifiSync_Connect() results since boot */
    uint32_t fail_count;
} WifiSyncInfo;

void WifiSync_GetInfo(WifiSyncInfo *out);

#ifdef __cplusplus
}
#endif
