// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "editor.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

void ui_build_editor(App *app)
{
    UiLayout *L = &app->layout;
    EditorState *ed = &app->editor;
    if (!ed->active) return;
    ui_screen_background(app, NULL, PAL.accent);

    int x = L->pad;
    int w = L->w - L->pad * 2;
    if (w < 80) w = 80;
    int y = L->content_y - 4;
    if (y < L->top_y + L->top_h) y = L->top_y + L->top_h + 6;

    int title_h = ui_text_height(FONT_LG);
    int small_h = ui_text_height(FONT_XS);

    char title[200];
    snprintf(title, sizeof(title), "%s%s", ed->name[0] ? ed->name : "未命名",
             ed->dirty ? "  *" : "");
    ui_label(app->ui.root, title, FONT_LG, PAL.text, x, y, w - 220, title_h);

    const char *status = ed->editing ? "编辑中" :
                         (ed->dirty ? "未保存" :
                          (ed->status[0] ? ed->status : "已保存"));
    lv_obj_t *st = ui_label(app->ui.root, status, FONT_XS,
                            ed->editing ? PAL.accent2 :
                            (ed->dirty ? PAL.warning : PAL.success),
                            x + w - 200, y + 4, 200, small_h);
    lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_RIGHT, 0);

    char sub[PATH_MAX + 96];
    snprintf(sub, sizeof(sub), "%s  /  %s  /  %d 行", ed->path,
             editor_format_label(ed->path), ed->count);
    lv_obj_t *sl = ui_label(app->ui.root, sub, FONT_XS, PAL.text3,
                            x, y + title_h, w, small_h);
    lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);

    int list_y = y + title_h + small_h + 10;
    int list_h = L->footer_y - list_y - 8;
    if (list_h < 60) list_h = 60;
    if (ed->editing) {
        /* Keep the caret area above the on-screen keyboard. */
        int keep_h = L->h * 58 / 100 - list_y;
        if (keep_h < 70) keep_h = 70;
        if (list_h > keep_h) list_h = keep_h;
    }

    lv_obj_t *panel = ui_rect(app->ui.root, x, list_y, w, list_h, 14, PAL.surface);
    lv_obj_set_style_bg_opa(panel, 220, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border), 0);
    lv_obj_set_style_border_opa(panel, 120, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    int line_h = ui_unscale_y(lv_font_get_line_height(FONT_SM));
    int row_h = ui_clamp(line_h + 12, 24, 40);
    int rows = (list_h - 12) / row_h;
    if (rows < 1) rows = 1;
    int top = ed->focus - rows / 2;
    if (top < 0) top = 0;
    if (top + rows > ed->count) top = ed->count - rows;
    if (top < 0) top = 0;
    ed->top = top;

    int num_w = ui_clamp(w * 7 / 100, 34, 64);
    int text_h = ui_text_height(FONT_SM);
    int caret_y = -1, caret_x = -1, caret_h = 0;
    for (int i = top; i < top + rows && i < ed->count; i++) {
        lv_obj_t *r = ui_rect(panel, 8, 6 + (i - top) * row_h, w - 16, row_h - 2,
                              6, PAL.card);
        lv_obj_set_style_bg_opa(r, 40, 0);

        char num[16];
        snprintf(num, sizeof(num), "%d", i + 1);
        lv_obj_t *nl = ui_label(r, num, FONT_XS, PAL.text3, 6,
                                (row_h - 2 - ui_text_height(FONT_XS)) / 2,
                                num_w - 10, ui_text_height(FONT_XS));
        lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_RIGHT, 0);

        const char *line = ed->lines[i] && ed->lines[i][0] ? ed->lines[i] : " ";
        int label_y = (row_h - 2 - text_h) / 2;
        lv_obj_t *tl = ui_label(r, line, FONT_SM, PAL.text2, num_w, label_y,
                                w - 16 - num_w - 8, text_h);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);

        if (i == ed->focus) {
            int col = ed->col;
            int len = ed->lines[i] ? (int)strlen(ed->lines[i]) : 0;
            if (col < 0) col = 0;
            if (col > len) col = len;
            caret_x = ui_scale_x(8 + num_w) +
                      lv_text_get_width(line, (uint32_t)col, FONT_SM, 0);
            caret_y = ui_scale_y(6 + (i - top) * row_h + label_y);
            caret_h = ui_scale_y(text_h);
        }
    }
    if (caret_x >= 0 && caret_y >= 0) {
        lv_obj_t *caret = lv_obj_create(panel);
        lv_obj_remove_style_all(caret);
        lv_obj_set_pos(caret, caret_x, caret_y);
        lv_obj_set_size(caret, ui_scale_x(2), caret_h);
        lv_obj_set_style_bg_color(caret, lv_color_hex(PAL.accent), 0);
        lv_obj_set_style_bg_opa(caret, 235, 0);
        lv_obj_set_style_radius(caret, ui_scale_x(1), 0);
        lv_obj_remove_flag(caret, LV_OBJ_FLAG_SCROLLABLE);
    }

    char foot_title[200];
    snprintf(foot_title, sizeof(foot_title), "%s%s",
             ed->name[0] ? ed->name : "文本编辑器", ed->dirty ? " *" : "");
    ui_footer_pill(app, foot_title, ed->count,
                   "A 编辑", "B 返回", "X 换行", "Y 退格");
}
