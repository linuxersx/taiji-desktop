// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_BLUETOOTH_H
#define TAIJI_BLUETOOTH_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char address[24];
    char name[128];
    bool connected;
    bool paired;
} BtDevice;

typedef struct {
    BtDevice items[64];
    int count;
    bool enabled;
    bool available;
    char controller[128];
    char message[256];
} BtState;

/* Quick radio/adapter detection; keeps the existing device list. */
bool bluetooth_radio(BtState *state);
void bluetooth_scan(BtState *state, bool rescan);
void bluetooth_scan_async(bool rescan);
bool bluetooth_scan_take(BtState *state);
bool bluetooth_toggle(bool enabled, char *error, size_t error_size);
void bluetooth_connect_async(const char *address);
bool bluetooth_action_take(char *message, size_t message_size);

#endif
