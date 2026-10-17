// SPDX-License-Identifier: Apache-2.0
// Speech-to-text for one recorded clip. Measured against the live Octop:
//   POST /api/voice/stt  multipart field "audio", Bearer JWT
//   -> 200 {"text":"...", "confidence":null}      2.6-4.2 s for 5.9 s of speech
//   -> "language" is optional; a 0.3 s tone still returns 200 with a
//      hallucinated "嗯。", so very short transcripts must be discarded.
//   -> cold requests can hang ~20 s then reset, so the timeout is 25 s and the
//      clip is retried once.
#include "octop_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "companion_credentials.h"

static const char *TAG = "octop_stt";
enum { STT_TIMEOUT_MS = 25000, TRANSCRIPT_CAP = 2048 };

/* Count codepoints, not bytes: a one-syllable invention is still two of them
 * and must be rejected. */
static size_t codepoints(const char *text)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if ((*p & 0xC0) != 0x80) ++n;
    return n;
}

static bool endpoint(octop_host_t *host, char *path, size_t path_cap,
    const char **bearer)
{
    const char *custom = companion_credentials_stt_url();
    if (custom && custom[0]) {
        /* A custom endpoint carries its own full path; nothing to append. */
        if (!octop_url_split(custom, host)) return false;
        path[0] = '\0';
        const char *key = companion_credentials_stt_key();
        *bearer = key && key[0] ? key : NULL;
        return true;
    }
    const char *base = companion_credentials_octop_url();
    if (!base || !octop_url_split(base, host)) return false;
    /* octop_http_json/upload compose scheme + host + host->base + path, so the
     * reverse-proxy prefix must only appear once. */
    octop_copy_text(path, path_cap, "/api/voice/stt");
    *bearer = octop_bearer();
    return true;
}

char *octop_stt_transcribe(const uint8_t *pcm, size_t size, unsigned language,
    char *detail, size_t detail_cap)
{
    octop_host_t host;
    char path[OCTOP_PATH_CAP];
    const char *bearer;
    detail[0] = '\0';
    if (!pcm || size < 3200) {              /* under 100 ms is never a question */
        snprintf(detail, detail_cap, "没有听到声音");
        return NULL;
    }
    if (!endpoint(&host, path, sizeof(path), &bearer)) {
        snprintf(detail, detail_cap, "识别服务未配置");
        return NULL;
    }
    static char transcript[TRANSCRIPT_CAP];
    const char *hint = language == 0 ? "zh-CN" : "en-US";
    char *result = NULL;
    for (unsigned attempt = 0; attempt < 2 && !result; ++attempt) {
        octop_response_t resp = {0};
        esp_err_t err = octop_http_upload_wav(&host, path, bearer, "audio",
            pcm, size, hint, STT_TIMEOUT_MS, &resp);
        if (err != ESP_OK) {
            snprintf(detail, detail_cap, "识别连接失败");
        } else if (resp.status == 200 && resp.body &&
            octop_json_string(resp.body, "text", transcript, sizeof(transcript))) {
            if (codepoints(transcript) < 2) snprintf(detail, detail_cap, "没有听清");
            else result = strdup(transcript);
        } else {
            ESP_LOGW(TAG, "stt status=%ld body=%.120s", resp.status,
                     resp.body ? resp.body : "");
            snprintf(detail, detail_cap, "识别失败 %ld", resp.status);
        }
        octop_response_free(&resp);
    }
    if (!result && !detail[0]) snprintf(detail, detail_cap, "识别失败");
    return result;
}
