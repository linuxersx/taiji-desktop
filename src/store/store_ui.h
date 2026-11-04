// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_STORE_UI_H
#define TAIJI_STORE_UI_H

#include "store.h"
#include "backend.h"
#include "config.h"

#include <pthread.h>
#include <stdbool.h>

typedef struct {
    Backend backend;
    Config config;
    StorePackageList all;
    int *filter;
    int filter_count;
    int focus;

    char search[96];
    bool search_edit;
    int category;
    int active_theme;
    int ui_percent;

    bool on_sidebar;
    int side_focus;

    bool show_detail;
    int detail_index;

    bool refreshing;
    volatile bool refresh_done;
    volatile bool refresh_ok;
    volatile bool refresh_cancel;
    char refresh_error[256];
    pthread_t refresh_thread;
    StorePackageList refresh_result;

    bool task_running;
    volatile bool task_done;
    volatile bool task_ok;
    volatile bool task_cancel;
    int task_kind; /* 0 install, 1 remove, 2 update */
    char task_stage[256];
    int task_percent;
    char task_line[256];
    char task_error[256];
    char notice[256];
    pthread_t task_thread;
    StoreBackend *task_backend;
    char **task_ids;
    int task_id_count;

    bool running;
    bool dirty;
    bool embedded;
    int view_w;
    int view_h;
    int percent;
    pthread_mutex_t lock;
    lv_obj_t *root;
    lv_obj_t *list;
    lv_obj_t *status;
} StoreApp;

void store_ui_init(StoreApp *app);
void store_ui_init_embedded(StoreApp *app, const Config *config, int width,
                            int height, int percent, lv_obj_t *root);
void store_ui_set_view(StoreApp *app, int width, int height, int percent,
                       lv_obj_t *root);
void store_ui_sync_config(StoreApp *app, const Config *config);
void store_ui_build(StoreApp *app);
void store_ui_input(StoreApp *app, InputAction action, const char *text);
void store_ui_tick(StoreApp *app);
void store_ui_free(StoreApp *app);
void store_ui_refresh(StoreApp *app);
void store_ui_start_task(StoreApp *app, int kind, int pkg_index);
bool store_ui_any_backend(char *error, size_t error_size);
const char *store_category_name(int category);

enum {
    STORE_CAT_ALL = 0,
    STORE_CAT_INSTALLED,
    STORE_CAT_UPDATES,
    STORE_CAT_GAMES,
    STORE_CAT_DEV,
    STORE_CAT_MEDIA,
    STORE_CAT_NETWORK,
    STORE_CAT_SYSTEM,
    STORE_CAT_UTILITY,
    STORE_CAT_DESKTOP,
    STORE_CAT_COUNT
};

#endif
