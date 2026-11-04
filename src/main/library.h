// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_LIBRARY_H
#define TAIJI_LIBRARY_H

#include "config.h"
#include "model.h"

typedef void (*ScanProgressCb)(void *userdata, const char *message, int percent);

void library_init(Library *lib);
void library_free(Library *lib);

/* Full rescan of configured roots. Replaces the in-memory library. */
bool library_scan(Library *lib, const Config *cfg, ScanProgressCb cb, void *userdata);

bool library_load_cache(Library *lib);
void library_save_cache(const Library *lib);

Game *library_find_game(Library *lib, const char *id);
Platform *library_find_platform(Library *lib, const char *id);
GameList library_query(Library *lib, const char *platform_id, SortMode sort,
                       FilterMode filter, const char *query);
void game_list_free(GameList *list);

void library_note_play(Library *lib, Game *game, int64_t seconds);
void library_toggle_favorite(Library *lib, Game *game);
int library_total_games(const Library *lib);
int library_favorites_count(const Library *lib);
int64_t library_total_play_seconds(const Library *lib);

#endif
