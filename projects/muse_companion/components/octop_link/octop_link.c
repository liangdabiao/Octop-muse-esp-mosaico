// SPDX-License-Identifier: Apache-2.0
// The Octop session: keep Wi-Fi, login, agent, thread and one chat socket up,
// then run turns in order. Everything blocking (HTTP, STT, handshake) happens
// on this task; the app only appends audio frames and polls events.
#include "octop_link.h"
#include "octop_internal.h"
#include "octop_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "companion_credentials.h"

static const char *TAG = "octop_link";
enum {
    /* 30 s at 16 kHz mono 16-bit is 960 kB of PCM: this board has PSRAM and no
     * room for that in internal RAM, so the clip lives there. */
    CLIP_MAX = 30 * 16000 * 2,
    CLIP_MIN = 3200,
    REPLY_CAP = 65536,
    /* A spoken question is usually well under 100 bytes, but the transcript is
     * shown on screen, so it must not be cut at the same size as an error line. */
    EVENT_CAP = 256,
    FIRST_FRAME_WAIT_US = 45000000LL,
    TOKEN_IDLE_US = 60000000LL,
    BRING_UP_PERIOD_MS = 3000,
    AGENTS_BODY_CAP = 16384
};

typedef enum { JOB_VOICE, JOB_TEXT, JOB_CANCEL } job_kind_t;
typedef struct { job_kind_t kind; char *text; } link_job_t;
typedef struct { octop_ev_t kind; char text[EVENT_CAP]; } link_event_t;
typedef enum { TURN_IDLE, TURN_CHAT } turn_state_t;

struct link_t {
    QueueHandle_t jobs, events;
    SemaphoreHandle_t lock;
    int16_t *clip;
    char *reply;
    size_t clip_len, reply_len;
    volatile bool recording, cancelled;
    unsigned language;
    octop_state_t state;
    char detail[OCTOP_DETAIL_CAP];
    char agent[32], thread[48];
    bool have_agent, have_thread, token_stale;
    /* Set while an OCTOP_EV_REPLY is still sitting in the queue. The answer
     * text lives in l->reply, so the event is only a "go re-read it" ping and a
     * second one behind the first would ask the app to copy the whole reply
     * again for no new characters. */
    volatile bool reply_pending;
    turn_state_t turn;
    bool turn_is_text, saw_token;
    int64_t last_frame_us, next_bring_up_us;
    octop_ws_handlers_t handlers;
    octop_text_cb observer;
    void *observer_ctx;
};
static struct link_t link;

static void set_state(struct link_t *l, octop_state_t state, const char *detail)
{
    l->state = state;
    octop_copy_text(l->detail, sizeof(l->detail), detail ? detail : "");
}

const char *octop_state_name(octop_state_t state)
{
    switch (state) {
    case OCTOP_NOT_SET: return "not-set";
    case OCTOP_OFFLINE: return "offline";
    case OCTOP_UNTESTED: return "untested";
    case OCTOP_TESTING: return "testing";
    case OCTOP_REACHABLE: return "reachable";
    default: return "unreachable";
    }
}

static void push_event(struct link_t *l, octop_ev_t kind, const char *text)
{
    link_event_t e = {.kind = kind};
    octop_copy_text(e.text, sizeof(e.text), text ? text : "");
    if (xQueueSend(l->events, &e, 0) != pdTRUE)
        ESP_LOGW(TAG, "event queue full");
}

static void push_reply(struct link_t *l)
{
    link_event_t e = {.kind = OCTOP_EV_REPLY};
    if (l->reply_pending) return;
    if (xQueueSend(l->events, &e, 0) == pdTRUE) l->reply_pending = true;
    else ESP_LOGW(TAG, "event queue full");
}

static void notify_text(struct link_t *l, const char *event, const char *text)
{
    if (l->observer) l->observer(l->observer_ctx, event, text);
}

void octop_text_observer(octop_text_cb cb, void *ctx)
{
    link.observer = cb;
    link.observer_ctx = ctx;
}

void octop_language(unsigned language) { link.language = language ? 1 : 0; }

