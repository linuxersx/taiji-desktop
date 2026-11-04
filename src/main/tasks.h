// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_TASKS_H
#define TAIJI_TASKS_H

#include <stdbool.h>

#define TASKS_MAX 32

typedef enum {
    TASK_KIND_TAIJI = 0,
    TASK_KIND_APP,
    TASK_KIND_MUSIC,
    TASK_KIND_EXTERNAL,
} TaskKind;

typedef struct {
    TaskKind kind;
    char kind_name[16];
    char id[96];        /* app id for managed tasks, con id for external */
    char title[192];
    char subtitle[192];
    char thumb[1024];
    long con_id;
    int pid;
    bool focused;
} TaskEntry;

typedef struct {
    TaskEntry items[TASKS_MAX];
    int count;
} TaskList;

/* Collect the current task list. Runs the taiji-tasks session helper when it is
 * installed and falls back to the managed app state otherwise. */
void tasks_refresh(TaskList *list);

#endif
