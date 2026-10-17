// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "companion_model.h"
typedef enum { COMP_MSG_REPLY,COMP_MSG_ERROR,COMP_MSG_HEARD } companion_message_kind_t;
typedef struct {
    companion_message_kind_t kind;
    uint32_t generation;
    /* true while Octop is still streaming, false for the final text. */
    bool streaming;
    char text[COMP_REPLY_CAP];
} companion_message_t;
/* Render task only calls these bounded platform operations. No network IO
 * happens on that task. Each platform owns its queues. */
void companion_platform_tick(uint32_t ms);
void companion_platform_status(bool *online,bool *paired,int *battery,bool *key,bool *motion);
bool companion_platform_message(companion_message_t *out);
bool companion_platform_effect(unsigned effects,const companion_snapshot_t *snapshot);
void companion_platform_brightness(unsigned value);
/* Octop session health for the settings page. The state value is the link
 * component's own enum kept as a plain integer so the UI needs no Octop header;
 * the detail is the short human sentence the link recorded (may be empty). */
unsigned companion_platform_link_state(void);
void companion_platform_link_detail(char *out,size_t cap);
void companion_platform_load_prefs(companion_snapshot_t *prefs);
bool companion_platform_setup(const char **ssid,const char **password);
bool companion_platform_setup_request(void);
bool companion_platform_setup_request_failed(void);
