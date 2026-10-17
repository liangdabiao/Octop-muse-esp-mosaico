// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "esp_err.h"

enum { COMPANION_OCTOP_URL_CAP=256, COMPANION_USERNAME_CAP=64,
       COMPANION_PASSWORD_CAP=128, COMPANION_STT_URL_CAP=256,
       COMPANION_STT_KEY_CAP=256 };

/* Octop is the only backend: base URL + an account. The STT pair is optional
 * and lets the device use a custom OpenAI-compatible transcriber instead of
 * Octop's built-in /api/voice/stt. Empty means "use Octop's built-in STT". */
typedef struct {
    const char *octop_url, *username, *password, *stt_url, *stt_key;
} companion_credentials_config_t;

/* App-lifetime immutable snapshot after init. Only the first-boot HTTP task
 * saves a replacement; it restarts the device before any service uses it. */
esp_err_t companion_credentials_init(void);
bool companion_credentials_ready(void);
bool companion_credentials_valid(const companion_credentials_config_t *config);
esp_err_t companion_credentials_save(const companion_credentials_config_t *config);
const char *companion_credentials_octop_url(void);
const char *companion_credentials_username(void);
const char *companion_credentials_password(void);
const char *companion_credentials_stt_url(void);
const char *companion_credentials_stt_key(void);
