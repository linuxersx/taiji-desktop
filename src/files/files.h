// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_FILES_H
#define TAIJI_FILES_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <limits.h>

#include "backend.h"
#include <lvgl.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef enum {
    FM_MANAGER = 0,
    FM_OPEN,
    FM_FOLDER,
    FM_SAVE,
} FilesMode;

typedef struct {
    char name[256];
    bool is_dir;
    off_t size;
    time_t mtime;
} FsEntry;

bool files_is_archive(const char *name);

typedef struct {
    FsEntry *items;
    int count;
} FsList;

typedef struct FilesApp {
    Backend backend;
    FilesMode mode;
    bool multiple;
    char cwd[PATH_MAX];
    char title[128];
    char output[PATH_MAX];
    char save_name[256];
    bool show_hidden;
    FsList list;
    int focus;
    bool *marked;
    int marked_count;
    char **clip;
    int clip_count;
    bool clip_cut;
    char clip_dir[PATH_MAX];
    bool running;
    int exit_code;
    char message[256];

    lv_obj_t *root;
    lv_obj_t *topbar;
    lv_obj_t *path_label;
    lv_obj_t *mode_label;
    lv_obj_t *listbox;
    lv_obj_t *status_label;
    lv_obj_t *footer;
    lv_obj_t **rows;
    lv_obj_t **row_marks;
    int row_count;

    lv_obj_t *modal;
    lv_obj_t *modal_panel;
    int modal_kind; /* 0 none, 1 menu, 2 text, 3 confirm */
    int modal_focus;
    int modal_action;

    lv_obj_t *menu_rows[12];
    const char *menu_items[12];
    int menu_actions[12];
    char menu_storage[12][160];
    int menu_count;

    char input[512];
    int input_len;
    char prompt[160];
    int text_action; /* 1 new folder, 2 rename, 3 save name, 4 path */
    int kb_focus;
    lv_obj_t *kb_keys[64];
    int kb_count;

    volatile bool op_running;
    volatile bool op_done;
    volatile int op_result;
    pid_t op_pid;
    char op_label[256];
    int op_action; /* for refresh after */
    char pending_path[PATH_MAX];
    char shortcut_paths[16][PATH_MAX];
    char shortcut_labels[16][64];
} FilesApp;

/* files_fs.c */
bool fs_list_dir(const char *path, bool hidden, FsList *out);
void fs_list_free(FsList *list);
void fs_join(char *out, size_t n, const char *a, const char *b);
bool fs_parent(const char *path, char *out, size_t n);
bool fs_exists(const char *path);
bool fs_is_dir(const char *path);
void fs_format_size(off_t size, char *out, size_t n);
void fs_format_time(time_t t, char *out, size_t n);
const char *fs_home(void);
int fs_shortcuts(char paths[][PATH_MAX], char labels[][64], int max);

/* files_ui.c */
void files_ui_build(FilesApp *app);
void files_ui_reload(FilesApp *app);
void files_ui_update(FilesApp *app);
void files_ui_input(FilesApp *app, InputAction action, const char *text);
void files_ui_tick(FilesApp *app);
void files_ui_free(FilesApp *app);
void files_ui_show_busy(FilesApp *app, const char *label);
void files_ui_close_modal(FilesApp *app);

void files_select_path(FilesApp *app, const char *path);
void files_cancel(FilesApp *app);
void files_start_op(FilesApp *app, char **argv, const char *label, int action);
void files_poll_op(FilesApp *app);
void files_request_output(FilesApp *app, const char *path);
void files_finish_multi(FilesApp *app, char **paths, int count);

#endif
