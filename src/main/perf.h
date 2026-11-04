// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_PERF_H
#define TAIJI_PERF_H

#include <stdbool.h>
#include <stddef.h>

#define PERF_MODE_SAVER 0
#define PERF_MODE_BALANCED 1
#define PERF_MODE_PERFORMANCE 2
#define PERF_MODE_NONE -1

typedef struct {
    bool available;
    bool modes[3];
    int mode;              /* PERF_MODE_* or PERF_MODE_NONE */
    char backend[64];      /* power-profiles-daemon / platform_profile / cpufreq */
    char detail[160];      /* human readable backend detail */
} PerfState;

void perf_init(PerfState *out);
void perf_poll(PerfState *out);
bool perf_set(PerfState *out, int mode, char *error, size_t error_size);
const char *perf_mode_label(int mode);
const char *perf_mode_hint(int mode);
int perf_mode_count(const PerfState *state);
int perf_mode_at(const PerfState *state, int index);

#endif
