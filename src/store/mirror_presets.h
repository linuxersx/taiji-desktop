// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_MIRROR_PRESETS_H
#define TAIJI_MIRROR_PRESETS_H

/* Arch Linux ARM pacman mirrors. Keep this table in sync with mirror.c and
 * the privileged helper; both include this header. */
typedef struct {
    const char *id;
    const char *label;
    const char *url;
} TaijiMirrorPreset;

static const TaijiMirrorPreset taiji_mirrors[] = {
    { "official", "官方源",     "http://mirror.archlinuxarm.org/$arch/$repo" },
    { "tuna",     "清华 TUNA",  "https://mirrors.tuna.tsinghua.edu.cn/archlinuxarm/$arch/$repo" },
    { "ustc",     "中科大 USTC", "https://mirrors.ustc.edu.cn/archlinuxarm/$arch/$repo" },
    { "bfsu",     "北外 BFSU",  "https://mirrors.bfsu.edu.cn/archlinuxarm/$arch/$repo" },
    { "huawei",   "华为云",     "https://repo.huaweicloud.com/archlinuxarm/$arch/$repo" },
};

#define TAIJI_MIRROR_COUNT ((int)(sizeof(taiji_mirrors) / sizeof(taiji_mirrors[0])))

#endif
