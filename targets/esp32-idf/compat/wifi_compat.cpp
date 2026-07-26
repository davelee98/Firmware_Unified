/* WiFiClass implementation over esp_wifi. TEMPORARY; part of the shim.
 *
 * Arduino's WiFi.begin() hides netif creation, event-loop setup and the join handshake. IDF
 * separates them, so the one-time init is done lazily here rather than requiring the caller
 * to learn about esp_netif -- which would be a change to the imported sources, and phase B is
 * meant to change as little of them as possible.
 */
#include "WiFi.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "od_wifi";
static bool s_inited = false;
static esp_netif_t *s_netif = nullptr;

static void ensure_init()
{
    if (s_inited) return;
    /* NVS is already initialised by od_hal_nvs; esp_wifi needs it too and calling
     * nvs_flash_init twice is safe. */
    esp_netif_init();
    esp_event_loop_create_default();
    s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_inited = true;
}

int WiFiClass::begin(const char *ssid, const char *pass)
{
    if (!ssid) return WL_CONNECT_FAILED;
    ensure_init();

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (pass) {
        strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    }
    /* Credentials are never logged here, at any level -- ARCHITECTURE.md § "Secrets are never
     * logged verbatim". Presence and length only. */
    ESP_LOGI(TAG, "joining SSID (set, %u chars), password %s",
             (unsigned)strlen(ssid), (pass && *pass) ? "(set)" : "(empty)");

    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_connect();
    return WL_IDLE_STATUS;
}

void WiFiClass::disconnect(bool wifioff)
{
    if (!s_inited) return;
    esp_wifi_disconnect();
    if (wifioff) esp_wifi_stop();
}

int WiFiClass::status() const
{
    if (!s_inited) return WL_IDLE_STATUS;
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? WL_CONNECTED : WL_DISCONNECTED;
}

IPAddress WiFiClass::localIP() const
{
    esp_netif_ip_info_t ip = {};
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        return IPAddress(ip.ip.addr);
    }
    return IPAddress();
}

int32_t WiFiClass::RSSI() const
{
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

int32_t WiFiClass::channel() const
{
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.primary : 0;
}

String WiFiClass::BSSIDstr() const
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return String("");
    char b[18];
    snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X",
             ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    return String(b);
}

WiFiClass WiFi;
