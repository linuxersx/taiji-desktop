// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_SMB_SHARE_H
#define TAIJI_SMB_SHARE_H

#include "app.h"

#include <stdbool.h>
#include <stddef.h>

/* LAN discovery (background thread; sets smb_ui.scan_done). */
void smb_scan_start(App *app);
/* List the shares of one host (background thread; sets smb_ui.shares_done). */
void smb_fetch_shares(App *app, const char *addr);

/* Local samba management through the root helper. */
void smb_local_refresh(App *app);
bool smb_local_add(App *app, const char *name, const char *path, bool writable,
                   char *err, size_t err_size);
bool smb_local_set(App *app, const char *name, const char *path, bool writable,
                   char *err, size_t err_size);
bool smb_local_remove(App *app, const char *name, char *err, size_t err_size);
bool smb_service_set(App *app, bool on, char *err, size_t err_size);
void smb_service_query(App *app);

#endif
