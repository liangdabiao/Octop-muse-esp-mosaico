// SPDX-License-Identifier: Apache-2.0
// One-shot JSON requests, the JWT cache and multipart speech uploads against a
// self-hosted Octop. Plain HTTP is the norm (Octop serves 8088 over cleartext),
// so TLS is chosen by the configured scheme rather than assumed.
#include "octop_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "octop_http";
enum { BODY_DEFAULT = 8192, BODY_MAX = 65536, UPLOAD_CAP = 1024 * 1024 };

bool octop_url_split(const char *url, octop_host_t *out)
{
    if (!url || !out) return false;
    bool tls = false;
    const char *rest = url;
    if (!strncasecmp(url, "https://", 8)) { tls = true; rest = url + 8; }
    else if (!strncasecmp(url, "http://", 7)) rest = url + 7;
    else return false;
    const char *slash = strchr(rest, '/');
    const char *end = slash ? slash : rest + strlen(rest);
    size_t n = (size_t)(end - rest);
    if (n == 0 || n >= OCTOP_HOST_CAP) return false;
    memcpy(out->host, rest, n);
    out->host[n] = '\0';
    out->tls = tls;
    out->port = tls ? 443 : 80;
    char *colon = strrchr(out->host, ':');
    if (colon) {
        long parsed = strtol(colon + 1, NULL, 10);
        if (parsed <= 0 || parsed > 65535) return false;
        out->port = (uint16_t)parsed;
        *colon = '\0';
        if (!out->host[0]) return false;
    }
    if (slash) {
        /* A trailing slash carries no prefix; keeping "/" would double the
         * separator in every composed request line. */
        if (slash[1] == '\0') out->base[0] = '\0';
        else octop_copy_text(out->base, sizeof(out->base), slash);
    } else out->base[0] = '\0';
    return true;
}

void octop_response_free(octop_response_t *resp)
{
    if (!resp) return;
    free(resp->body);
    resp->body = NULL;
    resp->size = 0;
}

static char *body_alloc(size_t cap)
{
    if (cap < 256) cap = 256;
    if (cap > BODY_MAX) cap = BODY_MAX;
    /* Responses are transient and can be large; keep them out of internal RAM,
     * which the Wi-Fi and LVGL paths need for their DMA and glyph budgets. */
    char *p = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(cap + 1);
    if (p) p[0] = '\0';
    return p;
}

static esp_err_t request(const octop_host_t *host, esp_http_client_method_t method,
    const char *path, const char *body, size_t body_size,
    const char *content_type, const char *bearer, uint32_t timeout_ms,
    size_t body_cap, octop_response_t *out)
{
    const char *name = method == HTTP_METHOD_GET ? "GET" : "POST";
    char url[OCTOP_HOST_CAP + OCTOP_PATH_CAP * 2 + 24];
    int n = snprintf(url, sizeof(url), "%s://%s:%u%s%s",
                     host->tls ? "https" : "http", host->host, (unsigned)host->port,
                     host->base, path);
    if (n < 0 || (size_t)n >= sizeof(url)) return ESP_ERR_INVALID_SIZE;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = (int)timeout_ms,
        .buffer_size = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    if (bearer) {
        /* Only the link task ever issues requests, so the JWT-length header
         * buffer stays out of the 8 KiB stack it runs on. */
        static char value[OCTOP_TOKEN_CAP + 16];
        snprintf(value, sizeof(value), "Bearer %s", bearer);
        esp_http_client_set_header(client, "Authorization", value);
    }
    if (content_type) esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Accept", "application/json");

    out->body = NULL;
    out->size = 0;
    out->status = 0;
    if (body_cap < 256) body_cap = 256;
    if (body_cap > BODY_MAX) body_cap = BODY_MAX;
    /* esp_http_client_perform switches off body caching while it fetches the
     * headers because it expects an event handler to consume the response.
     * Without one, every byte that arrives in the same TCP segment as the
     * status line is parsed and dropped, so a small fast reply (the 409-byte
     * login token) would reach esp_http_client_read as an empty body.
     * open/write/fetch_headers is the sequence that keeps those bytes. */
    esp_err_t err = esp_http_client_open(client, body ? (int)body_size : 0);
    if (err == ESP_OK && body && body_size &&
        esp_http_client_write(client, body, (int)body_size) < 0)
        err = ESP_FAIL;
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s failed: %s", name, path, esp_err_to_name(err));
        goto done;
    }
    out->status = esp_http_client_get_status_code(client);
    char *buffer = body_alloc(body_cap);
    if (!buffer) { err = ESP_ERR_NO_MEM; goto done; }
    size_t used = 0;
    for (;;) {
        int got = esp_http_client_read(client, buffer + used, (int)(body_cap - used));
        if (got <= 0) break;
        used += (size_t)got;
        if (used == body_cap) break;
    }
    buffer[used] = '\0';
    out->body = buffer;
    out->size = used;
