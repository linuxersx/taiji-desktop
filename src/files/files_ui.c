// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "files.h"
#include "i18n.h"
#include "theme.h"
#include "util.h"
#include "star_icon.h"

#include <errno.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    M_NEW = 1,
    M_RENAME,
    M_COPY,
    M_CUT,
    M_PASTE,
    M_DELETE,
    M_DEL_TRASH,
    M_DEL_PERM,
    M_HIDDEN,
    M_SHORTCUTS,
    M_PROP,
    M_PATH,
    M_QUIT,
    M_OVERWRITE,
    M_SAVE_OVERWRITE,
    M_SKIP,
    M_CLOSE,
    M_OPEN,
    M_SHORTCUT = 100,
};

enum { MODAL_NONE = 0, MODAL_MENU, MODAL_TEXT, MODAL_BUSY };

typedef struct {
    FilesApp *app;
    int index;
} RowCtx;

static const char *KROWS[5][10] = {
    {"1","2","3","4","5","6","7","8","9","0"},
    {"q","w","e","r","t","y","u","i","o","p"},
    {"a","s","d","f","g","h","j","k","l","-"},
    {"z","x","c","v","b","n","m",".","_","/"},
    {"空格","删除","确定","取消","","","","","",""},
};

static lv_obj_t *mk_box(lv_obj_t *p, int x, int y, int w, int h, int r, uint32_t c)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *p, const char *text, lv_font_t *font,
                          uint32_t color, int x, int y, int w, int h)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_label_set_text(l, text ? tr(text) : "");
    return l;
}

