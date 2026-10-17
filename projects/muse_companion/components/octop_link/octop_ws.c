// SPDX-License-Identifier: Apache-2.0
// One long-lived chat WebSocket. Measured against the live Octop:
//   WS /api/agents/{agent}/chat/ws?token=<JWT>          (query token only;
//                                                       header/cookie auth 403)
//   send {"type":"user_turn","text":...,"thread_id":...}
//   recv {"type":"token","content":"…","node":"model","thread_id":"…"} xN
//        {"type":"done","thread_id":"…"}                 63 B, ends the turn
//        {"type":"state_snapshot",...}                   up to 21 KiB, growing
//                                                        with history
// A snapshot is never read, so oversized frames are dropped instead of being
// allowed to kill the link, and one connection serves turn after turn.
#include "octop_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "octop_ws";
enum {
    WS_BUFFER = 1024,          /* rx chunk; larger frames arrive in pieces */
    WS_FRAME_CAP = 2048,       /* anything above this is a state snapshot */
    WS_TASK_STACK = 8192,
    WS_URI_CAP = OCTOP_TOKEN_CAP + 1024,
    WS_HANDSHAKE_WAIT_MS = 15000
};

static struct {
    esp_websocket_client_handle_t client;
    octop_ws_handlers_t handlers;
    char frame[WS_FRAME_CAP];
    bool dropping, open;
} ws;

/* esp_websocket_register_events stores a plain esp_event_handler_t, so the
 * callback's argument types come from the event loop, not the websocket header. */
static void on_event(void *ctx, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)ctx; (void)base;
    esp_websocket_event_data_t *data = event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ws.open = true;
        ws.dropping = false;
        ws.frame[0] = '\0';
        ESP_LOGI(TAG, "chat socket up");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED: {
        ws.open = false;
        int status = data ? data->error_handle.esp_ws_handshake_status_code : 0;
        ESP_LOGW(TAG, "chat socket down (handshake %d)", status);
        if (ws.handlers.on_lost) ws.handlers.on_lost(ws.handlers.ctx, status);
        break;
    }
    case WEBSOCKET_EVENT_DATA: {
        if (data->op_code == WS_TRANSPORT_OPCODES_BINARY || data->data_len < 0) break;
        /* The first fragment already knows the total size, so a 21 KiB state
         * snapshot is skipped whole rather than reassembled and discarded. */
        if (data->payload_offset == 0) ws.dropping = data->payload_len > WS_FRAME_CAP;
        if (ws.dropping) break;
        size_t end = (size_t)data->payload_offset + (size_t)data->data_len;
        if (end >= sizeof(ws.frame)) { ws.dropping = true; break; }
        memcpy(ws.frame + data->payload_offset, data->data_ptr, (size_t)data->data_len);
        ws.frame[end] = '\0';
        if (data->fin && end == (size_t)data->payload_len) {
            char type[24], content[1024];
            type[0] = content[0] = '\0';
            if (octop_json_string(ws.frame, "type", type, sizeof(type))) {
                octop_json_string(ws.frame, "content", content, sizeof(content));
                if (ws.handlers.on_frame)
                    ws.handlers.on_frame(ws.handlers.ctx, type, content);
            }
            ws.frame[0] = '\0';
        }
        break;
    }
    default:
        break;
    }
}

bool octop_ws_start(const octop_host_t *host, const char *agent,
    const char *token, const octop_ws_handlers_t *handlers)
{
    if (!host || !agent || !token || !handlers) return false;
    octop_ws_stop();
    ws.handlers = *handlers;
    static char uri[WS_URI_CAP];
    int n = snprintf(uri, sizeof(uri),
                     "%s://%s:%u%s/api/agents/%s/chat/ws?token=%s",
                     host->tls ? "wss" : "ws", host->host, (unsigned)host->port,
                     host->base, agent, token);
    if (n < 0 || (size_t)n >= sizeof(uri)) return false;
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .disable_auto_reconnect = true,
        .buffer_size = WS_BUFFER,
        .task_stack = WS_TASK_STACK,
        .network_timeout_ms = 15000,
    };
    ws.client = esp_websocket_client_init(&cfg);
    if (!ws.client) return false;
    esp_websocket_register_events(ws.client, WEBSOCKET_EVENT_ANY, on_event, NULL);
    if (esp_websocket_client_start(ws.client) != ESP_OK) {
        esp_websocket_client_destroy(ws.client);
        ws.client = NULL;
        return false;
    }
    for (unsigned waited = 0; waited < WS_HANDSHAKE_WAIT_MS && !ws.open; waited += 100)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (!ws.open) {
        ESP_LOGW(TAG, "handshake did not complete");
        octop_ws_stop();
        return false;
    }
    return true;
}

bool octop_ws_open(void) { return ws.client && ws.open; }

/* Build {"type":"user_turn","thread_id":"…","text":"…"} or the cancel form.
 * The prompt can be tens of kilobytes, so the frame is sized on the heap. */
static bool send_frame(const char *thread, const char *text)
{
    if (!octop_ws_open()) return false;
    size_t cap = text ? strlen(text) * 2 + strlen(thread) + 64
                      : strlen(thread) + 48;
    char *frame = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) frame = malloc(cap);
    if (!frame) return false;
    size_t out = 0;
    bool ok = true;
    if (text) {
        out += (size_t)snprintf(frame + out, cap - out,
                                "{\"type\":\"user_turn\",\"thread_id\":\"%s\",\"text\":\"",
                                thread);
        for (const char *s = text; *s; ++s) {
            if (out + 6 >= cap) { ok = false; break; }
            if (*s == '"' || *s == '\\') frame[out++] = '\\';
            if (*s == '\n') { frame[out++] = '\\'; frame[out++] = 'n'; }
            else if (*s == '\r') { frame[out++] = '\\'; frame[out++] = 'r'; }
            else if (*s == '\t') { frame[out++] = '\\'; frame[out++] = 't'; }
            else if ((unsigned char)*s < 0x20) continue;
            else frame[out++] = *s;
        }
        if (ok) {
            memcpy(frame + out, "\"}", 3);
            out += 2;
        }
    } else {
        out = (size_t)snprintf(frame, cap, "{\"type\":\"cancel\",\"thread_id\":\"%s\"}",
                               thread);
        ok = out < cap;
    }
    int sent = ok ? esp_websocket_client_send_text(ws.client, frame, (int)out,
                                                   pdMS_TO_TICKS(5000)) : -1;
    free(frame);
    /* send_text returns the byte count it wrote, not ESP_OK, so a fully
     * delivered frame reads as a positive number here. */
    if (!ok || sent < 0 || (size_t)sent != out) {
        ESP_LOGW(TAG, "frame send failed (ok=%d sent=%d want=%u)", (int)ok, sent,
                 (unsigned)out);
        /* A rejected write means the transport already aborted the link; the
         * event callback may never fire, so close the flag here or the next
         * turn would keep pushing into a dead socket. */
        ws.open = false;
        return false;
    }
    return true;
}

bool octop_ws_send_turn(const char *thread, const char *text)
{
    return send_frame(thread, text);
}

bool octop_ws_send_cancel(const char *thread)
{
    return send_frame(thread, NULL);
}

void octop_ws_stop(void)
{
    if (!ws.client) return;
    esp_websocket_client_stop(ws.client);
    esp_websocket_client_destroy(ws.client);
    ws.client = NULL;
    ws.open = false;
    ws.dropping = false;
    ws.frame[0] = '\0';
}
