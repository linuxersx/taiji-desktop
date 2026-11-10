// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "editor.h"
#include "audio.h"
#include "external_osk.h"
#include "files.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define EDITOR_MAX_BYTES (4 * 1024 * 1024)

static const char *k_text_ext[] = {
    "txt", "text", "md", "markdown", "mdown", "mkd",
    "rtf", "html", "htm", "xml", "json", "yml", "yaml",
    "ini", "conf", "cfg", "log", "csv", "tsv", "srt",
    "sh", "bash", "py", "js", "ts", "css", "c", "h", "cpp", "hpp",
    "lua", "toml", "nfo", "jsonc", NULL
};

const char *editor_format_label(const char *path)
{
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path;
    const char *dot = base ? strrchr(base, '.') : NULL;
    if (!dot || !dot[1]) return "纯文本";
    const char *ext = dot + 1;
    if (!strcasecmp(ext, "md") || !strcasecmp(ext, "markdown") ||
        !strcasecmp(ext, "mdown") || !strcasecmp(ext, "mkd"))
        return "Markdown";
    if (!strcasecmp(ext, "rtf")) return "RTF（源码）";
    if (!strcasecmp(ext, "html") || !strcasecmp(ext, "htm"))
        return "HTML（源码）";
    if (!strcasecmp(ext, "txt") || !strcasecmp(ext, "text")) return "纯文本";
    return "文本";
}

bool editor_is_text_path(const char *path)
{
    if (!path || !*path) return false;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || !dot[1]) return true;
    for (int i = 0; k_text_ext[i]; i++)
        if (!strcasecmp(dot + 1, k_text_ext[i])) return true;
    return false;
}

void editor_close(App *app)
{
    EditorState *ed = &app->editor;
    for (int i = 0; i < ed->count; i++) free(ed->lines[i]);
    free(ed->lines);
    memset(ed, 0, sizeof(*ed));
}

void editor_append_line(App *app)
{
    EditorState *ed = &app->editor;
    if (ed->count >= EDITOR_MAX_LINES) return;
    if (ed->count >= ed->cap) {
        int cap = ed->cap > 0 ? ed->cap * 2 : 128;
        char **nl = realloc(ed->lines, sizeof(char *) * (size_t)cap);
        if (!nl) return;
        ed->lines = nl;
        ed->cap = cap;
    }
    ed->lines[ed->count++] = xstrdup("");
}

void editor_insert_after(App *app, int idx)
{
    EditorState *ed = &app->editor;
    if (idx < -1) idx = -1;
    if (idx >= ed->count) idx = ed->count - 1;
    if (ed->count >= EDITOR_MAX_LINES) return;
    if (ed->count >= ed->cap) editor_append_line(app);
    if (ed->count >= EDITOR_MAX_LINES) return;
    int at = idx + 1;
    memmove(&ed->lines[at + 1], &ed->lines[at],
            sizeof(char *) * (size_t)(ed->count - at));
    ed->lines[at] = xstrdup("");
    ed->count++;
    ed->dirty = true;
}

void editor_set_line(App *app, int idx, const char *text)
{
    EditorState *ed = &app->editor;
    if (idx < 0 || idx >= ed->count) return;
    const char *value = text ? text : "";
    if (strcmp(ed->lines[idx], value) == 0) return;
    free(ed->lines[idx]);
    ed->lines[idx] = xstrdup(value);
    ed->dirty = true;
}

static bool editor_split(App *app, const char *data, size_t len)
{
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && data[i] != '\n') continue;
        if (app->editor.count >= EDITOR_MAX_LINES) {
            app->editor.status[0] = '\0';
            snprintf(app->editor.status, sizeof(app->editor.status),
                     "文件过大，只载入前 %d 行", EDITOR_MAX_LINES);
            return true;
        }
        size_t n = i - start;
        while (n > 0 && data[start + n - 1] == '\r') n--;
        if (n > EDITOR_LINE_MAX) n = EDITOR_LINE_MAX;
        char *line = xmalloc(n + 1);
        memcpy(line, data + start, n);
        line[n] = '\0';
        editor_append_line(app);
        if (app->editor.count > 0) {
            free(app->editor.lines[app->editor.count - 1]);
            app->editor.lines[app->editor.count - 1] = line;
        } else {
            free(line);
        }
        start = i + 1;
    }
    return true;
}

