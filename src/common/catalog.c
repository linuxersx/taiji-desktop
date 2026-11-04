// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* The table intentionally tracks the Android app's platform catalogue, using the
 * same NeoStation wordmark assets. `aliases` are matched against folder names. */
static const PlatformDef PLATFORMS[] = {
    {"nes", "Nintendo Entertainment System", "NES", "nes.png",
     "nes,famicom,fc,familycomputer", "nes,fds,unf,unif,nsf", 0xFFE45A5A},
    {"snes", "Super Nintendo", "SFC", "snes.png",
     "snes,sfc,superfamicom,supernintendo", "sfc,smc,swc,fig,bs,st", 0xFF8E7CFF},
    {"n64", "Nintendo 64", "N64", "n64.png",
     "n64,nintendo64", "z64,n64,v64,rom,bin", 0xFF4FC3F7},
    {"gb", "Game Boy", "GB", "gb.png",
     "gb,gameboy", "gb", 0xFF9CCC65},
    {"gbc", "Game Boy Color", "GBC", "gbc.png",
     "gbc,gameboycolor", "gbc", 0xFFFFCA28},
    {"gba", "Game Boy Advance", "GBA", "gba.png",
     "gba,gameboyadvance", "gba,srl", 0xFF7E57C2},
    {"nds", "Nintendo DS", "NDS", "ds.png",
     "nds,ds,nintendods", "nds,dsi,ids", 0xFF90A4AE},
    {"3ds", "Nintendo 3DS", "3DS", "3ds.png",
     "3ds,n3ds,nintendo3ds", "3ds,cci,cxi,cia", 0xFFEF5350},
    {"gc", "Nintendo GameCube", "GC", "gc.png",
     "gc,ngc,gamecube", "iso,gcm,rvz,gcz", 0xFF7E57C2},
    {"wii", "Nintendo Wii", "Wii", "wii.png",
     "wii,nintendowii", "iso,wbfs,rvz,wad", 0xFFB0BEC5},
    {"wiiu", "Nintendo Wii U", "WiiU", "wiiu.png",
     "wiiu", "wud,wux,rpx", 0xFF42A5F5},
    {"switch", "Nintendo Switch", "NSW", "switch.png",
     "switch,nsw", "nsp,xci,nsz,xcz", 0xFFE53935},
    {"vb", "Virtual Boy", "VB", "vb.png",
     "vb,virtualboy", "vb,vboy", 0xFFC62828},
    {"gw", "Game & Watch", "G&W", "gw.png",
     "gw,gameandwatch", "mgw", 0xFFFFB300},
    {"md", "Mega Drive / Genesis", "MD", "md.png",
     "md,genesis,megadrive,megadrivegenesis", "md,gen,bin,smd", 0xFF42A5F5},
    {"smc", "Sega Master System", "SMS", "sms.png",
     "sms,mastersystem,sega mastersystem", "sms", 0xFF29B6F6},
    {"gg", "Game Gear", "GG", "gg.png",
     "gg,gamegear", "gg", 0xFF26A69A},
    {"32x", "Sega 32X", "32X", "32x.png",
     "32x,sega32x", "32x,bin", 0xFFFF7043},
    {"scd", "Sega CD", "SCD", "scd.png",
     "scd,segacd,megacd,mdcd", "cue,chd,iso,bin", 0xFF546E7A},
    {"sat", "Sega Saturn", "SS", "sat.png",
     "sat,saturn,segasaturn", "cue,chd,iso,bin", 0xFF8D6E63},
    {"dc", "Dreamcast", "DC", "dc.png",
     "dc,dreamcast", "gdi,cdi,chd,cue", 0xFFFF8A65},
    {"sg1k", "SG-1000", "SG1K", "sg1k.png",
     "sg1k,sg1000", "sg", 0xFF4DB6AC},
    {"ps1", "PlayStation", "PS1", "ps1.png",
     "ps1,psx,playstation,psone", "cue,chd,iso,bin,img,ecm,pbp", 0xFF90A4AE},
    {"ps2", "PlayStation 2", "PS2", "ps2.png",
     "ps2,playstation2", "iso,chd,cso,bin", 0xFF42A5F5},
    {"ps3", "PlayStation 3", "PS3", "ps3.png",
     "ps3,playstation3", "iso,pkg,dec", 0xFF5C6BC0},
    {"psp", "PlayStation Portable", "PSP", "psp.png",
     "psp,playstationportable", "iso,cso,chd,pbp", 0xFF7E57C2},
    {"vita", "PlayStation Vita", "Vita", "vita.png",
     "vita,psvita,psv", "vpk,psv", 0xFF5C6BC0},
    {"arcade", "Arcade", "ARC", "arc.png",
     "arcade,mame,fba,fbneo,neogeo,naomi,atomiswave,cps1,cps2,cps3", "zip,7z", 0xFFFFB74D},
    {"neogeo", "Neo Geo", "NG", "neogeo.png",
     "neogeo,ng", "zip,7z", 0xFFFDD835},
    {"neogeocd", "Neo Geo CD", "NGCD", "ngcd.png",
     "neogeocd,ngcd", "cue,chd,iso", 0xFFF9A825},
    {"ngp", "Neo Geo Pocket", "NGP", "ngp.png",
     "ngp,neogeopocket", "ngp", 0xFFFFB300},
    {"ngpc", "Neo Geo Pocket Color", "NGPC", "ngpc.png",
     "ngpc,neogeopocketcolor", "ngc", 0xFFFFA000},
    {"pce", "PC Engine", "PCE", "pce.png",
     "pce,pcengine,turbografx,tg16", "pce,sgx", 0xFFEF6C00},
    {"pcecd", "PC Engine CD", "PCE-CD", "pccd.png",
     "pcecd,pcenginecd,turbografxcd,tgcd", "cue,chd,iso", 0xFFF57C00},
    {"pcfx", "PC-FX", "PC-FX", "pcfx.png",
     "pcfx,pcfx", "cue,chd,iso", 0xFF8D6E63},
    {"ws", "WonderSwan", "WS", "ws.png",
     "ws,wonderswan", "ws", 0xFF78909C},
    {"wsc", "WonderSwan Color", "WSC", "wsc.png",
     "wsc,wonderswancolor", "wsc", 0xFF607D8B},
    {"msx", "MSX", "MSX", "msx.png",
     "msx,msx1,msx2,msx2plus", "rom,mx1,mx2,bin,dsk", 0xFF29B6F6},
    {"c64", "Commodore 64", "C64", "c64.png",
     "c64,commodore64", "d64,t64,prg,crx,crt", 0xFF9575CD},
    {"amiga", "Commodore Amiga", "Amiga", "amiga.png",
     "amiga,amigacd32,amigacd", "adf,adz,dms,ipf,hdf,lha,iso", 0xFF5C6BC0},
    {"dos", "DOS", "DOS", "dos.png",
     "dos,msdos,pc", "exe,com,bat,conf,iso", 0xFF8D6E63},
    {"scummvm", "ScummVM", "ScummVM", "scummvm.png",
     "scummvm", "scummvm,exe", 0xFF8D6E63},
    {"atari2600", "Atari 2600", "2600", "2600.png",
     "atari2600,2600,a2600", "a26,bin,rom", 0xFFFF7043},
    {"atari5200", "Atari 5200", "5200", "5200.png",
     "atari5200,5200,a5200", "a52,bin,rom", 0xFFFF8A65},
    {"atari7800", "Atari 7800", "7800", "7800.png",
     "atari7800,7800,a7800", "a78,bin,rom", 0xFFFFA726},
    {"lynx", "Atari Lynx", "Lynx", "lynx.png",
     "lynx,atarilynx", "lnx", 0xFF66BB6A},
    {"jaguar", "Atari Jaguar", "Jaguar", "jag.png",
     "jaguar,atarijaguar,jag", "j64,jag,rom,abs,cof", 0xFFEF5350},
    {"3do", "3DO", "3DO", "3do.png",
     "3do,panasonic3do", "cue,chd,iso", 0xFF8D6E63},
    {"cdi", "Philips CD-i", "CD-i", "cdi.png",
     "cdi,philipscdi", "cue,chd,iso", 0xFF78909C},
    {"cv", "ColecoVision", "CV", "cv.png",
     "cv,colecovision", "col,bin,rom", 0xFF42A5F5},
    {"intv", "Intellivision", "Intv", "intv.png",
     "intv,intellivision", "int,bin,rom", 0xFFAB47BC},
    {"vect", "Vectrex", "Vectrex", "vect.png",
     "vectrex,vect", "vec,bin,rom", 0xFF26C6DA},
    {"zxspectrum", "ZX Spectrum", "ZX", "zxspectrum.png",
     "zxspectrum,zx,spectrum", "tap,tzx,z80,sna,szx,trd", 0xFFFF7043},
    {"cpc", "Amstrad CPC", "CPC", "cpc.png",
     "cpc,amstradcpc", "dsk,cdt,sna,bin", 0xFF66BB6A},
    {"bbcmicro", "BBC Micro", "BBC", "bbcmicro.png",
     "bbcmicro,bbc", "ssd,dsd,uef,mmb", 0xFF8D6E63},
    {"apple2", "Apple II", "A2", "a2.png",
     "apple2,appleii,a2", "dsk,do,po,nib,woz", 0xFF8BC34A},
    {"pc98", "PC-98", "PC98", "pc98.png",
     "pc98,pc9801,pc9821", "hdi,fdi,d88,88d", 0xFF78909C},
    {"x68000", "Sharp X68000", "X68K", "sharpx68000.png",
     "x68000,sharpx68000,x68k", "hdf,hdm,d88,88d", 0xFF8D6E63},
    {"x1", "Sharp X1", "X1", "x1.png",
     "x1,sharpx1", "d88,88d,2d,tap", 0xFF795548},
    {"pico8", "PICO-8", "PICO-8", "pico8.png",
     "pico8,pico-8", "png,p8,p8png", 0xFFFF4D6D},
    {"tic80", "TIC-80", "TIC-80", "tic80.png",
     "tic80,tic-80", "tic", 0xFF00BCD4},
    {"naomi", "Naomi / Atomiswave", "NAOMI", "naomi.png",
     "naomi,atomiswave,aw", "zip,7z,bin,dat", 0xFFFFA726},
    {"all", "全部游戏", "ALL", "all.png", "", "", 0xFF4D8BFF},
};