done:
    esp_http_client_cleanup(client);
    if (err != ESP_OK) { free(out->body); out->body = NULL; out->size = 0; }
    return err;
}

esp_err_t octop_http_json(const octop_host_t *host, const char *method,
    const char *path, const char *body, const char *bearer,
    uint32_t timeout_ms, size_t body_cap, octop_response_t *out)
{
    if (!host || !method || !path || !out) return ESP_ERR_INVALID_ARG;
    if (!body_cap) body_cap = BODY_DEFAULT;
    esp_http_client_method_t verb = !strcmp(method, "GET") ? HTTP_METHOD_GET : HTTP_METHOD_POST;
    return request(host, verb, path, body, body ? strlen(body) : 0,
                   body ? "application/json" : NULL, bearer, timeout_ms,
                   body_cap, out);
}

/* ---------------- JSON string reader ---------------- */
static const char *find_member(const char *json, const char *key)
{
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        /* Require a real "key": boundary, not a match inside a longer name. */
        bool left_ok = p == json || p[-1] == '"' || p[-1] == '{' || p[-1] == ',' ||
                       p[-1] == ' ' || p[-1] == '\n' || p[-1] == '\t' || p[-1] == '\r';
        const char *after = p + klen;
        bool right_ok = *after == '"' && (after[1] == ':' || after[1] == ' ' ||
                                          after[1] == '\t');
        if (left_ok && right_ok) {
            ++after;
            while (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r') ++after;
            if (*after == ':') return after + 1;
            /* "key" : "value" with the colon before the closing quote. */
            const char *colon = strchr(after, ':');
            if (colon && !strchr(after, '"')) return colon + 1;
        }
        p = after;
    }
    return NULL;
}

static unsigned hex4(const char *p)
{
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
        char c = p[i];
        value <<= 4;
        if (c >= '0' && c <= '9') value |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') value |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= (unsigned)(c - 'A' + 10);
        else return 0xFFFFFFFFu;
    }
    return value;
}

bool octop_json_string(const char *json, const char *key, char *out, size_t cap)
{
    if (!json || !key || !out || cap < 2) return false;
    const char *p = find_member(json, key);
    if (!p) return false;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != '"') return false;
    ++p;
    size_t used = 0;
    while (*p) {
        if (*p == '"') break;
        if (*p != '\\' && used + 1 >= cap) return false;
        if (*p == '\\') {
            ++p;
            char c = *p++;
            char decoded;
            switch (c) {
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            case 'u': {
                unsigned cp = hex4(p);
                if (cp > 0xFFFFu) return false;
                p += 4;
                /* Surrogate pairs would need the following escape; the fields
                 * this device reads (ids, tokens, transcripts) never carry any. */
                if (cp >= 0xD800u && cp <= 0xDFFFu) cp = '?';
                char *d = out + used;
                size_t left = cap - used;
                size_t n;
                if (cp < 0x80u) { d[0] = (char)cp; n = 1; }
                else if (cp < 0x800u) {
                    if (left < 3) return false;
                    d[0] = (char)(0xC0u | (cp >> 6));
                    d[1] = (char)(0x80u | (cp & 0x3Fu));
                    n = 2;
                } else {
                    if (left < 4) return false;
                    d[0] = (char)(0xE0u | (cp >> 12));
                    d[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    d[2] = (char)(0x80u | (cp & 0x3Fu));
                    n = 3;
                }
                used += n;
                continue;
            }
            default: return false;
            }
            if (used + 1 >= cap) return false;
            out[used++] = decoded;
            continue;
        }
        out[used++] = *p++;
    }
    out[used] = '\0';
    return used > 0;
}

/* ---------------- login and bearer cache ---------------- */
static char token[OCTOP_TOKEN_CAP];

