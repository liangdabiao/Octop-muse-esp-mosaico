// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct companion_model_t *companion_model_handle_t;
/* COMP_RECEIVING keeps the ordinal of the old speaking pose: the GSP scene
 * still calls that artwork "speaking", the device no longer plays audio.
 * Ordinals carry no meaning; the UI maps each mode to an artwork pose.
 * COMP_STT is the gap between releasing the AI key and Octop returning the
 * transcript, which reads as a hang if it shares the thinking caption. */
typedef enum { COMP_IDLE, COMP_LISTENING, COMP_STT, COMP_THINKING,
    COMP_RECEIVING, COMP_SLEEPY, COMP_ERROR } companion_mode_t;
typedef enum { COMP_PRESS, COMP_RELEASE, COMP_CANCEL,
    COMP_SETTINGS, COMP_BACK, COMP_WAKE, COMP_VOLUME, COMP_BRIGHTNESS,
    COMP_PAGE_NEXT, COMP_PAGE_PREV, COMP_AUTO_PAGE_NEXT, COMP_LANGUAGE,
    COMP_SLEEP_TIMEOUT, COMP_PAGE_INTERVAL } companion_action_t;
enum { COMP_EFFECT_NONE=0, COMP_EFFECT_BEGIN=1, COMP_EFFECT_END=2,
    COMP_EFFECT_CANCEL=4, COMP_EFFECT_PREFS=8, COMP_EFFECT_PREVIEW=16 };
/* The reader pages through the whole answer, so the entire reply has to stay
 * resident. This cap is the link component's own REPLY_CAP: a smaller value
 * here would silently drop the tail of a long answer before it was ever
 * paginated. 64 KiB is about 95 pages of 220 hanzi. */
#define COMP_REPLY_CAP 65536U
/* The home card shows the transcript over two lines of about 21 hanzi; the
 * extra room is for ASCII, which the pixel budget may still clip. */
#define COMP_QUESTION_CAP 256U
typedef struct {
    /* Bumped by every mutator below. The UI compares this instead of the whole
     * struct, so a 64 KiB answer stays where it is on every idle tick. */
    uint32_t revision;
    companion_mode_t mode;
    uint32_t generation;
    bool settings, key_down, online, paired, asleep;
    unsigned volume, brightness, display_brightness, page;
    unsigned language, sleep_seconds, page_seconds;
    int battery;
    char reply[COMP_REPLY_CAP];
    char question[COMP_QUESTION_CAP];
    char error[128];
} companion_snapshot_t;
/* UI/render task owns this model; other tasks send bounded messages only. */
companion_model_handle_t companion_model_create(void);
void companion_model_delete(companion_model_handle_t model);
unsigned companion_model_action(companion_model_handle_t model,
    companion_action_t action, unsigned value);
void companion_model_tick(companion_model_handle_t model, uint32_t ms);
void companion_model_connection(companion_model_handle_t model, bool online,
    bool paired, int battery);
/* streaming=false marks the final page-set of the turn. */
void companion_model_reply(companion_model_handle_t model, uint32_t generation,
    const char *text, bool streaming);
/* Octop returned the transcript of the clip, so recognition is over and the
 * agent is now composing the answer. The transcript is kept so the reader can
 * see which question this turn is about to answer. */
void companion_model_heard(companion_model_handle_t model, uint32_t generation,
    const char *transcript);
void companion_model_error(companion_model_handle_t model, uint32_t generation,
    const char *message);
void companion_model_prefs(companion_model_handle_t model,unsigned volume,
    unsigned brightness);
void companion_model_options(companion_model_handle_t model, unsigned language,
    unsigned sleep_seconds, unsigned page_seconds);
void companion_model_snapshot(companion_model_handle_t model,
    companion_snapshot_t *out);
