// SPDX-License-Identifier: Apache-2.0
// Internal shared surface of the Octop link: URL parsing, one-shot JSON
// requests, the JWT cache and the frame shapes both sides speak.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"

/* Bounded copy with a guaranteed terminator. Newlib on the host test
 * toolchain has no strlcpy, so this is the workspace's only string copier. */
static inline void octop_copy_text(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

enum {
    OCTOP_HOST_CAP = 96,
    OCTOP_PATH_CAP = 192,
    OCTOP_TOKEN_CAP = 2048,
    OCTOP_THREAD_CAP = 48,
    OCTOP_DETAIL_CAP = 48
};

typedef struct {
    char host[OCTOP_HOST_CAP];
    uint16_t port;
    bool tls;
    /* Path prefix the URL carried, "" when it addressed the host root. An
     * Octop behind a reverse-proxy subpath only works if we keep it. */
    char base[OCTOP_PATH_CAP];
} octop_host_t;

/* "http://host[:port][/prefix]" / "https://..." into host/port/tls/base.
 * The firmware talks plain HTTP to a self-hosted Octop by default (8088). */
bool octop_url_split(const char *url, octop_host_t *out);

typedef struct {
    long status;
    char *body;   /* PSRAM, NUL-terminated, freed by the caller */
    size_t size;
} octop_response_t;

esp_err_t octop_http_json(const octop_host_t *host, const char *method,
    const char *path, const char *body, const char *bearer,
    uint32_t timeout_ms, size_t body_cap, octop_response_t *out);
void octop_response_free(octop_response_t *resp);

/* Read one string member out of a JSON document without parsing the whole
 * thing: the agent list is far larger than what the device needs from it.
 * Handles \" \\ and \uXXXX (BMP only) so tokens and titles survive. */
bool octop_json_string(const char *json, const char *key, char *out, size_t cap);

/* Raw multipart POST of one WAV clip to a speech-to-text endpoint. `field` is
 * the form name the server expects for the attachment ("audio" for Octop's own
 * /api/voice/stt, "file" for OpenAI-compatible relays). */
esp_err_t octop_http_upload_wav(const octop_host_t *host, const char *path,
    const char *bearer, const char *field, const uint8_t *pcm, size_t pcm_size,
    const char *language, uint32_t timeout_ms, octop_response_t *out);

/* Login cache. Base URL + account come from the credential store. */
bool octop_login(const octop_host_t *host, const char *user, const char *pass,
    char *detail, size_t detail_cap);
const char *octop_bearer(void);
void octop_bearer_invalidate(void);

/* ---- chat WebSocket (singleton: one long-lived connection) ---- */
typedef struct {
    void (*on_frame)(void *ctx, const char *type, const char *content);
    /* handshake_status is the HTTP code of a failed upgrade, 0 for a plain drop. */
    void (*on_lost)(void *ctx, int handshake_status);
    void *ctx;
} octop_ws_handlers_t;
bool octop_ws_start(const octop_host_t *host, const char *agent,
    const char *token, const octop_ws_handlers_t *handlers);
bool octop_ws_open(void);
bool octop_ws_send_turn(const char *thread, const char *text);
bool octop_ws_send_cancel(const char *thread);
void octop_ws_stop(void);

/* ---- speech to text ---- */
/* Transcribe one 16 kHz mono PCM clip. Returns a heap string the caller must
 * free, or NULL with a human-readable `detail` explaining why. `language`
 * selects the hint sent along (0 = zh-CN, 1 = en-US). */
char *octop_stt_transcribe(const uint8_t *pcm, size_t pcm_size,
    unsigned language, char *detail, size_t detail_cap);
