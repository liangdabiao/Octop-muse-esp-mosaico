// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "esp_err.h"
/* Boot task owns startup; HTTP task owns bounded requests and credential
 * writes; a separate internal-stack task restarts after the response. */
esp_err_t companion_setup_start(void);
bool companion_setup_active(const char **ssid,const char **password);
esp_err_t companion_setup_request(void);
bool companion_setup_request_failed(void);
