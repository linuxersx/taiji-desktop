// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_LAUNCHER_H
#define TAIJI_LAUNCHER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "apps.h"
#include "config.h"
#include "model.h"

typedef struct {
    pid_t pid;
    int64_t started_at;
    bool active;
    bool external;
    bool game_session;
    char *game_id;
    char *command;
} Launcher;

bool launcher_start(Launcher *l, const Config *cfg, const Platform *platform,
                    const Game *game, char **error);
bool launcher_entry_needs_terminal(const AppEntry *entry);
bool launcher_start_app(Launcher *l, const AppEntry *entry, char **error);
bool launcher_session_available(void);
/* Returns 1 when the process just exited, 0 while running, -1 when idle. */
int launcher_poll(Launcher *l, int64_t *elapsed_seconds);
void launcher_init(Launcher *l);
void launcher_free(Launcher *l);
char *launcher_build_command(const Config *cfg, const Platform *platform,
                             const Game *game);

#endif