bool editor_open(App *app, const char *path)
{
    if (!app || !path || !*path) return false;
    if (!editor_is_text_path(path)) {
        ui_toast(app, "这个文件不是文本格式");
        return false;
    }
    editor_close(app);
    EditorState *ed = &app->editor;
    snprintf(ed->path, sizeof(ed->path), "%s", path);
    const char *base = strrchr(path, '/');
    snprintf(ed->name, sizeof(ed->name), "%s", base ? base + 1 : path);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char chunk[65536];
        char *data = NULL;
        size_t len = 0, cap = 0;
        bool truncated = false;
        bool read_error = false;
        for (;;) {
            ssize_t got = read(fd, chunk, sizeof(chunk));
            if (got < 0) {
                if (errno == EINTR) continue;
                read_error = true;
                break;
            }
            if (got == 0) break;
            size_t add = (size_t)got;
            if (add > EDITOR_MAX_BYTES - len) {
                add = EDITOR_MAX_BYTES - len;
                truncated = true;
            }
            if (len + add > cap) {
                size_t nc = cap ? cap * 2 : 65536;
                while (nc < len + add) nc *= 2;
                if (nc > EDITOR_MAX_BYTES) nc = EDITOR_MAX_BYTES;
                char *nd = realloc(data, nc);
                if (!nd) { read_error = true; break; }
                data = nd;
                cap = nc;
            }
            memcpy(data + len, chunk, add);
            len += add;
            if (truncated) break;
        }
        close(fd);
        if (!data || read_error) {
            free(data);
            ui_toast(app, "无法读取文件");
            return false;
        }
        if (truncated) {
            snprintf(ed->status, sizeof(ed->status), "文件过大，只载入前 4 MB");
        }
        if (memchr(data, '\0', len)) {
            free(data);
            ui_toast(app, "二进制文件无法用文本编辑器打开");
            return false;
        }
        editor_split(app, data, len);
        free(data);
    } else if (errno != ENOENT) {
        ui_toast(app, "无法读取文件");
        return false;
    }
    if (ed->count == 0) editor_append_line(app);
    ed->active = true;
    ed->editing = false;
    ed->dirty = false;
    ed->focus = 0;
    ed->col = 0;
    ed->want_col = 0;
    ed->top = 0;
    if (!ed->status[0]) snprintf(ed->status, sizeof(ed->status), "已载入");
    app_goto_screen(app, SCREEN_EDITOR);
    return true;
}

bool editor_save(App *app)
{
    EditorState *ed = &app->editor;
    if (!ed->active || !ed->path[0]) return false;
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", ed->path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ui_toast(app, "保存失败：无法写入");
        return false;
    }
    for (int i = 0; i < ed->count; i++) {
        if (i) fputc('\n', f);
        if (ed->lines[i] && fputs(ed->lines[i], f) == EOF) break;
    }
    if (fflush(f) != 0 || ferror(f)) {
        fclose(f);
        unlink(tmp);
        ui_toast(app, "保存失败：写入错误");
        return false;
    }
    fclose(f);
    struct stat st;
    if (stat(ed->path, &st) == 0) chmod(tmp, st.st_mode & 07777);
    else chmod(tmp, 0644);
    if (rename(tmp, ed->path) != 0) {
        unlink(tmp);
        ui_toast(app, "保存失败：无法替换原文件");
        return false;
    }
    ed->dirty = false;
    snprintf(ed->status, sizeof(ed->status), "已保存");
    ui_toast(app, "已保存");
    if (app->screen == SCREEN_EDITOR) app_rebuild(app);
    return true;
}

