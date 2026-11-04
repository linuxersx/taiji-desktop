// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"
#include "util.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_AudioDeviceID g_device = 0;
static bool g_enabled = true;
static int g_volume = 60;
static Uint8 *g_clip[SND_COUNT];
static Uint32 g_clip_len[SND_COUNT];

static const char *CLIP_NAMES[SND_COUNT] = {
    "sounds/nav.wav", "sounds/nav_strong.wav", "sounds/enter.wav", "sounds/back.wav"
};

bool audio_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[taiji] audio unavailable: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_device) {
        fprintf(stderr, "[taiji] audio device unavailable: %s\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(g_device, 0);

    for (int i = 0; i < SND_COUNT; i++) {
        char *path = asset_path(CLIP_NAMES[i]);
        SDL_AudioSpec spec;
        Uint8 *buf = NULL;
        Uint32 len = 0;
        if (!SDL_LoadWAV(path, &spec, &buf, &len)) {
            fprintf(stderr, "[taiji] cannot load %s: %s\n", path, SDL_GetError());
            free(path);
            continue;
        }
        SDL_AudioCVT cvt;
        if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
                              have.format, have.channels, have.freq) < 0) {
            SDL_FreeWAV(buf);
            free(path);
            continue;
        }
        cvt.len = (int)len;
        cvt.buf = (Uint8 *)SDL_malloc((size_t)cvt.len * (size_t)cvt.len_mult);
        memcpy(cvt.buf, buf, len);
        SDL_FreeWAV(buf);
        if (SDL_ConvertAudio(&cvt) == 0) {
            g_clip[i] = cvt.buf;
            g_clip_len[i] = (Uint32)cvt.len_cvt;
        } else {
            SDL_free(cvt.buf);
        }
        free(path);
    }
    return true;
}

void audio_shutdown(void)
{
    if (g_device) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
    for (int i = 0; i < SND_COUNT; i++) {
        if (g_clip[i]) { SDL_free(g_clip[i]); g_clip[i] = NULL; }
        g_clip_len[i] = 0;
    }
}

void audio_play(SoundId id)
{
    if (!g_enabled || !g_device || id < 0 || id >= SND_COUNT || !g_clip[id]) return;
    if (SDL_GetQueuedAudioSize(g_device) > g_clip_len[id] * 3) {
        SDL_ClearQueuedAudio(g_device);
    }
    if (g_volume >= 100) {
        SDL_QueueAudio(g_device, g_clip[id], g_clip_len[id]);
    } else {
        int16_t *tmp = (int16_t *)malloc(g_clip_len[id]);
        if (tmp) {
            const int16_t *src = (const int16_t *)g_clip[id];
            size_t samples = g_clip_len[id] / sizeof(int16_t);
            for (size_t i = 0; i < samples; i++) tmp[i] = (int16_t)((int)src[i] * g_volume / 100);
            SDL_QueueAudio(g_device, tmp, g_clip_len[id]);
            free(tmp);
        }
    }
    SDL_PauseAudioDevice(g_device, 0);
}

void audio_set_enabled(bool enabled) { g_enabled = enabled; }
void audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volume = percent;
}
bool audio_enabled(void) { return g_enabled; }
