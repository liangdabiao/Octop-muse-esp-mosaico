// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "led_status.h"
bool muse_port_initialized(void);
bool muse_port_link_ready(void);
led_state_t muse_port_state(void);
bool muse_companion_reply_copy(char *out, size_t cap);
/* Voice-worker only; sets the natural-language instruction for the next turn. */
void muse_companion_language(unsigned language);
typedef void (*muse_companion_text_cb)(void *ctx,const char *event,
    const char *message_id,const char *text,const char *raw,size_t raw_size);
/* Register before SDK tasks start; callback runs under the chat receive lock. */
void muse_companion_text_observer(muse_companion_text_cb cb,void *ctx);
void muse_companion_text_observe(const char *event,const char *id,const char *text,
    const char *raw,size_t raw_size);
/* Voice-worker only; shares the same authenticated transport as voice turns. */
bool muse_companion_text_begin(const char *text);
