// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_APPS_H
#define TAIJI_APPS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum {
    APP_MODE_AUTO = 0,
    APP_MODE_RESIDENT,
    APP_MODE_HANDOFF,
} AppMode;

typedef enum {
    APP_ON_RETURN_PAUSE = 0,
    APP_ON_RETURN_RUN,
    APP_ON_RETURN_CLOSE,
} AppReturn;

typedef enum {
    APP_BACKEND_AUTO = 0,
    APP_BACKEND_WAYLAND,
    APP_BACKEND_X11,
} AppBackend;

typedef enum {
    APP_TERMINAL_AUTO = 0,
    APP_TERMINAL_YES,
    APP_TERMINAL_NO,
} AppTerminal;

typedef struct {
    char id[64];
    char name[160];
    char command[1024];
    char icon[1024];
    char category[32];
    char desktop_file[1024];
    int mode;          /* AppMode */
    int on_return;     /* AppReturn */
    int backend;       /* AppBackend */
    bool heavy;
    bool background;
    bool startup;
    int terminal;      /* AppTerminal */
    bool terminal_custom;
    bool on_return_custom;
} AppEntry;

typedef struct {
    AppEntry apps[64];
    int count;
} AppList;

typedef struct {
    char foreground[16];   /* taiji | app | music */
    char app_id[64];
    char app_state[24];    /* launching/foreground/paused/background/closed/failed */
    char on_return[16];    /* pause/run/close */
    pid_t pid;
    pid_t pgid;
} AppRuntime;

void apps_init(AppList *list);
void apps_free(AppList *list);
void apps_load(AppList *list);
bool apps_save(const AppList *list);
int apps_add(AppList *list, const char *name, const char *command);
void apps_remove(AppList *list, int index);
AppEntry *apps_find(AppList *list, const char *id);
const AppEntry *apps_find_const(const AppList *list, const char *id);

const char *apps_mode_name(int mode);
const char *apps_return_name(int on_return);
const char *apps_backend_name(int backend);
const char *apps_category_id(int index);
const char *apps_category_label(int index);
int apps_category_count(void);
int apps_category_index(const char *id);
int apps_cycle_category(const char *id, int dir);
bool apps_category_matches(const AppEntry *entry, const char *category);
int apps_cycle_mode(int mode, int dir);
int apps_cycle_return(int on_return, int dir);
int apps_cycle_backend(int backend, int dir);

void apps_runtime_read(AppRuntime *rt);
bool apps_runtime_write(const AppRuntime *rt);
bool apps_runtime_alive(const AppRuntime *rt);
const char *apps_runtime_state_label(const AppRuntime *rt, const AppEntry *entry);
char *apps_runtime_path(void);
char *apps_log_path(const char *id);

#endif
