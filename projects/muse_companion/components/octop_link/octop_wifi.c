// SPDX-License-Identifier: Apache-2.0
// Wi-Fi ownership for the companion. Two modes, never at the same time:
//   portal  : AP + STA (APSTACK) so the setup page can scan and test-join.
//   runtime : STA only, with endless backoff retry.
// Credentials live in NVS namespace "companion_wifi" and are written by the
// setup portal before it clears the setup flag and reboots.
#include "octop_wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "octop_wifi";
enum { SCAN_MAX = 24, CONNECT_WAIT_MS = 15000 };

/* Newlib on the host test toolchain has no strlcpy, so copy bounded text with
 * a local helper instead of a libc extension. */
static void copy_text(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

struct octop_wifi_t {
    esp_netif_t *sta, *ap;
    SemaphoreHandle_t up;
    esp_timer_handle_t retry_timer;
    wifi_ap_record_t scan[SCAN_MAX];
    bool wifi_started, station_active, connected, portal;
    uint8_t retry;
};
static struct octop_wifi_t *wifi;

static bool ssid_valid(const char *ssid)
{
    size_t n = ssid ? strlen(ssid) : 0;
    if (n == 0 || n > 32) return false;
    /* The ESP-IDF AP config only takes raw bytes, so quotes and control
     * characters would silently change the network being joined. */
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)ssid[i] < 0x20 || (unsigned char)ssid[i] == 0x7f) return false;
    return true;
}
static bool password_valid(const char *pass)
{
    size_t n = pass ? strlen(pass) : 0;
    if (n == 0) return true;                      /* open network */
    if (n < 8 || n > 64) return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)pass[i] < 0x20 || (unsigned char)pass[i] == 0x7f) return false;
    return true;
}

