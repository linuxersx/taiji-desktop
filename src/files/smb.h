// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_SMB_H
#define TAIJI_SMB_H

#include <stdbool.h>
#include <stddef.h>

/* Mount an SMB share through GVFS and return its local FUSE path. */
bool smb_mount_uri(const char *uri, const char *user, const char *password,
                   char *out_path, size_t out_size,
                   char *err, size_t err_size);

#endif
