// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_CONFIG_H
#define TAIJI_CONFIG_H

#include <stdbool.h>

typedef struct {
    char *platform_id;
    char *command;
} PlatformCommand;

typedef struct {
    char **roots;
    int root_count;

    PlatformCommand *commands;
    int command_count;

    char *last_platform;
    char *last_game;

    bool swap_ab;
    bool swap_xy;
    bool custom_mapping;
    int map_confirm;
    int map_cancel;
    int map_menu1;
    int map_menu2;

    int language;       /* 0 中文, 1 English */
    int theme;          /* 0 deep space, 1 paper white, 2 sunset neon, 3 forest */
    bool theme_auto;    /* auto dark/light */
    int auto_dark_min;  /* minutes from midnight; -1 = sunset */
    int auto_light_min; /* minutes from midnight; -1 = sunrise */
    bool sound;
    int volume;         /* 0..100 */
    int ui_scale;       /* 100..300 percent */
    bool ui_scale_auto; /* derive scale from the display resolution */
    int refresh_hz;     /* 0 = auto, 60 or 120 = present-rate limit */
    int render_scale;   /* 50..100, internal render resolution percent */
    char renderer[16];  /* opengl, opengles2, vulkan */
    bool reduce_motion;
    int grid_columns;   /* 0 = automatic */
    bool scan_on_start;
    bool show_cover_panel;
    bool low_power;
    int app_mode;        /* 0 auto, 1 resident, 2 handoff */
    int max_paused_apps; /* 1..4 */
    int store_pkg_type;  /* 0 pacman, 1 flatpak */
    int store_pacman_source;
    int store_flatpak_source;
} Config;

void config_init(Config *c);
void config_free(Config *c);
void config_load(Config *c);
void config_save(const Config *c);

void config_add_root(Config *c, const char *path);
void config_remove_root(Config *c, int index);
const char *config_command_for(const Config *c, const char *platform_id);
void config_set_command(Config *c, const char *platform_id, const char *cmd);

#endif