bool octop_login(const octop_host_t *host, const char *user, const char *pass,
    char *detail, size_t detail_cap)
{
    octop_bearer_invalidate();
    char escaped_user[64 * 2 + 1], escaped_pass[128 * 2 + 1];
    /* Credentials come from a form, so quotes and backslashes must not be able
     * to break out of the JSON body we assemble here. */
    struct { const char *src; char *dst; size_t cap; } items[] = {
        {user, escaped_user, sizeof(escaped_user)},
        {pass, escaped_pass, sizeof(escaped_pass)}};
    for (unsigned i = 0; i < 2; ++i) {
        size_t out = 0;
        for (const char *s = items[i].src; *s; ++s) {
            const char *rep = NULL;
            if (*s == '"') rep = "\\\"";
            else if (*s == '\\') rep = "\\\\";
            else if (*s == '\n') rep = "\\n";
            else if (*s == '\r') rep = "\\r";
            else if (*s == '\t') rep = "\\t";
            if (rep) {
                if (out + 2 >= items[i].cap) return false;
                items[i].dst[out++] = rep[0];
                items[i].dst[out++] = rep[1];
            } else {
                if (out + 1 >= items[i].cap) return false;
                items[i].dst[out++] = *s;
            }
        }
        items[i].dst[out] = '\0';
    }
    char body[512];
    snprintf(body, sizeof(body), "{\"username\":\"%s\",\"password\":\"%s\"}",
             escaped_user, escaped_pass);
    octop_response_t resp = {0};
    esp_err_t err = octop_http_json(host, "POST", "/api/auth/login", body, NULL,
                                    15000, 4096, &resp);
    if (err != ESP_OK) {
        snprintf(detail, detail_cap, "网络不可达");
        return false;
    }
    if (resp.status != 200 || !resp.body ||
        !octop_json_string(resp.body, "access_token", token, sizeof(token))) {
        ESP_LOGW(TAG, "login reply status=%ld size=%u body=%.*s", resp.status,
                 (unsigned)resp.size, (int)(resp.size < 160 ? resp.size : 160),
                 resp.body ? resp.body : "");
        snprintf(detail, detail_cap, "登录失败 %ld", resp.status);
        octop_response_free(&resp);
        return false;
    }
    octop_response_free(&resp);
    snprintf(detail, detail_cap, "已登录");
    ESP_LOGI(TAG, "login ok");
    return true;
}

const char *octop_bearer(void) { return token[0] ? token : NULL; }
void octop_bearer_invalidate(void) { token[0] = '\0'; }

/* ---------------- speech upload ---------------- */
static void wav_header(char out[44], uint32_t data_size)
{
    const uint32_t byte_rate = 16000 * 2;
    const uint32_t riff = 36 + data_size;
    const uint32_t fmt = 16, rate = 16000;
    const uint16_t pcm = 1, channels = 1, align = 2, bits = 16;
    memcpy(out + 0, "RIFF", 4);
    memcpy(out + 4, &riff, 4);
    memcpy(out + 8, "WAVEfmt ", 8);
    memcpy(out + 16, &fmt, 4);
    memcpy(out + 20, &pcm, 2);
    memcpy(out + 22, &channels, 2);
    memcpy(out + 24, &rate, 4);
    memcpy(out + 28, &byte_rate, 4);
    memcpy(out + 32, &align, 2);
    memcpy(out + 34, &bits, 2);
    memcpy(out + 36, "data", 4);
    memcpy(out + 40, &data_size, 4);
}

esp_err_t octop_http_upload_wav(const octop_host_t *host, const char *path,
    const char *bearer, const char *field, const uint8_t *pcm, size_t pcm_size,
    const char *language, uint32_t timeout_ms, octop_response_t *out)
{
    if (!host || !path || !pcm || !field || !out) return ESP_ERR_INVALID_ARG;
    if (pcm_size > UPLOAD_CAP) return ESP_ERR_INVALID_SIZE;
    static const char BOUNDARY[] = "----OctopPassportBoundary";
    char preamble[256], trailer[192], header[44];
    int pn = snprintf(preamble, sizeof(preamble),
        "--%s\r\nContent-Disposition: form-data; name=\"%s\"; "
        "filename=\"speech.wav\"\r\nContent-Type: audio/wav\r\n\r\n",
        BOUNDARY, field);
    int tn = language
        ? snprintf(trailer, sizeof(trailer),
              "\r\n--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n"
              "%s\r\n--%s--\r\n", BOUNDARY, language, BOUNDARY)
        : snprintf(trailer, sizeof(trailer), "\r\n--%s--\r\n", BOUNDARY);
    if (pn <= 0 || tn <= 0 || (size_t)pn >= sizeof(preamble) ||
        (size_t)tn >= sizeof(trailer)) return ESP_ERR_INVALID_SIZE;
    wav_header(header, (uint32_t)pcm_size);

    size_t total = (size_t)pn + sizeof(header) + pcm_size + (size_t)tn;
    uint8_t *body = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) body = malloc(total);
    if (!body) return ESP_ERR_NO_MEM;
    size_t at = 0;
    memcpy(body + at, preamble, (size_t)pn); at += (size_t)pn;
    memcpy(body + at, header, sizeof(header)); at += sizeof(header);
    memcpy(body + at, pcm, pcm_size); at += pcm_size;
    memcpy(body + at, trailer, (size_t)tn); at += (size_t)tn;

    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s",
             BOUNDARY);
    esp_err_t err = request(host, HTTP_METHOD_POST, path, (const char *)body, total,
                            content_type, bearer, timeout_ms, 4096, out);
    free(body);
    return err;
}