void editor_edit_line(App *app, int idx)
{
    EditorState *ed = &app->editor;
    if (!ed->active) return;
    if (idx < 0) idx = 0;
    while (ed->count <= idx && ed->count < EDITOR_MAX_LINES) {
        editor_append_line(app);
        ed->dirty = true;
    }
    if (ed->count == 0) return;
    if (idx >= ed->count) idx = ed->count - 1;
    ed->focus = idx;
    char title[80];
    snprintf(title, sizeof(title), "编辑第 %d 行", idx + 1);
    app_open_text(app, TEXT_EDITOR_LINE, idx, title, ed->lines[idx],
                  "确定/Enter 下一行 / 取消/Esc 完成");
}

void editor_leave(App *app)
{
    external_osk_hide(app);
    editor_close(app);
    app_goto_screen(app, SCREEN_FILES);
}

static int editor_line_len(App *app, int idx)
{
    if (idx < 0 || idx >= app->editor.count) return 0;
    return (int)strlen(app->editor.lines[idx]);
}

static int utf8_next(const char *s, int i)
{
    if (!s || i < 0) return 0;
    i++;
    while (s[i] && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static int utf8_prev(const char *s, int i)
{
    if (!s || i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static void editor_sync_col(App *app)
{
    EditorState *ed = &app->editor;
    int len = editor_line_len(app, ed->focus);
    if (ed->col < 0) ed->col = 0;
    if (ed->col > len) ed->col = len;
}

void editor_enter(App *app)
{
    EditorState *ed = &app->editor;
    if (!ed->active || ed->focus < 0 || ed->focus >= ed->count) return;
    char *line = ed->lines[ed->focus];
    int len = (int)strlen(line);
    if (ed->col < 0) ed->col = 0;
    if (ed->col > len) ed->col = len;
    char *rest = xstrdup(line + ed->col);
    line[ed->col] = '\0';
    editor_insert_after(app, ed->focus);
    if (ed->focus + 1 < ed->count) {
        free(ed->lines[ed->focus + 1]);
        ed->lines[ed->focus + 1] = rest;
        ed->focus++;
        ed->col = 0;
        ed->want_col = 0;
        ed->dirty = true;
    } else {
        free(rest);
    }
    app_rebuild(app);
}

void editor_begin_edit(App *app)
{
    EditorState *ed = &app->editor;
    if (!ed->active) return;
    if (!external_osk_available()) {
        editor_edit_line(app, ed->focus);
        return;
    }
    ed->editing = true;
    external_osk_show(app);
    app_rebuild(app);
}

void editor_stop_edit(App *app)
{
    EditorState *ed = &app->editor;
    if (!ed->active) return;
    ed->editing = false;
    external_osk_hide(app);
    app_rebuild(app);
}

void editor_insert_text(App *app, const char *text)
{
    EditorState *ed = &app->editor;
    if (!ed->active || !text || !*text) return;
    if (ed->focus < 0 || ed->focus >= ed->count) return;
    char *line = ed->lines[ed->focus];
    size_t len = strlen(line);
    size_t add = strlen(text);
    if (ed->col < 0) ed->col = 0;
    if ((size_t)ed->col > len) ed->col = (int)len;
    if (len + add >= EDITOR_LINE_MAX) return;
    char *nl = xmalloc(len + add + 1);
    memcpy(nl, line, (size_t)ed->col);
    memcpy(nl + ed->col, text, add);
    memcpy(nl + ed->col + add, line + ed->col, len - (size_t)ed->col + 1);
    free(line);
    ed->lines[ed->focus] = nl;
    ed->col += (int)add;
    ed->want_col = ed->col;
    ed->dirty = true;
    app_rebuild(app);
}

void editor_clear_line(App *app)
{
    EditorState *ed = &app->editor;
    if (!ed->active || ed->focus < 0 || ed->focus >= ed->count) return;
    if (!ed->lines[ed->focus][0]) return;
    free(ed->lines[ed->focus]);
    ed->lines[ed->focus] = xstrdup("");
    ed->col = 0;
    ed->want_col = 0;
    ed->dirty = true;
    app_rebuild(app);
}

void editor_backspace(App *app)
{
    EditorState *ed = &app->editor;
    if (!ed->active || ed->focus < 0 || ed->focus >= ed->count) return;
    char *line = ed->lines[ed->focus];
    int len = (int)strlen(line);
    if (ed->col < 0) ed->col = 0;
    if (ed->col > len) ed->col = len;
    if (ed->col > 0) {
        int start = utf8_prev(line, ed->col);
        memmove(line + start, line + ed->col, (size_t)(len - ed->col) + 1);
        ed->col = start;
        ed->want_col = start;
        ed->dirty = true;
        app_rebuild(app);
        return;
    }
    if (ed->focus > 0) {
        int prev = ed->focus - 1;
        size_t pl = strlen(ed->lines[prev]);
        size_t cl = strlen(line);
        char *merged = xmalloc(pl + cl + 1);
        memcpy(merged, ed->lines[prev], pl);
        memcpy(merged + pl, line, cl + 1);
        free(ed->lines[prev]);
        free(line);
        ed->lines[prev] = merged;
        memmove(&ed->lines[ed->focus], &ed->lines[ed->focus + 1],
                sizeof(char *) * (size_t)(ed->count - ed->focus - 1));
        ed->count--;
        ed->focus = prev;
        ed->col = (int)pl;
        ed->want_col = ed->col;
        ed->dirty = true;
        app_rebuild(app);
    }
}

void editor_input(App *app, InputAction action, const char *text)
{
    EditorState *ed = &app->editor;
    if (!ed->active) return;
    if (action == IN_TEXT && text) { editor_insert_text(app, text); return; }
    if (action == IN_BACKSPACE) { editor_backspace(app); return; }
    int n = ed->count;
    if (action == IN_UP || action == IN_DOWN) {
        int dir = action == IN_UP ? -1 : 1;
        int line = ed->focus + dir;
        if (line >= 0 && line < n) {
            ed->focus = line;
            int len = editor_line_len(app, line);
            ed->col = ed->want_col < len ? ed->want_col : len;
            app_rebuild(app);
            audio_play(SND_NAV);
        }
        return;
    }
    if (action == IN_LEFT || action == IN_RIGHT) {
        const char *line = (ed->focus >= 0 && ed->focus < n) ?
                           ed->lines[ed->focus] : "";
        int len = (int)strlen(line);
        if (action == IN_LEFT) {
            if (ed->col > 0) ed->col = utf8_prev(line, ed->col);
            else if (ed->focus > 0) {
                ed->focus--;
                ed->col = editor_line_len(app, ed->focus);
            }
        } else {
            if (ed->col < len) ed->col = utf8_next(line, ed->col);
            else if (ed->focus + 1 < n) {
                ed->focus++;
                ed->col = 0;
            }
        }
        ed->want_col = ed->col;
        app_rebuild(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_LB || action == IN_RB) {
        ed->focus += action == IN_LB ? -10 : 10;
        if (ed->focus < 0) ed->focus = 0;
        if (ed->focus >= n) ed->focus = n > 0 ? n - 1 : 0;
        editor_sync_col(app);
        ed->want_col = ed->col;
        app_rebuild(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_LT) {
        ed->focus = 0;
        ed->col = 0;
        ed->want_col = 0;
        app_rebuild(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_RT) {
        ed->focus = n > 0 ? n - 1 : 0;
        ed->col = editor_line_len(app, ed->focus);
        ed->want_col = ed->col;
        app_rebuild(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_CONFIRM || action == IN_A) {
        audio_play(SND_ENTER);
        editor_begin_edit(app);
        return;
    }
    if (action == IN_X) {
        editor_enter(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_Y) {
        editor_backspace(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_START) {
        editor_save(app);
        return;
    }
    if (action == IN_SELECT) {
        app->modal = MODAL_EDITOR_HELP;
        ui_build_modal(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_CANCEL || action == IN_B) {
        audio_play(SND_BACK);
        if (ed->editing) { editor_stop_edit(app); return; }
        if (ed->dirty)
            app_open_confirm(app, CONFIRM_EDITOR_DISCARD, 0,
                             "放弃修改？", "有未保存的修改，返回将丢失。");
        else
            editor_leave(app);
        return;
    }
}
