// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_FLATPAK_PRESETS_H
#define TAIJI_FLATPAK_PRESETS_H

/* Flathub mirrors. 官方源固定置顶，默认选择国内源。 */
typedef struct {
    const char *id;
    const char *label;
    const char *url;
} TaijiFlatpakPreset;

static const TaijiFlatpakPreset taiji_flatpak_mirrors[] = {
    { "official", "官方 Flathub", "https://dl.flathub.org/repo/" },
    { "sjtu",     "上海交大",     "https://mirror.sjtu.edu.cn/flathub" },
    { "ustc",     "中科大 USTC",  "https://mirrors.ustc.edu.cn/flathub" },
};

#define TAIJI_FLATPAK_MIRROR_COUNT \
    ((int)(sizeof(taiji_flatpak_mirrors) / sizeof(taiji_flatpak_mirrors[0])))
#define TAIJI_FLATPAK_DEFAULT_MIRROR 1

#endif
