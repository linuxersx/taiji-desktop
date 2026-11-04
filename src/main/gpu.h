// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_GPU_H
#define TAIJI_GPU_H

#include <stdbool.h>
#include <stddef.h>

#define GPU_MODE_AUTO 0
#define GPU_MODE_SAVER 1
#define GPU_MODE_PERFORMANCE 2
#define GPU_MODE_NONE -1

typedef struct {
    bool available;
    bool modes[3];
    int mode;                 /* GPU_MODE_* or GPU_MODE_NONE */
    char name[64];            /* Mali-G31 / Adreno / AMD Radeon ... */
    char backend[64];         /* devfreq / kgsl / amdgpu */
    char detail[192];         /* backend detail shown in the card */

    /* devfreq */
    char devfreq_dir[384];
    char governor_path[512];
    char original_governor[64];
    char auto_governor[64];
    char saver_governor[64];
    char perf_governor[64];

    /* downstream KGSL power levels */
    char min_level_path[512];
    char max_level_path[512];
    int orig_min_level;
    int orig_max_level;
    int max_level;
} GpuState;

void gpu_init(GpuState *out);
void gpu_poll(GpuState *out);
bool gpu_set(GpuState *out, int mode, char *error, size_t error_size);
const char *gpu_mode_label(int mode);
const char *gpu_mode_hint(int mode);
int gpu_mode_count(const GpuState *state);
int gpu_mode_at(const GpuState *state, int index);

#endif
