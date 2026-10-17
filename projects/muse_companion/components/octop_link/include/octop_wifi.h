// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char ssid[33];
    int16_t rssi;
} octop_ap_record_t;

/* Runtime: join the saved network and keep retrying while the app runs.
 * ESP_ERR_NOT_FOUND when no network has been provisioned yet. */
esp_err_t octop_wifi_start(void);
bool octop_wifi_connected(void);
bool octop_wifi_configured(void);
bool octop_wifi_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
esp_err_t octop_wifi_save(const char *ssid, const char *password);

/* Setup portal only: the portal owns the radio in AP+STA mode so it can list
 * networks and test-join the one the user picked before anything is saved. */
esp_err_t octop_wifi_portal_start(const char *ap_ssid);
size_t octop_wifi_scan(octop_ap_record_t *out, size_t max);
/* Blocks up to 15 s waiting for an address; false leaves the station idle. */
bool octop_wifi_portal_join(const char *ssid, const char *password);
