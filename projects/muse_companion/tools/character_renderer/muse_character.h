// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#define MUSE_CHARACTER_SIDE 128
typedef enum {
    CHARACTER_IDLE,CHARACTER_LISTENING,CHARACTER_THINKING,CHARACTER_SPEAKING,
    CHARACTER_SLEEPY,CHARACTER_ERROR,CHARACTER_HAPPY,CHARACTER_BOOT,CHARACTER_MODE_COUNT
} muse_character_mode_t;
typedef struct {
    muse_character_mode_t mode;
    float t; /* seconds in the loop, 0 <= t < 4 */
    float level; /* 0..1; microphone or voice amplitude */
    float happy; /* 0..1; optional petting intensity */
} muse_character_pose_t;
void muse_character_render(const muse_character_pose_t *pose,uint32_t *pixels);
