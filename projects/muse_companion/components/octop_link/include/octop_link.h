// SPDX-License-Identifier: Apache-2.0
// Octop device link: Wi-Fi STA, JWT login, push-to-talk STT and chat streaming.
// The app talks to this component only through these symbols.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* ---- Connection ---- */
typedef enum {
    OCTOP_NOT_SET,       /* no Octop URL or account configured */
    OCTOP_OFFLINE,       /* configured, Wi-Fi down */
    OCTOP_UNTESTED,
    OCTOP_TESTING,
    OCTOP_REACHABLE,     /* logged in, a thread is usable */
    OCTOP_UNREACHABLE,
} octop_state_t;
typedef struct { octop_state_t state; char detail[48]; } octop_status_t;

/* Starts the Wi-Fi and session tasks. Safe to call once per boot. */
esp_err_t octop_link_start(void);
bool octop_wifi_connected(void);
/* Wi-Fi up, credentials present and the last login/agent probe succeeded. */
bool octop_link_ready(void);
void octop_link_status(octop_status_t *out);
const char *octop_state_name(octop_state_t state);
/* Call after the portal writes new credentials (before reboot). */
void octop_link_config_changed(void);

/* ---- Push-to-talk turns (voice worker only) ---- */
/* Credentials present and Wi-Fi up: a press may start a turn. */
bool octop_turn_ready(void);
/* Press: start capturing speech into the PSRAM clip. */
void octop_turn_begin(void);
/* 16 kHz mono frames, in order, from press to release. */
void octop_turn_audio(const int16_t *pcm, size_t frames);
/* Release: the clip is complete; STT then chat run on the session task. */
void octop_turn_end(void);
/* Abandon: drop the clip, and cancel a running turn on the server. */
void octop_turn_cancel(void);

typedef enum {
    OCTOP_EV_NONE,
    OCTOP_EV_HEARD,   /* final transcript of the spoken question */
    OCTOP_EV_REPLY,   /* reply text so far */
    OCTOP_EV_DONE,    /* reply complete; read it with octop_reply_copy */
    OCTOP_EV_ERROR,   /* turn failed; the event text says why */
} octop_ev_t;
/* Non-blocking; copies the event text. Events of cancelled turns are dropped. */
octop_ev_t octop_turn_event(char *text, size_t cap);

/* ---- Typed turns (share the same transport, used by the Iris service) ---- */
typedef void (*octop_text_cb)(void *ctx, const char *event, const char *text);
/* event is "start" | "delta" | "done" | "error"; register before tasks start. */
void octop_text_observer(octop_text_cb cb, void *ctx);
bool octop_text_begin(const char *text);
/* Copy the current reply, up to cap-1 bytes; true when there is any. */
bool octop_reply_copy(char *out, size_t cap);
/* 0 = Chinese, 1 = English: sets the STT language hint. */
void octop_language(unsigned language);