/* ---------------- thread identity ----------------
 * The thread id is the only conversation anchor the server keeps, so exactly
 * one is stored in NVS and reused forever; creating one per turn would grow
 * the server's history and slow every answer down. The key name changed once
 * ("thread" -> "chat_thread") to drop the ids saved from the thread list,
 * which pointed at somebody else's dashboard conversation. */
static void thread_load(struct link_t *l)
{
    nvs_handle_t h;
    if (nvs_open("companion_octop", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(l->thread);
    if (nvs_get_str(h, "chat_thread", l->thread, &n) == ESP_OK && l->thread[0])
        l->have_thread = true;
    nvs_close(h);
}

static void thread_store(struct link_t *l)
{
    nvs_handle_t h;
    if (nvs_open("companion_octop", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, "chat_thread", l->thread) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void thread_forget(struct link_t *l)
{
    l->have_thread = false;
    l->thread[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("companion_octop", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "chat_thread");
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------------- link bring-up ---------------- */
static bool probe_agent(struct link_t *l, const octop_host_t *host)
{
    octop_response_t resp = {0};
    esp_err_t err = octop_http_json(host, "GET", "/api/agents", NULL, octop_bearer(),
                                    15000, AGENTS_BODY_CAP, &resp);
    bool ok = err == ESP_OK && resp.status == 200 && resp.body &&
              octop_json_string(resp.body, "agent_id", l->agent, sizeof(l->agent));
    if (ok) l->have_agent = true;
    else {
        ESP_LOGW(TAG, "agents probe status=%ld err=%s", resp.status,
                 err == ESP_OK ? "ok" : esp_err_to_name(err));
        /* A rejected bearer is not a network fault: the JWT aged out (24 h) or
         * the password changed, and only a fresh login can fix it. Without this
         * the retry loop would knock on a 401 door forever. */
        if (resp.status == 401 || resp.status == 403) {
            l->token_stale = true;
            set_state(l, OCTOP_UNREACHABLE, "登录已过期，正在重新登录");
        } else set_state(l, OCTOP_UNREACHABLE, "没有可用智能体");
    }
    octop_response_free(&resp);
    return ok;
}

static bool probe_thread(struct link_t *l, const octop_host_t *host)
{
    if (l->have_thread && l->thread[0]) return true;
    char path[128];
    snprintf(path, sizeof(path), "/api/agents/%s/threads", l->agent);
    /* Always create the conversation here. Listing threads used to be the fast
     * path, but it hands the companion whatever dashboard thread the web UI
     * touched last, and the agent then answers that unrelated history instead
     * of what was just spoken into the microphone. */
    octop_response_t made = {0};
    bool ok = octop_http_json(host, "POST", path, "{}", octop_bearer(), 15000,
                              4096, &made) == ESP_OK &&
              made.status == 201 && made.body &&
              octop_json_string(made.body, "thread_id", l->thread, sizeof(l->thread));
    if (ok) {
        l->have_thread = true;
        thread_store(l);
        ESP_LOGI(TAG, "created thread %s", l->thread);
    } else {
        ESP_LOGW(TAG, "thread probe status=%ld", made.status);
        if (made.status == 401 || made.status == 403) {
            l->token_stale = true;
            set_state(l, OCTOP_UNREACHABLE, "登录已过期，正在重新登录");
        } else set_state(l, OCTOP_UNREACHABLE, "无法建立会话");
    }
    octop_response_free(&made);
    return ok;
}

/* Everything a turn needs: credentials, radio, JWT, agent, thread, socket. */
static bool bring_up(struct link_t *l)
{
    if (!companion_credentials_ready()) {
        set_state(l, OCTOP_NOT_SET, NULL);
        return false;
    }
    if (!octop_wifi_connected()) {
        set_state(l, OCTOP_OFFLINE, NULL);
        return false;
    }
    octop_host_t host;
    if (!octop_url_split(companion_credentials_octop_url(), &host)) {
        set_state(l, OCTOP_UNREACHABLE, "地址无效");
        return false;
    }
    if (!octop_bearer() || l->token_stale) {
        l->token_stale = false;
        set_state(l, OCTOP_TESTING, "正在登录");
        char detail[OCTOP_DETAIL_CAP];
        if (!octop_login(&host, companion_credentials_username(),
                         companion_credentials_password(), detail, sizeof(detail))) {
            set_state(l, OCTOP_UNREACHABLE, detail);
            return false;
        }
        l->have_agent = false;
    }
    if (!l->have_agent && !probe_agent(l, &host)) return false;
    if (!probe_thread(l, &host)) return false;
    if (!octop_ws_open()) {
        set_state(l, OCTOP_TESTING, "正在连接对话");
        if (!octop_ws_start(&host, l->agent, octop_bearer(), &l->handlers)) {
            set_state(l, OCTOP_UNREACHABLE, "对话通道连不上");
            return false;
        }
    }
    set_state(l, OCTOP_REACHABLE, NULL);
    return true;
}

/* ---------------- frames ---------------- */
static void finish_turn(struct link_t *l, const char *error);

static void append_reply(struct link_t *l, const char *text)
{
    size_t n = strlen(text);
    xSemaphoreTake(l->lock, portMAX_DELAY);
    if (l->reply_len + n < REPLY_CAP) {
        memcpy(l->reply + l->reply_len, text, n);
        l->reply_len += n;
        l->reply[l->reply_len] = '\0';
    } else if (l->reply_len + 4 < REPLY_CAP) {
        /* Truncate rather than fail: a long answer is still worth reading, and
         * the ellipsis tells the reader the tail is missing. */
        memcpy(l->reply + l->reply_len, "…", 3);
        l->reply_len += 3;
        l->reply[l->reply_len] = '\0';
    }
    xSemaphoreGive(l->lock);
}

static void on_frame(void *ctx, const char *type, const char *content)
{
    struct link_t *l = ctx;
    int64_t now = esp_timer_get_time();
    if (!strcmp(type, "done")) {
        /* The server always closes a turn with `done`, even after `cancel`
         * (measured 165 ms later), so this is the only correct end of stream. */
        if (l->turn == TURN_CHAT) finish_turn(l, NULL);
        return;
    }
    if (!strcmp(type, "error")) {
        if (l->turn == TURN_CHAT)
            finish_turn(l, l->turn_is_text ? (content && content[0] ? content : "agent error")
                : "Octop 拒绝了这次请求");
        return;
    }
    if (!strcmp(type, "reasoning")) {
        if (l->turn == TURN_CHAT) l->last_frame_us = now;
        return;
    }
    if (strcmp(type, "token")) return;      /* usage and state frames: unused */
    if (l->turn != TURN_CHAT || l->cancelled || !content || !content[0]) return;
    l->last_frame_us = now;
    if (!l->saw_token) {
        l->saw_token = true;
        notify_text(l, "start", "");
    }
    append_reply(l, content);
    if (l->turn_is_text) notify_text(l, "delta", content);
    else push_reply(l);
}

static void on_lost(void *ctx, int handshake_status)
{
    struct link_t *l = ctx;
    /* A 403 handshake means the JWT is no longer accepted (24 h expiry or a
     * password change) and a network retry cannot fix it: log in again. A 404
     * means the stored thread is gone, so a new one must be made. */
    if (handshake_status == 403) l->token_stale = true;
    if (handshake_status == 404) thread_forget(l);
    if (l->turn == TURN_CHAT) {
        bool was_text = l->turn_is_text;
        l->turn = TURN_IDLE;
        l->saw_token = false;
        if (l->cancelled) l->cancelled = false;
        else if (was_text) notify_text(l, "error", "connection lost");
        else push_event(l, OCTOP_EV_ERROR, "与 Octop 的连接中断");
    }
}

/* ---------------- turns ---------------- */
static void finish_turn(struct link_t *l, const char *error)
{
    bool was_text = l->turn_is_text, silenced = l->cancelled;
    l->turn = TURN_IDLE;
    l->saw_token = false;
    l->cancelled = false;
    if (silenced) {
        if (was_text) notify_text(l, "done", "");
        return;
    }
    if (error) {
        if (was_text) notify_text(l, "error", error);
        else push_event(l, OCTOP_EV_ERROR, error);
        return;
    }
    if (was_text) notify_text(l, "done", "");
    else push_event(l, OCTOP_EV_DONE, "");
}

static void start_chat(struct link_t *l, const char *question, bool from_text)
{
    xSemaphoreTake(l->lock, portMAX_DELAY);
    l->reply_len = 0;
    l->reply[0] = '\0';
    xSemaphoreGive(l->lock);
    l->reply_pending = false;
    l->turn_is_text = from_text;
    l->saw_token = false;
    l->cancelled = false;
    l->turn = TURN_CHAT;
    l->last_frame_us = esp_timer_get_time();
    if (!octop_ws_send_turn(l->thread, question))
        finish_turn(l, from_text ? "send failed" : "问题没有发出去，请重试");
}

/* A failed bring-up already recorded the precise reason, except for the two
 * states the state name itself describes. */
static const char *turn_error(struct link_t *l)
{
    if (l->detail[0]) return l->detail;
    switch (l->state) {
    case OCTOP_NOT_SET: return "还没有配置服务，请先用手机完成配置";
    case OCTOP_OFFLINE: return "网络未连接，请稍后重试";
    default: return "Octop 连不上，请检查配置";
    }
}

static void run_job(struct link_t *l, link_job_t *job)
{
    if (job->kind == JOB_CANCEL) {
        if (l->turn == TURN_CHAT) octop_ws_send_cancel(l->thread);
        else finish_turn(l, NULL);
        return;
    }
    if (job->kind == JOB_VOICE && l->clip_len < CLIP_MIN) {
        push_event(l, OCTOP_EV_ERROR, "没有听到声音，请按住再说");
        return;
    }
    /* The session has to be up before anything that carries the bearer, and the
     * speech upload is one of those: transcribing first with an aged-out JWT
     * spends two 25 s timeouts on a request the server rejects at once. */
    if (!bring_up(l)) {
        free(job->text);
        push_event(l, OCTOP_EV_ERROR, turn_error(l));
        return;
    }
    char *question = NULL;
    bool from_text = job->kind == JOB_TEXT;
    if (from_text) question = job->text;
    else {
        char detail[OCTOP_DETAIL_CAP];
        set_state(l, OCTOP_TESTING, "正在识别语音");
        size_t size = l->clip_len;
        l->clip_len = 0;
        question = octop_stt_transcribe((const uint8_t *)l->clip, size, l->language,
                                       detail, sizeof(detail));
        if (!question) {
            push_event(l, OCTOP_EV_ERROR, detail[0] ? detail : "语音识别失败");
            return;
        }
        push_event(l, OCTOP_EV_HEARD, question);
    }
    start_chat(l, question, from_text);
    /* Both kinds own their string here: the transcript is heap from the STT
     * call, the typed prompt is the heap copy the job carried. */
    free(question);
}

static void watch_turn(struct link_t *l)
{
    if (l->turn != TURN_CHAT) return;
    /* Proof of life is any frame, not answer text: the live server put 176
     * `reasoning` frames in front of a one-line answer, and a hard wait from
     * the turn start aborted healthy turns mid-thought. A half-open socket
     * sends nothing at all, and that is what this wait has to catch. */
    int64_t silence = l->saw_token ? TOKEN_IDLE_US : FIRST_FRAME_WAIT_US;
    if (esp_timer_get_time() - l->last_frame_us > silence) {
        octop_ws_send_cancel(l->thread);
        finish_turn(l, l->turn_is_text ? (l->saw_token ? "stream stopped" : "no answer")
            : (l->saw_token ? "回答中断，请重试" : "等了 45 秒没有回答，请重试"));
    }
}

static void link_task(void *arg)
{
    struct link_t *l = arg;
    thread_load(l);
    bring_up(l);
    l->next_bring_up_us = esp_timer_get_time();
    for (;;) {
        link_job_t job;
        if (xQueueReceive(l->jobs, &job, pdMS_TO_TICKS(100)) == pdTRUE) run_job(l, &job);
        watch_turn(l);
        if (l->turn == TURN_IDLE && esp_timer_get_time() > l->next_bring_up_us) {
            l->next_bring_up_us = esp_timer_get_time() +
                (bring_up(l) ? BRING_UP_PERIOD_MS * 4 : BRING_UP_PERIOD_MS) * 1000LL;
        }
    }
}

/* ---------------- public surface ---------------- */
esp_err_t octop_link_start(void)
{
    struct link_t *l = &link;
    l->handlers = (octop_ws_handlers_t){
        .on_frame = on_frame, .on_lost = on_lost, .ctx = l};
    l->jobs = xQueueCreate(2, sizeof(link_job_t));
    l->events = xQueueCreate(6, sizeof(link_event_t));
    l->lock = xSemaphoreCreateMutex();
    l->clip = heap_caps_malloc(CLIP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    l->reply = heap_caps_calloc(1, REPLY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!l->jobs || !l->events || !l->lock || !l->clip || !l->reply)
        return ESP_ERR_NO_MEM;
    set_state(l, OCTOP_UNTESTED, "启动中");
    if (octop_wifi_start() != ESP_OK)
        ESP_LOGW(TAG, "no saved Wi-Fi network yet");
    if (xTaskCreate(link_task, "octop_link", 8192, l, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool octop_link_ready(void) { return link.state == OCTOP_REACHABLE; }

void octop_link_status(octop_status_t *out)
{
    if (!out) return;
    out->state = link.state;
    octop_copy_text(out->detail, sizeof(out->detail), link.detail);
}

bool octop_turn_ready(void)
{
    return companion_credentials_ready() && octop_wifi_connected();
}

void octop_turn_begin(void)
{
    link.cancelled = false;
    link.clip_len = 0;
    link.recording = true;
}

void octop_turn_audio(const int16_t *pcm, size_t frames)
{
    if (!link.recording || !pcm || !frames) return;
    size_t bytes = frames * sizeof(int16_t);
    if (link.clip_len + bytes > CLIP_MAX) {
        link.recording = false;         /* the 30 s ceiling is reached */
        return;
    }
    memcpy((uint8_t *)link.clip + link.clip_len, pcm, bytes);
    link.clip_len += bytes;
}

void octop_turn_end(void)
{
    link.recording = false;
    link_job_t job = {.kind = JOB_VOICE};
    if (xQueueSend(link.jobs, &job, pdMS_TO_TICKS(50)) != pdTRUE)
        ESP_LOGW(TAG, "job queue full");
}

void octop_turn_cancel(void)
{
    link.recording = false;
    link.cancelled = true;
    link_job_t job = {.kind = JOB_CANCEL};
    if (xQueueSend(link.jobs, &job, 0) != pdTRUE) ESP_LOGW(TAG, "cancel dropped");
}

octop_ev_t octop_turn_event(char *text, size_t cap)
{
    link_event_t e;
    if (xQueueReceive(link.events, &e, 0) != pdTRUE) return OCTOP_EV_NONE;
    /* The flag only describes the queue slot. Whether the app acts on this
     * ping or throws it away as stale, the slot is free from now on. */
    if (e.kind == OCTOP_EV_REPLY) link.reply_pending = false;
    if (link.cancelled && e.kind != OCTOP_EV_ERROR) {
        /* Events from an abandoned turn are stale by definition. */
        return OCTOP_EV_NONE;
    }
    octop_copy_text(text, cap, e.text);
    return e.kind;
}

bool octop_text_begin(const char *text)
{
    if (!text || !text[0]) return false;
    char *copy = strdup(text);
    if (!copy) return false;
    link_job_t job = {.kind = JOB_TEXT, .text = copy};
    if (xQueueSend(link.jobs, &job, pdMS_TO_TICKS(200)) != pdTRUE) {
        free(copy);
        return false;
    }
    return true;
}

bool octop_reply_copy(char *out, size_t cap)
{
    if (!cap) return false;
    xSemaphoreTake(link.lock, portMAX_DELAY);
    size_t n = link.reply_len;
    if (n >= cap) n = cap - 1;
    memcpy(out, link.reply, n);
    out[n] = '\0';
    bool any = link.reply_len > 0;
    xSemaphoreGive(link.lock);
    return any;
}

void octop_link_config_changed(void)
{
    octop_ws_stop();
    thread_forget(&link);
    link.have_agent = false;
    link.token_stale = false;
    set_state(&link, OCTOP_UNTESTED, "配置已变更");
}
