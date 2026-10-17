// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct companion_generation_t *companion_generation_handle_t;
/* App-lifetime RPC collector. Buffers belong to this service, protected by its
 * mutex. Voice worker alone takes requests, starts/cancels Octop and finishes. */
esp_err_t companion_generation_create(companion_generation_handle_t *out);
const char *companion_generation_take(companion_generation_handle_t service);
bool companion_generation_cancelled(companion_generation_handle_t service);
bool companion_generation_finished(companion_generation_handle_t service);
void companion_generation_finish(companion_generation_handle_t service,const char *error);
