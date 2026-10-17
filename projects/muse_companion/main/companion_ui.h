// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_gsp.h"
typedef struct companion_ui_t *companion_ui_handle_t;
esp_gsp_err_t companion_ui_create(esp_gsp_handle_t ui,companion_ui_handle_t *out);
void companion_ui_delete(companion_ui_handle_t state);
