// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_MODEL_H
#define TAIJI_MODEL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char *id;
    char *platform_id;
    char *name;
    char *sort_name;
    char *rom_path;
    char *cover_path;
    char *logo_path;
    char *description;
    char *developer;
    char *publisher;
    char *genre;
    char *release_date;
    int players;
    bool favorite;
    bool hidden;
    int64_t last_played;
    int play_count;
    int64_t play_seconds;
    char *command;
} Game;

typedef struct {
    char *id;
    char *name;
    char *short_name;
    char *logo;
    char *root_dir;
    char *cover_path;
    int game_count;
} Platform;

typedef struct {
    Platform *platforms;
    int platform_count;
    Game *games;
    int game_count;
} Library;

typedef enum {
    SORT_NAME = 0,
    SORT_RECENT,
    SORT_MOST_PLAYED,
    SORT_FAVORITES_FIRST,
} SortMode;

typedef enum {
    FILTER_ALL = 0,
    FILTER_FAVORITES,
    FILTER_UNPLAYED,
    FILTER_MULTIPLAYER,
} FilterMode;

typedef struct {
    Game **items;
    int count;
} GameList;

#endif
