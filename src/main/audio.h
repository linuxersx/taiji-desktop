// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_AUDIO_H
#define TAIJI_AUDIO_H

#include <stdbool.h>

typedef enum {
    SND_NAV = 0,
    SND_NAV_STRONG,
    SND_ENTER,
    SND_BACK,
    SND_COUNT
} SoundId;

bool audio_init(void);
void audio_shutdown(void);
void audio_play(SoundId id);
void audio_set_enabled(bool enabled);
void audio_set_volume(int percent);
bool audio_enabled(void);

#endif