const PlatformDef *catalog_all(int *count)
{
    if (count) *count = (int)(sizeof(PLATFORMS) / sizeof(PLATFORMS[0]));
    return PLATFORMS;
}

const PlatformDef *catalog_by_id(const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < sizeof(PLATFORMS) / sizeof(PLATFORMS[0]); i++) {
        if (str_ieq(PLATFORMS[i].id, id)) return &PLATFORMS[i];
    }
    return NULL;
}

const PlatformDef *catalog_by_folder(const char *folder_name)
{
    if (!folder_name) return NULL;
    char *norm = xstrdup(folder_name);
    str_lower(norm);
    char *clean = str_trim(norm);
    size_t clen = strlen(clean);
    while (clen > 0 && (clean[clen - 1] == '/' || clean[clen - 1] == '\\')) clean[--clen] = '\0';

    /* 1. exact alias or exact display name */
    for (size_t i = 0; i < sizeof(PLATFORMS) / sizeof(PLATFORMS[0]); i++) {
        if (str_ieq(clean, PLATFORMS[i].id) || str_ieq(clean, PLATFORMS[i].name) ||
            str_ieq(clean, PLATFORMS[i].short_name)) {
            free(norm);
            return &PLATFORMS[i];
        }
        char *aliases = xstrdup(PLATFORMS[i].aliases);
        char *save = NULL;
        for (char *tok = strtok_r(aliases, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            if (str_ieq(clean, str_trim(tok))) {
                free(aliases);
                free(norm);
                return &PLATFORMS[i];
            }
        }
        free(aliases);
    }

    /* 2. compact name match, choosing the longest alias so PlayStation 2 wins over
     * the generic PlayStation alias. */
    char compact[512];
    size_t cj = 0;
    for (const char *p = clean; *p && cj + 1 < sizeof(compact); p++) {
        if (isalnum((unsigned char)*p)) compact[cj++] = (char)tolower((unsigned char)*p);
    }
    compact[cj] = '\0';
    const PlatformDef *best_compact = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < sizeof(PLATFORMS) / sizeof(PLATFORMS[0]); i++) {
        char *aliases = xstrdup(PLATFORMS[i].aliases);
        char *save = NULL;
        for (char *tok = strtok_r(aliases, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char ac[128];
            size_t aj = 0;
            for (const char *p = str_trim(tok); *p && aj + 1 < sizeof(ac); p++) {
                if (isalnum((unsigned char)*p)) ac[aj++] = (char)tolower((unsigned char)*p);
            }
            ac[aj] = '\0';
            if (aj >= 5 && strstr(compact, ac) && aj > best_len) {
                best_compact = &PLATFORMS[i];
                best_len = aj;
            }
        }
        free(aliases);
    }
    if (best_compact) {
        free(norm);
        return best_compact;
    }

    /* 3. an alias surrounded by separators, e.g. "roms_nes" or "nes-games" */
    for (size_t i = 0; i < sizeof(PLATFORMS) / sizeof(PLATFORMS[0]); i++) {
        char *aliases = xstrdup(PLATFORMS[i].aliases);
        char *save = NULL;
        for (char *tok = strtok_r(aliases, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char *a = str_trim(tok);
            size_t la = strlen(a);
            if (la < 2) continue;
            const char *p = clean;
            while ((p = strstr(p, a))) {
                bool left_ok = (p == clean) || !isalnum((unsigned char)p[-1]);
                bool right_ok = (p[la] == '\0') || !isalnum((unsigned char)p[la]);
                if (left_ok && right_ok) {
                    free(aliases);
                    free(norm);
                    return &PLATFORMS[i];
                }
                p += la;
            }
        }
        free(aliases);
    }
    free(norm);
    return NULL;
}

const PlatformDef *catalog_by_extension(const char *ext)
{
    if (!ext || !*ext) return NULL;
    for (size_t i = 0; i < sizeof(PLATFORMS) / sizeof(PLATFORMS[0]); i++) {
        char *exts = xstrdup(PLATFORMS[i].extensions);
        char *save = NULL;
        bool hit = false;
        for (char *tok = strtok_r(exts, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            if (str_ieq(str_trim(tok), ext)) { hit = true; break; }
        }
        free(exts);
        if (hit) return &PLATFORMS[i];
    }
    return NULL;
}

bool catalog_accepts_extension(const PlatformDef *def, const char *ext)
{
    if (!def || !ext || !*ext) return false;
    if (!def->extensions[0]) return true;
    char *exts = xstrdup(def->extensions);
    char *save = NULL;
    bool hit = false;
    for (char *tok = strtok_r(exts, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (str_ieq(str_trim(tok), ext)) { hit = true; break; }
    }
    free(exts);
    if (hit) return true;
    /* Archives are commonly used as ROM containers for every system. */
    return str_ieq(ext, "zip") || str_ieq(ext, "7z") || str_ieq(ext, "m3u");
}

bool catalog_is_image(const char *path)
{
    const char *ext = path_extension(path);
    return str_ieq(ext, "png") || str_ieq(ext, "jpg") || str_ieq(ext, "jpeg") ||
           str_ieq(ext, "bmp") || str_ieq(ext, "webp");
}

bool catalog_is_rom(const char *path)
{
    const char *ext = path_extension(path);
    if (!ext[0]) return false;
    return catalog_by_extension(ext) != NULL || str_ieq(ext, "zip") ||
           str_ieq(ext, "7z") || str_ieq(ext, "m3u");
}

char *catalog_logo_path(const char *id)
{
    const PlatformDef *def = catalog_by_id(id);
    if (!def) def = catalog_by_id("all");
    char rel[256];
    snprintf(rel, sizeof(rel), "platform_logos/%s", def->logo);
    return asset_path(rel);
}

static bool path_exec(const char *path)
{
    return path && access(path, X_OK) == 0;
}

static bool which_cmd(const char *cmd)
{
    const char *path = getenv("PATH");
    if (!path) return false;
    char *copy = xstrdup(path);
    char *save = NULL;
    bool found = false;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char *p = path_join(dir, cmd);
        if (path_exec(p)) { found = true; free(p); break; }
        free(p);
    }
    free(copy);
    return found;
}

static bool mac_app(const char *name)
{
    const char *home = getenv("HOME");
    char *a = xasprintf("/Applications/%s.app", name);
    char *b = xasprintf("%s/Applications/%s.app", home ? home : "", name);
    bool ok = dir_exists(a) || dir_exists(b);
    free(a); free(b);
    return ok;
}

char *catalog_default_command(const char *id, bool mac)
{
    if (mac) {
        const char *wanted[3] = {NULL, NULL, NULL};
        if (str_ieq(id, "ps1")) { wanted[0] = "DuckStation"; wanted[1] = "RetroArch"; }
        else if (str_ieq(id, "ps2")) { wanted[0] = "PCSX2"; wanted[1] = "RetroArch"; }
        else if (str_ieq(id, "psp")) { wanted[0] = "PPSSPP"; wanted[1] = "RetroArch"; }
        else if (str_ieq(id, "gc") || str_ieq(id, "wii")) { wanted[0] = "Dolphin"; wanted[1] = "RetroArch"; }
        else if (str_ieq(id, "nds")) { wanted[0] = "melonDS"; wanted[1] = "Citra"; wanted[2] = "RetroArch"; }
        else if (str_ieq(id, "3ds")) { wanted[0] = "Azahar"; wanted[1] = "Citra"; wanted[2] = "RetroArch"; }
        else if (str_ieq(id, "switch")) { wanted[0] = "Ryujinx"; wanted[1] = "Suyu"; wanted[2] = "RetroArch"; }
        else if (str_ieq(id, "arcade") || str_ieq(id, "neogeo")) { wanted[0] = "MAME"; wanted[1] = "RetroArch"; }
        else { wanted[0] = "RetroArch"; wanted[1] = "OpenEmu"; }
        const char *pick = NULL;
        for (int i = 0; i < 3 && !pick; i++) {
            if (wanted[i] && mac_app(wanted[i])) pick = wanted[i];
        }
        if (pick) return xasprintf("open -W -a \"%s\" --args %%ROM%%", pick);
        return xstrdup("open %ROM%");
    }

    const char *wanted[3] = {NULL, NULL, NULL};
    if (str_ieq(id, "ps1")) { wanted[0] = "duckstation-qt"; wanted[1] = "retroarch"; }
    else if (str_ieq(id, "ps2")) { wanted[0] = "pcsx2-qt"; wanted[1] = "retroarch"; }
    else if (str_ieq(id, "psp")) { wanted[0] = "ppsspp"; wanted[1] = "retroarch"; }
    else if (str_ieq(id, "gc") || str_ieq(id, "wii")) { wanted[0] = "dolphin-emu"; wanted[1] = "retroarch"; }
    else if (str_ieq(id, "nds")) { wanted[0] = "melonds"; wanted[1] = "retroarch"; }
    else if (str_ieq(id, "3ds")) { wanted[0] = "azahar"; wanted[1] = "retroarch"; }
    else if (str_ieq(id, "switch")) { wanted[0] = "ryujinx"; wanted[1] = "retroarch"; }
    else if (str_ieq(id, "arcade") || str_ieq(id, "neogeo")) { wanted[0] = "mame"; wanted[1] = "retroarch"; }
    else { wanted[0] = "retroarch"; wanted[1] = "mame"; }
    for (int i = 0; i < 3; i++) {
        if (wanted[i] && which_cmd(wanted[i])) return xasprintf("%s %%ROM%%", wanted[i]);
    }
    return xstrdup("xdg-open %ROM%");
}

const char *catalog_platform_name(const char *id)
{
    const PlatformDef *def = catalog_by_id(id);
    return def ? def->name : "未知平台";
}

const char *catalog_zh_name(const char *id)
{
    static const struct { const char *id; const char *name; } names[] = {
        {"all", "全部游戏"}, {"nes", "红白机"}, {"snes", "超级任天堂"},
        {"n64", "任天堂 64"}, {"gb", "Game Boy"}, {"gbc", "Game Boy 彩色"},
        {"gba", "Game Boy Advance"}, {"nds", "任天堂 DS"}, {"3ds", "任天堂 3DS"},
        {"gc", "GameCube"}, {"wii", "Wii"}, {"wiiu", "Wii U"}, {"switch", "Switch"},
        {"vb", "Virtual Boy"}, {"gw", "Game & Watch"}, {"md", "世嘉 MD"},
        {"smc", "世嘉 Master System"}, {"gg", "Game Gear"}, {"32x", "世嘉 32X"},
        {"scd", "Mega CD"}, {"sat", "世嘉土星"}, {"dc", "Dreamcast"},
        {"sg1k", "SG-1000"}, {"ps1", "PlayStation"}, {"ps2", "PlayStation 2"},
        {"ps3", "PlayStation 3"}, {"psp", "PSP"}, {"vita", "PS Vita"},
        {"arcade", "街机"}, {"neogeo", "Neo Geo"}, {"neogeocd", "Neo Geo CD"},
        {"ngp", "Neo Geo Pocket"}, {"ngpc", "Neo Geo Pocket Color"},
        {"pce", "PC Engine"}, {"pcecd", "PC Engine CD"}, {"pcfx", "PC-FX"},
        {"ws", "WonderSwan"}, {"wsc", "WonderSwan Color"}, {"msx", "MSX"},
        {"c64", "Commodore 64"}, {"amiga", "Amiga"}, {"dos", "DOS"},
        {"scummvm", "ScummVM"}, {"atari2600", "Atari 2600"}, {"atari5200", "Atari 5200"},
        {"atari7800", "Atari 7800"}, {"lynx", "Atari Lynx"}, {"jaguar", "Atari Jaguar"},
        {"3do", "3DO"}, {"cdi", "CD-i"}, {"cv", "ColecoVision"}, {"intv", "Intellivision"},
        {"vect", "Vectrex"}, {"zxspectrum", "ZX Spectrum"}, {"cpc", "Amstrad CPC"},
        {"bbcmicro", "BBC Micro"}, {"apple2", "Apple II"}, {"pc98", "PC-98"},
        {"x68000", "Sharp X68000"}, {"x1", "Sharp X1"}, {"pico8", "PICO-8"},
        {"tic80", "TIC-80"}, {"naomi", "Naomi"},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (str_ieq(id, names[i].id)) return names[i].name;
    }
    const PlatformDef *def = catalog_by_id(id);
    return def ? def->name : "未知平台";
}
