// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_STORE_H
#define TAIJI_STORE_H

#include <stdbool.h>
#include <stddef.h>

#define STORE_SOURCE_MAX 32
#define STORE_ID_MAX 128
#define STORE_NAME_MAX 160
#define STORE_TEXT_MAX 512

typedef struct {
    char source[STORE_SOURCE_MAX];
    char id[STORE_ID_MAX];
    char name[STORE_NAME_MAX];
    char version[64];
    char installed_version[64];
    char description[STORE_TEXT_MAX];
    char license[64];
    char category[32];
    char icon[512];
    long long size_kb;
    bool installed;
    bool update_available;
    bool removable;
} StorePackage;

typedef struct {
    StorePackage *items;
    int count;
    int capacity;
} StorePackageList;

typedef struct {
    void (*progress)(void *ud, const char *stage, int percent, const char *line);
    void *ud;
    volatile bool *cancel;
} StoreProgress;

typedef struct StoreBackend StoreBackend;

struct StoreBackend {
    const char *source;       /* pacman / flatpak */
    const char *label;
    bool needs_root;
    bool supports_remove;
    bool supports_update;

    bool (*probe)(char *error, size_t error_size);
    bool (*refresh)(StoreBackend *self, StorePackageList *out,
                    StoreProgress *progress, char *error, size_t error_size);
    bool (*install)(StoreBackend *self, const char **ids, int count,
                    StoreProgress *progress, char *error, size_t error_size);
    bool (*remove)(StoreBackend *self, const char **ids, int count,
                   StoreProgress *progress, char *error, size_t error_size);
    bool (*update)(StoreBackend *self, const char **ids, int count,
                   StoreProgress *progress, char *error, size_t error_size);
    bool (*is_protected)(const char *id);
};

bool store_list_push(StorePackageList *list, const StorePackage *pkg);
void store_list_free(StorePackageList *list);
StorePackage *store_list_find(StorePackageList *list, const char *source,
                              const char *id);

int store_backend_count(void);
StoreBackend *store_backend_at(int index);
StoreBackend *store_backend_find(const char *source);

const char *store_helper_path(void);

#endif
