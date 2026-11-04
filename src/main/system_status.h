// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_SYSTEM_STATUS_H
#define TAIJI_SYSTEM_STATUS_H

#include <stdbool.h>

typedef struct {
    int wifi;     /* -1 unavailable/unknown, 0 disconnected, 1 connected */
    int battery;  /* -1 unknown/no battery, 0..100 */
    bool charging;
    int battery_health;          /* -1 unknown, 0..100 */
    char battery_health_label[32]; /* vendor health text, e.g. Good */
} SystemStatus;

void system_status_poll(SystemStatus *out);

#endif