esp_err_t octop_wifi_save(const char *ssid, const char *password)
{
    if (!ssid_valid(ssid) || !password_valid(password)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open("companion_wifi", NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", password && *password ? password : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool octop_wifi_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    nvs_handle_t h;
    if (nvs_open("companion_wifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = ssid_cap;
    esp_err_t err = nvs_get_str(h, "ssid", ssid, &n);
    n = pass_cap;
    if (err == ESP_OK) err = nvs_get_str(h, "pass", pass, &n);
    nvs_close(h);
    if (err != ESP_OK) return false;
    return ssid_valid(ssid);
}

bool octop_wifi_configured(void)
{
    char ssid[33], pass[65];
    return octop_wifi_load(ssid, sizeof(ssid), pass, sizeof(pass));
}

bool octop_wifi_connected(void) { return wifi && wifi->connected; }

static void ip_event(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    (void)ctx; (void)data;
    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) return;
    if (!wifi->station_active) return;
    wifi->connected = true;
    wifi->retry = 0;
    if (wifi->up) xSemaphoreGive(wifi->up);
    ESP_LOGI(TAG, "station up");
}

static void retry_connect(void *ctx)
{
    (void)ctx;
    if (!wifi || !wifi->station_active || wifi->connected) return;
    if (esp_wifi_connect() != ESP_OK) ESP_LOGW(TAG, "connect request failed");
}

static void wifi_event(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    (void)ctx; (void)base; (void)data;
    if (id != WIFI_EVENT_STA_START) {
        if (id == WIFI_EVENT_STA_DISCONNECTED && wifi->station_active) {
            wifi->connected = false;
            /* The portal is a one-shot test join; only the runtime keeps
             * dialling. Backoff is a timer because sleeping in this handler
             * would park the whole system event queue, setup page included. */
            if (wifi->portal) return;
            uint32_t wait = wifi->retry < 6 ? 1000u << wifi->retry : 60000;
            if (wifi->retry < 8) ++wifi->retry;
            ESP_LOGW(TAG, "station down, retry in %ums", (unsigned)wait);
            esp_timer_stop(wifi->retry_timer);
            if (esp_timer_start_once(wifi->retry_timer, (uint64_t)wait * 1000) != ESP_OK)
                retry_connect(NULL);
        }
        return;
    }
    if (wifi->station_active && esp_wifi_connect() != ESP_OK)
        ESP_LOGW(TAG, "initial connect failed");
}

static esp_err_t radio_init(bool ap, uint8_t channel, const char *ap_ssid)
{
    if (wifi) return ESP_OK;
    struct octop_wifi_t *w = calloc(1, sizeof(*w));
    if (!w) return ESP_ERR_NO_MEM;
    w->up = xSemaphoreCreateBinary();
    if (!w->up) { free(w); return ESP_ERR_NO_MEM; }
    const esp_timer_create_args_t retry_args = {.callback = retry_connect,
        .dispatch_method = ESP_TIMER_TASK, .name = "octop_wifi_retry"};
    if (esp_timer_create(&retry_args, &w->retry_timer) != ESP_OK) {
        vSemaphoreDelete(w->up); free(w); return ESP_ERR_NO_MEM; }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;

    w->sta = esp_netif_create_default_wifi_sta();
    if (ap) {
        w->ap = esp_netif_create_default_wifi_ap();
        if (!w->ap) goto nomem;
    }
    if (!w->sta) goto nomem;

    /* esp_wifi_init copies this, so the default macro's brace initializer can
     * only be used as a declaration, never as an assigned expression. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) goto fail;

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event, NULL);
    if (err != ESP_OK) goto fail;

    /* Publish before esp_wifi_start(): the event handlers below run on the
     * default event loop and dereference the singleton. */
    wifi = w;
    w->portal = ap;
    if (ap) {
        wifi_config_t ap_cfg = {0};
        size_t n = strlen(ap_ssid);
        if (n > 32) n = 32;
        memcpy(ap_cfg.ap.ssid, ap_ssid, n);
        ap_cfg.ap.ssid_len = (uint8_t)n;
        ap_cfg.ap.channel = channel ? channel : 1;
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        ap_cfg.ap.max_connection = 2;
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        if (err != ESP_OK) goto fail;
    }
    /* A band-agnostic scan is the only way 5 GHz networks show up in the list,
     * even though this board's own radio is 2.4 GHz. */
    wifi_country_t country = {.cc = "US", .schan = 1, .nchan = 13,
                              .policy = WIFI_COUNTRY_POLICY_MANUAL};
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_country(&country);
    if (err != ESP_OK) goto fail;
#if SOC_WIFI_SUPPORT_5G
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif
    err = esp_wifi_set_mode(ap ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) goto fail;
    if (!ap) {
        wifi_config_t sta_cfg = {0};
        char ssid[33], pass[65];
        if (!octop_wifi_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
            err = ESP_ERR_NOT_FOUND;
            goto fail;
        }
        copy_text((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), ssid);
        copy_text((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), pass);
        sta_cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK) goto fail;
        w->station_active = true;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    w->wifi_started = true;
    return ESP_OK;
nomem:
    err = ESP_ERR_NO_MEM;
fail:
    if (w->retry_timer) esp_timer_delete(w->retry_timer);
    if (w->up) vSemaphoreDelete(w->up);
    if (w->wifi_started) esp_wifi_stop();
    if (w->wifi_started) esp_wifi_deinit();
    if (w->sta) esp_netif_destroy_default_wifi(w->sta);
    if (w->ap) esp_netif_destroy_default_wifi(w->ap);
    free(w);
    wifi = NULL;
    return err != ESP_OK ? err : ESP_FAIL;
}

esp_err_t octop_wifi_start(void)
{
    if (!octop_wifi_configured()) return ESP_ERR_NOT_FOUND;
    return radio_init(false, 0, NULL);
}

/* ---- setup portal ---- */
esp_err_t octop_wifi_portal_start(const char *ap_ssid)
{
    return radio_init(true, 1, ap_ssid);
}

size_t octop_wifi_scan(octop_ap_record_t *out, size_t max)
{
    if (!wifi || !out || !max) return 0;
    wifi_scan_config_t cfg = {.show_hidden = false};
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed");
        return 0;
    }
    uint16_t found = SCAN_MAX;
    if (esp_wifi_scan_get_ap_records(&found, wifi->scan) != ESP_OK) return 0;
    /* A blocking scan can leave the soft AP parked on the last channel it
     * hopped to; the portal is configured for channel 1. */
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    size_t written = 0;
    for (uint16_t i = 0; i < found && written < max; ++i) {
        if (!wifi->scan[i].ssid[0]) continue;
        bool seen = false;
        for (size_t j = 0; j < written; ++j)
            if (!strcmp(out[j].ssid, (const char *)wifi->scan[i].ssid)) { seen = true; break; }
        if (seen) continue;
        copy_text(out[written].ssid, sizeof(out[written].ssid),
                  (const char *)wifi->scan[i].ssid);
        out[written].rssi = wifi->scan[i].rssi;
        ++written;
    }
    return written;
}

bool octop_wifi_portal_join(const char *ssid, const char *password)
{
    if (!wifi || !ssid_valid(ssid) || !password_valid(password)) return false;
    wifi_config_t cfg = {0};
    copy_text((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), ssid);
    copy_text((char *)cfg.sta.password, sizeof(cfg.sta.password), password);
    cfg.sta.threshold.authmode = password && *password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
    wifi->station_active = true;
    wifi->connected = false;
    wifi->retry = 0;
    xSemaphoreTake(wifi->up, 0);
    if (esp_wifi_connect() != ESP_OK) {
        wifi->station_active = false;
        return false;
    }
    bool up = xSemaphoreTake(wifi->up, pdMS_TO_TICKS(CONNECT_WAIT_MS)) == pdTRUE;
    if (!up) {
        wifi->station_active = false;
        esp_wifi_disconnect();
    }
    return up;
}