static lv_obj_t *mk_label_center(lv_obj_t *p, const char *text, lv_font_t *font,
                                 uint32_t color, int x, int y, int w, int h)
{
    lv_obj_t *l = mk_label(p, text, font, color, x, y, w, h);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static FilesMode app_mode(FilesApp *app) { return app->mode; }

static bool current_path(FilesApp *app, char *out, size_t n)
{
    if (!app || app->focus < 0 || app->focus >= app->list.count) return false;
    fs_join(out, n, app->cwd, app->list.items[app->focus].name);
    return true;
}

static const char *mode_text(FilesMode m)
{
    switch (m) {
        case FM_OPEN: return "选择文件";
        case FM_FOLDER: return "选择目录";
        case FM_SAVE: return "保存文件";
        default: return "文件管理器";
    }
}

static void update_status(FilesApp *app);

static void update_row_styles(FilesApp *app)
{
    if (!app->rows) return;
    for (int i = 0; i < app->row_count; i++) {
        lv_obj_t *r = app->rows[i];
        if (!r) continue;
        bool sel = i == app->focus;
        bool marked = app->marked && i < app->list.count && app->marked[i];
        lv_obj_set_style_bg_color(r, lv_color_hex(sel ? PAL.accent : PAL.card), 0);
        lv_obj_set_style_bg_opa(r, sel ? 62 : 55, 0);
        lv_obj_set_style_border_width(r, sel ? 1 : 0, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(PAL.accent2), 0);
        lv_obj_set_style_border_opa(r, sel ? 255 : 0, 0);
        if (app->row_marks && app->row_marks[i]) {
            if (marked) lv_obj_clear_flag(app->row_marks[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(app->row_marks[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    update_status(app);
    if (app->focus >= 0 && app->focus < app->row_count && app->rows[app->focus])
        lv_obj_scroll_to_view(app->rows[app->focus], LV_ANIM_OFF);
}

static void update_status(FilesApp *app)
{
    if (!app->status_label) return;
    if (app->message[0]) {
        lv_label_set_text(app->status_label, app->message);
        lv_obj_set_style_text_color(app->status_label, lv_color_hex(PAL.warning), 0);
        return;
    }
    char buf[160];
    if (app->mode == FM_FOLDER)
        snprintf(buf, sizeof(buf), "%d 项 / 当前目录可确认", app->list.count);
    else if (app->multiple)
        snprintf(buf, sizeof(buf), "%d 项 / 已选 %d / Start 确认", app->list.count,
                 app->marked_count);
    else
        snprintf(buf, sizeof(buf), "%d 项", app->list.count);
    lv_label_set_text(app->status_label, buf);
    lv_obj_set_style_text_color(app->status_label, lv_color_hex(PAL.text2), 0);
}

static void close_modal(FilesApp *app);
void files_ui_menu_action(FilesApp *app, int action);

static void row_clicked(lv_event_t *e)
{
    RowCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    FilesApp *app = ctx->app;
    if (app->focus == ctx->index) {
        char path[PATH_MAX];
        if (!current_path(app, path, sizeof(path))) return;
        if (app->list.items[ctx->index].is_dir) {
            snprintf(app->cwd, sizeof(app->cwd), "%s", path);
            app->focus = 0;
            files_ui_reload(app);
        } else if (app->mode == FM_OPEN) {
            if (app->multiple) {
                app->marked[ctx->index] = !app->marked[ctx->index];
                app->marked_count += app->marked[ctx->index] ? 1 : -1;
                update_row_styles(app);
            } else {
                files_select_path(app, path);
            }
        } else if (app->mode == FM_SAVE) {
            snprintf(app->save_name, sizeof(app->save_name), "%s",
                     app->list.items[ctx->index].name);
            app->message[0] = '\0';
            update_status(app);
        } else {
            if (!fs_has_default_app(path)) {
                snprintf(app->message, sizeof(app->message),
                         "未安装可用的关联程序");
            } else if (fs_open_default(path)) {
                snprintf(app->message, sizeof(app->message), "正在打开 %s",
                         app->list.items[ctx->index].name);
            } else {
                snprintf(app->message, sizeof(app->message),
                         "未安装可用的关联程序");
            }
            update_status(app);
        }
        return;
    }
    app->focus = ctx->index;
    app->message[0] = '\0';
    update_row_styles(app);
}

static void row_ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void add_row(FilesApp *app, lv_obj_t *list, int i, int y, int list_w, int row_h)
{
    FsEntry *it = &app->list.items[i];
    lv_obj_t *r = mk_box(list, 0, y, list_w, row_h, 10, PAL.card);
    lv_obj_set_style_bg_opa(r, 55, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    int icon_x = row_h < 42 ? 10 : 12;
    int name_x = icon_x + (row_h < 42 ? 22 : 28);
    int size_w = list_w >= 620 ? 130 : 84;
    int star_w = row_h >= 42 ? 22 : 18;
    int star_x = list_w - star_w - 8;
    int size_x = star_x - size_w - 8;
    int name_w = size_x - name_x - 8;
    if (name_w < 40) name_w = 40;

    lv_obj_t *m = lv_image_create(r);
    lv_obj_remove_style_all(m);
    lv_obj_set_pos(m, star_x, (row_h - star_w) / 2);
    lv_obj_set_size(m, star_w, star_w);
    lv_image_set_src(m, &taiji_star_icon);
    lv_obj_set_style_image_recolor(m, lv_color_hex(PAL.warning), 0);
    lv_obj_set_style_image_recolor_opa(m, LV_OPA_COVER, 0);
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    app->row_marks[i] = m;

    lv_obj_t *ic = mk_label(r, it->is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                            row_h >= 44 ? FONT_MD : FONT_SM,
                            it->is_dir ? PAL.accent2 : PAL.text3,
                            icon_x, (row_h - 20) / 2, 22, 22);
    lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);

    char view[300];
    if (app->marked && app->marked[i])
        snprintf(view, sizeof(view), "%s", it->name);
    else
        snprintf(view, sizeof(view), "%s", it->name);
    lv_obj_t *n = mk_label(r, view, row_h >= 44 ? FONT_MD : FONT_SM,
                           app->marked && app->marked[i] ? PAL.accent2 : PAL.text,
                           name_x, (row_h - 20) / 2, name_w, 22);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);

    char sz[32];
    fs_format_size(it->size, sz, sizeof(sz));
    lv_obj_t *s = mk_label(r, it->is_dir ? "" : sz, FONT_XS, PAL.text3,
                           size_x, (row_h - 16) / 2, size_w, 18);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_RIGHT, 0);

    RowCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->index = i;
    lv_obj_add_event_cb(r, row_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(r, row_ctx_free, LV_EVENT_DELETE, ctx);
    app->rows[i] = r;
}

static void free_marks(FilesApp *app)
{
    free(app->marked);
    app->marked = NULL;
    app->marked_count = 0;
}

void files_ui_reload(FilesApp *app)
{
    fs_list_free(&app->list);
    fs_list_dir(app->cwd, app->show_hidden, &app->list);
    free_marks(app);
    if (app->list.count > 0) {
        app->marked = calloc((size_t)app->list.count, sizeof(bool));
        if (app->focus >= app->list.count) app->focus = app->list.count - 1;
        if (app->focus < 0) app->focus = 0;
    } else {
        app->focus = 0;
    }
    app->message[0] = '\0';
    files_ui_build(app);
}

void files_ui_build(FilesApp *app)
{
    lv_obj_t *scr = lv_screen_active();
    free(app->rows);
    free(app->row_marks);
    app->rows = NULL;
    app->row_marks = NULL;
    lv_obj_clean(scr);
    int w = app->backend.width > 0 ? app->backend.width : 1280;
    int h = app->backend.height > 0 ? app->backend.height : 720;
    int pad = clampi((w < h ? w : h) * 4 / 100, 8, 28);
    int top_h = clampi(h * 9 / 100, 36, 84);
    int foot_h = clampi(h * 8 / 100, 30, 60);
    int row_h = clampi(h * 8 / 100, 32, 60);
    int gap = clampi(h * 1 / 100, 3, 8);

    lv_obj_t *root = mk_box(scr, 0, 0, w, h, 0, PAL.bg);
    lv_obj_set_style_bg_grad_color(root, lv_color_hex(PAL.bg_bottom), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    app->root = root;

    lv_obj_t *top = mk_box(root, pad, pad, w - pad * 2, top_h, 14, PAL.surface);
    lv_obj_set_style_bg_opa(top, 220, 0);
    lv_obj_set_style_border_width(top, 1, 0);
    lv_obj_set_style_border_color(top, lv_color_hex(PAL.border), 0);
    app->topbar = top;

    const char *title = app->title[0] ? app->title : mode_text(app->mode);
    lv_font_t *title_font = top_h >= 60 ? FONT_LG : FONT_MD;
    lv_obj_t *tl = mk_label(top, title, title_font, PAL.text, 14,
                            top_h >= 60 ? 8 : 4, w - pad * 2 - 180, top_h / 2);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_t *pl = mk_label(top, app->cwd, FONT_XS, PAL.text2, 14,
                            top_h >= 60 ? top_h - 27 : top_h - 18,
                            w - pad * 2 - 180, 18);
    lv_label_set_long_mode(pl, LV_LABEL_LONG_DOT);
    app->path_label = pl;
    app->mode_label = NULL;
    if (w >= 620) {
        app->mode_label = mk_label(top, mode_text(app->mode), FONT_XS, PAL.accent2,
                                   w - pad * 2 - 160, (top_h - 16) / 2, 146, 18);
        lv_obj_set_style_text_align(app->mode_label, LV_TEXT_ALIGN_RIGHT, 0);
    }

    int list_y = pad + top_h + gap;
    int list_h = h - list_y - foot_h - pad - gap;
    if (list_h < 40) list_h = 40;
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, pad, list_y);
    lv_obj_set_size(list, w - pad * 2, list_h);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    app->listbox = list;

    app->row_count = app->list.count;
    app->rows = calloc((size_t)(app->row_count > 0 ? app->row_count : 1), sizeof(lv_obj_t *));
    app->row_marks = calloc((size_t)(app->row_count > 0 ? app->row_count : 1), sizeof(lv_obj_t *));
    for (int i = 0; i < app->row_count; i++)
        add_row(app, list, i, i * (row_h + gap), w - pad * 2, row_h);

    if (app->row_count == 0) {
        mk_label_center(list, "这个目录是空的", FONT_MD, PAL.text3,
                        0, list_h / 2 - 12, w - pad * 2, 24);
    }

    lv_obj_t *foot = mk_box(root, pad, h - pad - foot_h, w - pad * 2, foot_h,
                            foot_h / 2, PAL.surface);
    lv_obj_set_style_bg_opa(foot, 210, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_border_color(foot, lv_color_hex(PAL.border), 0);
    const char *hints;
    if (app->mode == FM_FOLDER)
        hints = w < 520 ? "A 进入  Start 选此目录  B 上级" :
                          "A 进入   Start 选择此目录   B 上级   Y 菜单";
    else if (app->mode == FM_SAVE)
        hints = w < 520 ? "A 进入  Start 保存  B 上级" :
                          "A 进入   Start 保存   B 上级   Y 菜单";
    else if (app->mode == FM_OPEN && app->multiple)
        hints = w < 520 ? "A 标记  Start 确认  B 上级" :
                          "A 标记   X 多选   Start 确认   B 上级   Y 菜单";
    else if (app->mode == FM_OPEN)
        hints = w < 520 ? "A 选择  B 上级" : "A 选择   B 上级   Y 菜单";
    else
        hints = w < 520 ? "A 打开  B 上级  Y 菜单" :
                          "A 打开   B 上级   X 多选   Y 菜单   Start 操作";
    mk_label(foot, hints, FONT_XS, PAL.text2, 14, (foot_h - 16) / 2,
             w - pad * 2 - 28, 18);
    app->status_label = NULL;
    if (w >= 460) {
        app->status_label = mk_label(foot, "", FONT_XS, PAL.text3,
                                     w - pad * 2 - 200, (foot_h - 16) / 2, 186, 18);
        lv_obj_set_style_text_align(app->status_label, LV_TEXT_ALIGN_RIGHT, 0);
    }

    update_row_styles(app);
}

static void text_ok(FilesApp *app);
static void open_text(FilesApp *app, int action, const char *prompt, const char *value);

static void set_busy(FilesApp *app, const char *label);

static void close_modal(FilesApp *app)
{
    if (app->modal) {
        lv_obj_del(app->modal);
        app->modal = NULL;
    }
    app->modal_panel = NULL;
    app->modal_kind = MODAL_NONE;
    app->menu_count = 0;
    app->kb_count = 0;
    backend_text_input(false);
}

void files_ui_close_modal(FilesApp *app) { close_modal(app); }

static void menu_row_clicked(lv_event_t *e)
{
    RowCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    FilesApp *app = ctx->app;
    if (app->modal_focus != ctx->index) {
        app->modal_focus = ctx->index;
        for (int i = 0; i < app->menu_count; i++) {
            lv_obj_t *r = app->menu_rows[i];
            if (!r) continue;
            bool sel = i == app->modal_focus;
            lv_obj_set_style_bg_color(r, lv_color_hex(sel ? PAL.accent : PAL.card), 0);
            lv_obj_set_style_bg_opa(r, sel ? 62 : 55, 0);
            lv_obj_set_style_border_width(r, sel ? 1 : 0, 0);
            lv_obj_set_style_border_color(r, lv_color_hex(PAL.accent2), 0);
        }
        return;
    }
    int action = app->menu_actions[ctx->index];
    close_modal(app);
    extern void files_ui_menu_action(FilesApp *, int);
    files_ui_menu_action(app, action);
}

static void menu_set(FilesApp *app, const char *const *items, const int *acts, int n)
{
    if (n > 12) n = 12;
    app->menu_count = n;
    for (int i = 0; i < n; i++) {
        snprintf(app->menu_storage[i], sizeof(app->menu_storage[i]), "%s", items[i]);
        app->menu_items[i] = app->menu_storage[i];
        app->menu_actions[i] = acts[i];
    }
}

static void build_menu(FilesApp *app)
{
    int w = app->backend.width, h = app->backend.height;
    int pad = 16;
    lv_obj_t *ov = mk_box(lv_screen_active(), 0, 0, w, h, 0, 0x000000);
    lv_obj_set_style_bg_opa(ov, 155, 0);
    app->modal = ov;
    int n = app->menu_count;
    int row_h = clampi(h * 7 / 100, 34, 54);
    int pw = clampi(w - 2 * pad, 240, 620);
    int ph = clampi(n * (row_h + 4) + 52, 120, h - 2 * pad);
    int px = (w - pw) / 2, py = (h - ph) / 2;
    lv_obj_t *panel = mk_box(ov, px, py, pw, ph, 18, PAL.surface);
    lv_obj_set_style_bg_opa(panel, 248, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border_focus), 0);
    app->modal_panel = panel;
    mk_label(panel, "操作", FONT_MD, PAL.text, 16, 10, pw - 32, 24);
    int y = 42;
    for (int i = 0; i < n && y + row_h <= ph - 6; i++) {
        lv_obj_t *r = mk_box(panel, 12, y, pw - 24, row_h, 10, PAL.card);
        lv_obj_set_style_bg_opa(r, i == app->modal_focus ? 62 : 50, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        mk_label(r, app->menu_items[i], FONT_SM, PAL.text, 12, (row_h - 18) / 2,
                 pw - 48, 20);
        RowCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->index = i;
        lv_obj_add_event_cb(r, menu_row_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(r, row_ctx_free, LV_EVENT_DELETE, ctx);
        app->menu_rows[i] = r;
        y += row_h + 4;
    }
    app->modal_kind = MODAL_MENU;
}

static void open_menu(FilesApp *app, const char *const *items, const int *acts, int n)
{
    close_modal(app);
    app->modal_focus = 0;
    menu_set(app, items, acts, n);
    build_menu(app);
}

static void kb_update(FilesApp *app)
{
    for (int i = 0; i < app->kb_count; i++) {
        lv_obj_t *k = app->kb_keys[i];
        if (!k) continue;
        bool sel = i == app->kb_focus;
        lv_obj_set_style_bg_color(k, lv_color_hex(sel ? PAL.accent : PAL.card), 0);
        lv_obj_set_style_bg_opa(k, sel ? 90 : 55, 0);
        lv_obj_set_style_border_width(k, sel ? 1 : 0, 0);
        lv_obj_set_style_border_color(k, lv_color_hex(PAL.accent2), 0);
    }
}

static void text_update(FilesApp *app)
{
    if (!app->modal_panel) return;
    lv_obj_t *lab = lv_obj_get_child(app->modal_panel, 1);
    if (lab) {
        char buf[540];
        snprintf(buf, sizeof(buf), "%s_", app->input);
        lv_label_set_text(lab, buf);
    }
}

static void text_append(FilesApp *app, const char *s)
{
    size_t n = strlen(app->input);
    size_t add = strlen(s);
    if (n + add >= sizeof(app->input)) add = sizeof(app->input) - n - 1;
    memcpy(app->input + n, s, add);
    app->input[n + add] = '\0';
    app->input_len = (int)strlen(app->input);
    text_update(app);
}

static void text_backspace(FilesApp *app)
{
    size_t n = strlen(app->input);
    if (n == 0) return;
    n--;
    while (n > 0 && ((unsigned char)app->input[n] & 0xC0) == 0x80) n--;
    app->input[n] = '\0';
    app->input_len = (int)n;
    text_update(app);
}

static void key_clicked(lv_event_t *e)
{
    RowCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    FilesApp *app = ctx->app;
    if (app->kb_focus != ctx->index) {
        app->kb_focus = ctx->index;
        kb_update(app);
        return;
    }
    int idx = ctx->index;
    if (idx < 0 || idx >= app->kb_count) return;
    /* Find the label from the keyboard row table. */
    int row = idx / 10, col = idx % 10;
    const char *label = (row >= 0 && row < 5 && col >= 0 && col < 10) ? KROWS[row][col] : "";
    if (strcmp(label, "确定") == 0) text_ok(app);
    else if (strcmp(label, "取消") == 0) close_modal(app);
    else if (strcmp(label, "删除") == 0) text_backspace(app);
    else if (strcmp(label, "空格") == 0) text_append(app, " ");
    else text_append(app, label);
}

static void open_text(FilesApp *app, int action, const char *prompt, const char *value)
{
    close_modal(app);
    int w = app->backend.width, h = app->backend.height;
    int pad = 14;
    lv_obj_t *ov = mk_box(lv_screen_active(), 0, 0, w, h, 0, 0x000000);
    lv_obj_set_style_bg_opa(ov, 160, 0);
    app->modal = ov;
    int pw = clampi(w - 2 * pad, 260, 900);
    int ph = clampi(h - 2 * pad, 200, 560);
    int px = (w - pw) / 2, py = (h - ph) / 2;
    lv_obj_t *panel = mk_box(ov, px, py, pw, ph, 18, PAL.surface);
    lv_obj_set_style_bg_opa(panel, 250, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border_focus), 0);
    app->modal_panel = panel;
    mk_label(panel, prompt ? prompt : "输入", FONT_SM, PAL.text2, 16, 10, pw - 32, 22);
    snprintf(app->input, sizeof(app->input), "%s", value ? value : "");
    app->input_len = (int)strlen(app->input);
    app->text_action = action;
    app->kb_focus = 0;
    mk_label(panel, "", FONT_MD, PAL.text, 16, 34, pw - 32, 26);
    text_update(app);

    int cols = 10;
    int gap = 4;
    int kw = (pw - 32 - gap * (cols - 1)) / cols;
    int kh = clampi(ph / 11, 24, 46);
    int ky = 74;
    app->kb_count = 0;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < cols; c++) {
            const char *label = KROWS[r][c];
            if (!label || !*label) continue;
            lv_obj_t *k = mk_box(panel, 16 + c * (kw + gap), ky + r * (kh + gap),
                                 kw, kh, 8, PAL.card);
            lv_obj_set_style_bg_opa(k, 55, 0);
            lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
            lv_font_t *kf = kw < 42 ? FONT_XS : FONT_SM;
            mk_label_center(k, label, kf, PAL.text, 0, (kh - 16) / 2, kw, 18);
            RowCtx *ctx = xmalloc(sizeof(*ctx));
            ctx->app = app;
            ctx->index = app->kb_count;
            lv_obj_add_event_cb(k, key_clicked, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(k, row_ctx_free, LV_EVENT_DELETE, ctx);
            app->kb_keys[app->kb_count++] = k;
        }
    }
    app->modal_kind = MODAL_TEXT;
    kb_update(app);
    backend_text_input(true);
}

void files_ui_show_busy(FilesApp *app, const char *label)
{
    close_modal(app);
    int w = app->backend.width, h = app->backend.height;
    lv_obj_t *ov = mk_box(lv_screen_active(), 0, 0, w, h, 0, 0x000000);
    lv_obj_set_style_bg_opa(ov, 160, 0);
    app->modal = ov;
    int pw = clampi(w - 48, 240, 460);
    int ph = 150;
    lv_obj_t *panel = mk_box(ov, (w - pw) / 2, (h - ph) / 2, pw, ph, 18, PAL.surface);
    lv_obj_set_style_bg_opa(panel, 250, 0);
    mk_label_center(panel, label ? label : "正在处理...", FONT_MD, PAL.text,
                    16, 36, pw - 32, 26);
    mk_label_center(panel, "按 B 取消", FONT_XS, PAL.text2,
                    16, 92, pw - 32, 20);
    app->modal_kind = MODAL_BUSY;
}

static void files_quit(FilesApp *app)
{
    app->running = false;
    app->exit_code = 0;
}

static void paste_confirm(FilesApp *app, int action, bool skip)
{
    (void)action;
    const char *items[3] = {"覆盖", "跳过冲突", "取消"};
    int acts[3] = {M_OVERWRITE, M_SKIP, M_CLOSE};
    app->modal_action = skip ? 1 : 0;
    open_menu(app, items, acts, 3);
}

static char **clip_collect(FilesApp *app, int *out_n)
{
    int n = 0;
    char **v = xmalloc(sizeof(char *) * 256);
    if (app->marked_count > 0) {
        for (int i = 0; i < app->list.count; i++) {
            if (app->marked[i]) {
                char p[PATH_MAX];
                fs_join(p, sizeof(p), app->cwd, app->list.items[i].name);
                v[n++] = xstrdup(p);
            }
        }
    } else if (app->focus >= 0 && app->focus < app->list.count) {
        char p[PATH_MAX];
        fs_join(p, sizeof(p), app->cwd, app->list.items[app->focus].name);
        v[n++] = xstrdup(p);
    }
    *out_n = n;
    return v;
}

static void free_paths(char **v, int n)
{
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

static void copy_selection(FilesApp *app, bool cut)
{
    if (app->list.count == 0) return;
    int n = 0;
    char **v = clip_collect(app, &n);
    if (n == 0) { free(v); return; }
    for (int i = 0; i < app->clip_count; i++) free(app->clip[i]);
    free(app->clip);
    app->clip = v;
    app->clip_count = n;
    app->clip_cut = cut;
    snprintf(app->clip_dir, sizeof(app->clip_dir), "%s", app->cwd);
    snprintf(app->message, sizeof(app->message), cut ? "已剪切 %d 项" : "已复制 %d 项", n);
    update_status(app);
}

static void delete_start(FilesApp *app, bool trash)
{
    int n = 0;
    char **v = clip_collect(app, &n);
    if (n == 0) { free(v); return; }
    int extra = trash ? 3 : 3;
    char **argv = xmalloc(sizeof(char *) * (size_t)(n + extra + 1));
    int k = 0;
    if (trash) {
        argv[k++] = xstrdup("gio");
        argv[k++] = xstrdup("trash");
        argv[k++] = xstrdup("--");
    } else {
        argv[k++] = xstrdup("rm");
        argv[k++] = xstrdup("-rf");
        argv[k++] = xstrdup("--");
    }
    for (int i = 0; i < n; i++) argv[k++] = v[i];
    argv[k] = NULL;
    free(v);
    files_start_op(app, argv, trash ? "正在移到回收站..." : "正在永久删除...", 2);
    for (int i = 0; i < k; i++) free(argv[i]);
    free(argv);
}

static void paste_start(FilesApp *app, bool skip)
{
    if (app->clip_count == 0) {
        snprintf(app->message, sizeof(app->message), "剪贴板为空");
        update_status(app);
        return;
    }
    int n = 0;
    char **v = xmalloc(sizeof(char *) * (size_t)app->clip_count);
    for (int i = 0; i < app->clip_count; i++) {
        const char *base = strrchr(app->clip[i], '/');
        base = base ? base + 1 : app->clip[i];
        char dest[PATH_MAX];
        fs_join(dest, sizeof(dest), app->cwd, base);
        if (skip && fs_exists(dest)) continue;
        v[n++] = xstrdup(app->clip[i]);
    }
    if (n == 0) {
        free(v);
        snprintf(app->message, sizeof(app->message), "全部冲突已跳过");
        update_status(app);
        return;
    }
    int base_n = 3;
    char **argv = xmalloc(sizeof(char *) * (size_t)(n + base_n + 2));
    int k = 0;
    argv[k++] = xstrdup(app->clip_cut ? "mv" : "cp");
    argv[k++] = xstrdup(app->clip_cut ? "-f" : "-a");
    argv[k++] = xstrdup("--");
    for (int i = 0; i < n; i++) argv[k++] = v[i];
    argv[k++] = xstrdup(app->cwd);
    argv[k] = NULL;
    free(v);
    files_start_op(app, argv, app->clip_cut ? "正在移动..." : "正在复制...", 1);
    for (int i = 0; i < k; i++) free(argv[i]);
    free(argv);
}

static bool clip_has_conflict(FilesApp *app)
{
    for (int i = 0; i < app->clip_count; i++) {
        const char *base = strrchr(app->clip[i], '/');
        base = base ? base + 1 : app->clip[i];
        char dest[PATH_MAX];
        fs_join(dest, sizeof(dest), app->cwd, base);
        if (fs_exists(dest)) return true;
    }
    return false;
}

static void show_properties(FilesApp *app)
{
    if (app->list.count == 0) return;
    FsEntry *e = &app->list.items[app->focus];
    char path[PATH_MAX], size[32], tm[32];
    fs_join(path, sizeof(path), app->cwd, e->name);
    fs_format_size(e->size, size, sizeof(size));
    fs_format_time(e->mtime, tm, sizeof(tm));
    const char *items[5];
    static char b0[200], b1[200], b2[200], b3[200];
    snprintf(b0, sizeof(b0), "名称: %s", e->name);
    snprintf(b1, sizeof(b1), "类型: %s", e->is_dir ? "目录" : "文件");
    snprintf(b2, sizeof(b2), "大小: %s", e->is_dir ? "-" : size);
    snprintf(b3, sizeof(b3), "修改: %s", tm);
    items[0] = b0; items[1] = b1; items[2] = b2; items[3] = b3;
    items[4] = "返回";
    int acts[5] = {M_CLOSE, M_CLOSE, M_CLOSE, M_CLOSE, M_CLOSE};
    open_menu(app, items, acts, 5);
}

void files_ui_menu_action(FilesApp *app, int action)
{
    if (action >= M_SHORTCUT) {
        int i = action - M_SHORTCUT;
        if (i >= 0 && i < 16 && app->shortcut_paths[i][0]) {
            snprintf(app->cwd, sizeof(app->cwd), "%s", app->shortcut_paths[i]);
            app->focus = 0;
            files_ui_reload(app);
        }
        return;
    }
    switch (action) {
        case M_CLOSE: close_modal(app); break;
        case M_NEW: open_text(app, 1, "新建文件夹", ""); break;
        case M_OPEN: {
            if (app->list.count == 0) break;
            FsEntry *e = &app->list.items[app->focus];
            if (e->is_dir) break;
            char path[PATH_MAX];
            if (!current_path(app, path, sizeof(path))) break;
            if (!fs_has_default_app(path)) {
                snprintf(app->message, sizeof(app->message),
                         "未安装可用的关联程序");
            } else if (fs_open_default(path)) {
                snprintf(app->message, sizeof(app->message), "正在打开 %s", e->name);
            } else {
                snprintf(app->message, sizeof(app->message),
                         "未安装可用的关联程序");
            }
            update_status(app);
            break;
        }
        case M_RENAME:
            if (app->list.count > 0)
                open_text(app, 2, "重命名", app->list.items[app->focus].name);
            break;
        case M_COPY: copy_selection(app, false); break;
        case M_CUT: copy_selection(app, true); break;
        case M_PASTE:
            if (app->clip_count == 0) {
                snprintf(app->message, sizeof(app->message), "剪贴板为空");
                update_status(app);
            } else if (clip_has_conflict(app)) {
                paste_confirm(app, 0, false);
            } else {
                paste_start(app, false);
            }
            break;
        case M_DELETE: {
            const char *items[3] = {"移到回收站", "永久删除", "取消"};
            int acts[3] = {M_DEL_TRASH, M_DEL_PERM, M_CLOSE};
            open_menu(app, items, acts, 3);
            break;
        }
        case M_DEL_TRASH: delete_start(app, true); break;
        case M_DEL_PERM: delete_start(app, false); break;
        case M_OVERWRITE: paste_start(app, false); break;
        case M_SAVE_OVERWRITE:
            if (app->pending_path[0]) files_select_path(app, app->pending_path);
            break;
        case M_SKIP: paste_start(app, true); break;
        case M_HIDDEN:
            app->show_hidden = !app->show_hidden;
            files_ui_reload(app);
            break;
        case M_SHORTCUTS: {
            char paths[16][PATH_MAX], labels[16][64];
            int n = fs_shortcuts(paths, labels, 16);
            const char *items[16];
            int acts[16];
            for (int i = 0; i < n; i++) {
                snprintf(app->shortcut_paths[i], PATH_MAX, "%s", paths[i]);
                snprintf(app->shortcut_labels[i], 64, "%s", labels[i]);
                items[i] = app->shortcut_labels[i];
                acts[i] = M_SHORTCUT + i;
            }
            items[n] = "取消";
            acts[n] = M_CLOSE;
            open_menu(app, items, acts, n + 1);
            break;
        }
        case M_PROP: show_properties(app); break;
        case M_PATH: open_text(app, 4, "输入路径", app->cwd); break;
        case M_QUIT: files_quit(app); break;
        default: break;
    }
}

static void open_actions(FilesApp *app)
{
    const char *items[11] = {
        "新建文件夹", "打开", "重命名", "复制", "剪切", "粘贴",
        "删除", "显示隐藏文件", "快捷位置", "属性", "退出"
    };
    int acts[11] = {
        M_NEW, M_OPEN, M_RENAME, M_COPY, M_CUT, M_PASTE,
        M_DELETE, M_HIDDEN, M_SHORTCUTS, M_PROP, M_QUIT
    };
    if (app->mode != FM_MANAGER) {
        items[9] = "取消";
        acts[9] = M_CLOSE;
    }
    open_menu(app, items, acts, 10);
}

static void save_check(FilesApp *app)
{
    char name[256];
    snprintf(name, sizeof(name), "%s", app->input);
    char *s = str_trim(name);
    if (!*s) {
        app->message[0] = '\0';
        update_status(app);
        return;
    }
    snprintf(app->save_name, sizeof(app->save_name), "%s", s);
    char path[PATH_MAX];
    fs_join(path, sizeof(path), app->cwd, app->save_name);
    if (fs_exists(path)) {
        snprintf(app->pending_path, sizeof(app->pending_path), "%s", path);
        const char *items[2] = {"覆盖", "取消"};
        int acts[2] = {M_SAVE_OVERWRITE, M_CLOSE};
        open_menu(app, items, acts, 2);
        snprintf(app->message, sizeof(app->message), "文件已存在，是否覆盖？");
        return;
    }
    files_select_path(app, path);
}

static void text_ok(FilesApp *app)
{
    int action = app->text_action;
    char name[256];
    snprintf(name, sizeof(name), "%s", app->input);
    char *s = str_trim(name);
    if (!*s) { close_modal(app); return; }
    if (action == 1) {
        char path[PATH_MAX];
        fs_join(path, sizeof(path), app->cwd, s);
        if (mkdir(path, 0755) != 0)
            snprintf(app->message, sizeof(app->message), "无法创建：%s", strerror(errno));
        close_modal(app);
        files_ui_reload(app);
    } else if (action == 2) {
        if (app->list.count == 0) { close_modal(app); return; }
        char old[PATH_MAX], path[PATH_MAX];
        fs_join(old, sizeof(old), app->cwd, app->list.items[app->focus].name);
        fs_join(path, sizeof(path), app->cwd, s);
        if (rename(old, path) != 0)
            snprintf(app->message, sizeof(app->message), "重命名失败：%s", strerror(errno));
        close_modal(app);
        files_ui_reload(app);
    } else if (action == 3) {
        close_modal(app);
        save_check(app);
    } else if (action == 4) {
        char *expanded = expand_home(s);
        if (fs_is_dir(expanded)) {
            snprintf(app->cwd, sizeof(app->cwd), "%s", expanded);
            app->focus = 0;
            close_modal(app);
            files_ui_reload(app);
        } else {
            app->message[0] = '\0';
            close_modal(app);
            snprintf(app->message, sizeof(app->message), "目录不存在");
            update_status(app);
        }
        free(expanded);
    }
}

static void enter_path(FilesApp *app, const char *path)
{
    if (!fs_is_dir(path)) return;
    snprintf(app->cwd, sizeof(app->cwd), "%s", path);
    app->focus = 0;
    files_ui_reload(app);
}

static void go_up(FilesApp *app)
{
    char parent[PATH_MAX];
    if (!fs_parent(app->cwd, parent, sizeof(parent))) {
        if (app->mode == FM_MANAGER) files_quit(app);
        else files_cancel(app);
        return;
    }
    enter_path(app, parent);
}

static void activate_current(FilesApp *app)
{
    if (app->list.count == 0) {
        if (app->mode == FM_FOLDER) files_select_path(app, app->cwd);
        return;
    }
    char path[PATH_MAX];
    if (!current_path(app, path, sizeof(path))) return;
    FsEntry *e = &app->list.items[app->focus];
    if (e->is_dir) {
        enter_path(app, path);
        return;
    }
    if (app->mode == FM_OPEN) {
        if (app->multiple) {
            app->marked[app->focus] = !app->marked[app->focus];
            app->marked_count += app->marked[app->focus] ? 1 : -1;
            update_row_styles(app);
        } else {
            files_select_path(app, path);
        }
    } else if (app->mode == FM_SAVE) {
        snprintf(app->save_name, sizeof(app->save_name), "%s", e->name);
        app->message[0] = '\0';
        update_status(app);
    }
}

static void finish_picker(FilesApp *app)
{
    if (app->mode == FM_FOLDER) {
        files_select_path(app, app->cwd);
        return;
    }
    if (app->mode == FM_SAVE) {
        snprintf(app->input, sizeof(app->input), "%s", app->save_name);
        open_text(app, 3, "保存文件名", app->save_name);
        return;
    }
    int n = 0;
    char **v = xmalloc(sizeof(char *) * 256);
    if (app->multiple && app->marked_count > 0) {
        for (int i = 0; i < app->list.count; i++) {
            if (app->marked[i]) {
                char p[PATH_MAX];
                fs_join(p, sizeof(p), app->cwd, app->list.items[i].name);
                v[n++] = xstrdup(p);
            }
        }
    } else if (app->focus >= 0 && app->focus < app->list.count &&
               !app->list.items[app->focus].is_dir) {
        char p[PATH_MAX];
        fs_join(p, sizeof(p), app->cwd, app->list.items[app->focus].name);
        v[n++] = xstrdup(p);
    }
    if (n == 0) {
        free(v);
        snprintf(app->message, sizeof(app->message), "请选择文件");
        update_status(app);
        return;
    }
    files_finish_multi(app, v, n);
    free_paths(v, n);
}

static void modal_input(FilesApp *app, InputAction action, const char *text)
{
    if (app->modal_kind == MODAL_BUSY) {
        if (action == IN_CANCEL || action == IN_B || action == IN_PAD_CANCEL) {
            if (app->op_running && app->op_pid > 0) {
                kill(-app->op_pid, SIGTERM);
                snprintf(app->op_label, sizeof(app->op_label), "正在取消...");
                snprintf(app->message, sizeof(app->message), "正在取消操作");
            }
        }
        return;
    }
    if (app->modal_kind == MODAL_MENU) {
        if (action == IN_UP) {
            if (app->modal_focus > 0) app->modal_focus--;
        } else if (action == IN_DOWN) {
            if (app->modal_focus + 1 < app->menu_count) app->modal_focus++;
        } else if (action == IN_CONFIRM || action == IN_PAD_CONFIRM || action == IN_A) {
            if (app->modal_focus >= 0 && app->modal_focus < app->menu_count) {
                int act = app->menu_actions[app->modal_focus];
                close_modal(app);
                files_ui_menu_action(app, act);
            }
            return;
        } else if (action == IN_CANCEL || action == IN_B || action == IN_PAD_CANCEL) {
            close_modal(app);
            return;
        } else {
            return;
        }
        build_menu(app);
        return;
    }
    if (app->modal_kind == MODAL_TEXT) {
        if (action == IN_TEXT && text) { text_append(app, text); return; }
        if (action == IN_BACKSPACE) { text_backspace(app); return; }
        if (action == IN_LEFT || action == IN_RIGHT || action == IN_UP || action == IN_DOWN) {
            if (app->kb_count <= 0) return;
            if (action == IN_LEFT) app->kb_focus = (app->kb_focus - 1 + app->kb_count) % app->kb_count;
            else if (action == IN_RIGHT) app->kb_focus = (app->kb_focus + 1) % app->kb_count;
            else if (action == IN_UP) app->kb_focus = clampi(app->kb_focus - 10, 0, app->kb_count - 1);
            else app->kb_focus = clampi(app->kb_focus + 10, 0, app->kb_count - 1);
            kb_update(app);
            return;
        }
        if (action == IN_CONFIRM) { text_ok(app); return; }
        if (action == IN_CANCEL || action == IN_B || action == IN_PAD_CANCEL) {
            close_modal(app);
            return;
        }
        if (action == IN_PAD_CONFIRM || action == IN_A) {
            if (app->kb_focus >= 0 && app->kb_focus < app->kb_count) {
                int row = app->kb_focus / 10, col = app->kb_focus % 10;
                const char *label = KROWS[row][col];
                if (strcmp(label, "确定") == 0) text_ok(app);
                else if (strcmp(label, "取消") == 0) close_modal(app);
                else if (strcmp(label, "删除") == 0) text_backspace(app);
                else if (strcmp(label, "空格") == 0) text_append(app, " ");
                else text_append(app, label);
            }
        }
        return;
    }
}

void files_ui_input(FilesApp *app, InputAction action, const char *text)
{
    if (action == IN_NONE) return;
    if (app->op_running) {
        if (action == IN_CANCEL || action == IN_B || action == IN_PAD_CANCEL) {
            if (app->op_pid > 0) kill(-app->op_pid, SIGTERM);
            snprintf(app->op_label, sizeof(app->op_label), "正在取消...");
            snprintf(app->message, sizeof(app->message), "正在取消操作");
        }
        return;
    }
    if (app->modal_kind != MODAL_NONE) { modal_input(app, action, text); return; }

    if (action == IN_QUIT) { files_quit(app); return; }
    if (action == IN_CANCEL || action == IN_B || action == IN_PAD_CANCEL) {
        go_up(app);
        return;
    }
    if (action == IN_UP || action == IN_SCROLL_UP) {
        if (app->focus > 0) { app->focus--; app->message[0] = '\0'; update_row_styles(app); }
        return;
    }
    if (action == IN_DOWN || action == IN_SCROLL_DOWN) {
        if (app->focus + 1 < app->list.count) { app->focus++; app->message[0] = '\0'; update_row_styles(app); }
        return;
    }
    if (action == IN_LEFT || action == IN_LT) {
        if (app->focus > 0) { app->focus = clampi(app->focus - 10, 0, app->list.count - 1); update_row_styles(app); }
        return;
    }
    if (action == IN_RIGHT || action == IN_RT) {
        if (app->focus + 10 < app->list.count) { app->focus = clampi(app->focus + 10, 0, app->list.count - 1); update_row_styles(app); }
        return;
    }
    if (action == IN_X) {
        if (app->list.count > 0) {
            app->marked[app->focus] = !app->marked[app->focus];
            app->marked_count += app->marked[app->focus] ? 1 : -1;
            update_row_styles(app);
        }
        return;
    }
    if (action == IN_Y || action == IN_SELECT) { open_actions(app); return; }
    if (action == IN_START) {
        if (app->mode == FM_MANAGER) { open_actions(app); return; }
        finish_picker(app);
        return;
    }
    if (action == IN_CONFIRM || action == IN_A || action == IN_PAD_CONFIRM) {
        activate_current(app);
        return;
    }
}

void files_ui_tick(FilesApp *app)
{
    (void)app;
}

void files_ui_free(FilesApp *app)
{
    free_marks(app);
    for (int i = 0; i < app->clip_count; i++) free(app->clip[i]);
    free(app->clip);
    app->clip = NULL;
    app->clip_count = 0;
    free(app->rows);
    free(app->row_marks);
    app->rows = NULL;
    app->row_marks = NULL;
}
