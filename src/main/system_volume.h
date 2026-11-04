// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_SYSTEM_VOLUME_H
#define TAIJI_SYSTEM_VOLUME_H

#include <stdbool.h>

typedef struct {
    bool available;
    int percent;      /* 0..100 */
    bool muted;
    char backend[32]; /* PipeWire / CoreAudio */
} SystemVolume;

void system_volume_init(SystemVolume *out);
void system_volume_poll(SystemVolume *out);
bool system_volume_set(SystemVolume *out, int percent);
bool system_volume_apply(SystemVolume *out, int percent);
bool system_volume_delta(SystemVolume *out, int delta);
bool system_volume_toggle_mute(SystemVolume *out);

#endif
