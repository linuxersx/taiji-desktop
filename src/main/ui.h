// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_UI_H
#define TAIJI_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <lvgl.h>

struct App;

typedef struct {
    int w, h, pad;
    int top_x, top_y, top_w, top_h;
    int nav_w, nav_gap, status_w;
    int footer_x, footer_y, footer_w, footer_h;
    int content_y, content_h, header_h;
    int home_cols, home_rows, home_gap, home_card_w, home_card_h, home_page;
    int game_cols, game_rows, game_page, game_title_h;
    int game_grid_x, game_grid_y, game_grid_w, game_grid_h;
    int app_cols, app_rows, app_page, app_card_w, app_card_h, app_grid_x, app_grid_y, app_grid_w, app_grid_h;
    int app_cat_y, app_cat_h;
    int panel_x, panel_y, panel_w, panel_h;
    int rail_x, rail_y, rail_w, rail_h, rail_row_h, rail_gap;
    int settings_panel_x, settings_panel_y, settings_panel_w, settings_panel_h;
    int settings_row_h, settings_rows, settings_header_h;
    int font_scale_x100;
} UiLayout;

void ui_layout_update(struct App *app);
int ui_clamp(int v, int lo, int hi);
int ui_min(int a, int b);
int ui_scale_x(int v);
void ui_set_draw_scale(float scale);
int ui_scale_y(int v);
int ui_text_height(lv_font_t *font);
int ui_unscale_x(int v);
int ui_unscale_y(int v);

typedef struct {
    lv_obj_t *root;
    lv_obj_t *bg_img;
    lv_obj_t *topbar;
    lv_obj_t *footer;
    lv_obj_t *toast;

    lv_obj_t *home_cards[64];
    int home_card_count;
    int home_page_start;
    lv_obj_t *home_page_label;
    lv_obj_t *footer_title_label;
    lv_obj_t *footer_count_label;

    lv_obj_t *game_cards[64];
    lv_obj_t *game_titles[64];
    int game_card_count;
    int game_page_start;
    lv_obj_t *game_page_label;
    lv_obj_t *game_header_logo;
    lv_obj_t *game_header_title;
    lv_obj_t *game_header_count;
    lv_obj_t *game_cover;
    lv_obj_t *game_detail_title;
    lv_obj_t *game_detail_meta;
    lv_obj_t *game_detail_desc;
    lv_obj_t *game_detail_stats;
    lv_obj_t *game_fav_badge;
    lv_obj_t *game_fav_label;

    lv_obj_t *nav_items[8];
    int nav_screens[8];
    lv_obj_t *nav_scroll;
    lv_obj_t *nav_app_badge;
    lv_obj_t *nav_store_badge;
    int nav_count;
    lv_obj_t *app_grid;
    lv_obj_t *app_cards[64];
    lv_obj_t *app_titles[64];
    lv_obj_t *app_status[64];
    int app_card_indices[64];
    int app_card_count;
    int app_page_start;
    lv_obj_t *app_cat_tabs[16];
    int app_cat_count;
    lv_obj_t *power_buttons[4];
    int power_button_count;
    lv_obj_t *control_rows[64];
    int control_row_count;
    lv_obj_t *control_wifi_value;
    lv_obj_t *control_battery_value;
    lv_obj_t *control_volume_value;
    lv_obj_t *control_volume_icon;
    lv_obj_t *control_volume_bar;
    lv_obj_t *control_volume_slider;
    int control_volume_bar_full;
    int control_volume_track_x;
    int control_volume_track_w;
    int control_volume_knob_size;
    lv_obj_t *control_brightness_value;
    lv_obj_t *control_brightness_icon;
    lv_obj_t *control_brightness_bar;
    lv_obj_t *control_brightness_slider;
    int control_brightness_bar_full;
    int control_brightness_track_x;
    int control_brightness_track_w;
    int control_brightness_knob_size;
    lv_obj_t *status_items[5];
    int status_kinds[5];
    int status_item_count;
    lv_obj_t *files_rows[128];
    lv_obj_t *files_marks[128];
    int files_row_count;
    lv_obj_t *files_side_rows[48];
    int files_side_row_count;
    lv_obj_t *files_side_panel;
    lv_obj_t *files_eye_btn;
    lv_obj_t *files_refresh_btn;
    lv_obj_t *files_path_label;
    lv_obj_t *files_status_label;
    lv_obj_t *app_page_label;
    lv_obj_t *app_grid_title;
    lv_obj_t *app_grid_count;
    lv_obj_t *app_edit_list;
    lv_obj_t *app_edit_rows[11];
    lv_obj_t *app_edit_values[11];
    int app_edit_row_count;

    lv_obj_t *settings_rail[16];
    lv_obj_t *settings_rail_labels[16];
    int settings_rail_count;
    lv_obj_t *settings_rows[40];
    lv_obj_t *settings_hint_label;
    int settings_row_count;
    int app_focus_drawn;
    lv_obj_t *app_focus_ring;
    lv_obj_t *settings_title;
    lv_obj_t *settings_subtitle;
    lv_obj_t *settings_license_body;

    lv_obj_t *modal;
    lv_obj_t *overview_stage;
    lv_obj_t *overview_main_card;
    lv_obj_t *overview_cards[16];
    int overview_card_count;
    lv_obj_t *modal_input;
    lv_obj_t *modal_panel;
    lv_obj_t *modal_list[16];
    lv_obj_t *modal_scroll;
    lv_obj_t *keyboard;
    lv_obj_t *modal_anim_target;
    int modal_list_count;
    lv_obj_t *mapping_label;
    lv_obj_t *clock_label;
    lv_obj_t *wifi_label;
    lv_obj_t *battery_label;
    lv_obj_t *battery_percent;
} Ui;

void ui_init(Ui *ui);
void ui_build(struct App *app);
void ui_update(struct App *app);
void ui_build_modal(struct App *app);
void ui_refresh_modal(struct App *app);
void ui_ime_show(struct App *app);
void ui_ime_hide(struct App *app);
void ui_ime_raise(struct App *app);
void ui_ime_input(struct App *app, InputAction action);
void ui_close_modal_visual(struct App *app);
void ui_toast(struct App *app, const char *text);
void ui_tick(struct App *app);
void ui_update_status(struct App *app);
void ui_update_status_focus(struct App *app);
void ui_update_nav_focus(struct App *app);
void ui_update_app_badge(struct App *app);
void ui_update_store_badge(struct App *app);
void ui_update_volume_modal(struct App *app);
void ui_update_brightness_modal(struct App *app);
void ui_fade_in_modal(struct App *app);
void ui_fade_out_modal(struct App *app);
void ui_set_input_active(bool active);
void ui_set_external_osk_mode(bool enabled);
lv_obj_t *ui_modal_panel(struct App *app, int w, int h);
void ui_draw_spinner_overlay(struct App *app, const char *title, const char *detail);

/* helpers shared by ui_*.c */
lv_obj_t *ui_obj(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color);
lv_obj_t *ui_rect(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color);
lv_obj_t *ui_gradient(lv_obj_t *parent, int x, int y, int w, int h, int radius,
                      uint32_t top, uint32_t bottom);
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, lv_font_t *font,
                   uint32_t color, int x, int y, int w, int h);
lv_obj_t *ui_label_center(lv_obj_t *parent, const char *text, lv_font_t *font,
                          uint32_t color, int x, int y, int w, int h);
lv_obj_t *ui_image(lv_obj_t *parent, const char *path, int x, int y, int w, int h,
                   int radius, bool cover);
lv_obj_t *ui_round_panel(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color);
void ui_set_focus_ring(lv_obj_t *obj, bool focused, uint32_t accent);
uint32_t ui_focus_accent(int index);
void ui_set_card_focus(lv_obj_t *obj, bool focused, uint32_t accent);
/* Scroll obj into view only when it is actually outside the parent's content
 * area. During ui_build() the animation is forced off so a rebuilt list does
 * not visibly scroll back from the top. */
void ui_scroll_into_view(lv_obj_t *obj, bool allow_anim);
void ui_scrollbar_style(lv_obj_t *obj);
char *ui_img_src(const char *path);
void ui_screen_background(struct App *app, const char *cover_path, uint32_t accent);
void ui_topbar(struct App *app);
void ui_button_hint(struct App *app, char *out, size_t out_size, const char *fmt);
void ui_footer_pill(struct App *app, const char *title, int count,
                    const char *a, const char *b, const char *x, const char *y);
lv_obj_t *ui_click_card(lv_obj_t *parent, int x, int y, int w, int h, int radius,
                        uint32_t color, void (*cb)(lv_event_t *), void *user);
void ui_apply_theme_to_screen(struct App *app);
void ui_draw_spinner_overlay(struct App *app, const char *title, const char *detail);

/* screen builders */
void ui_build_home(struct App *app);
void ui_build_overview(struct App *app);
void ui_build_games(struct App *app, const char *collection_id);
void ui_build_settings(struct App *app);
void ui_build_files(struct App *app);
void ui_build_editor(struct App *app);
void ui_build_store(struct App *app);
void ui_update_files(struct App *app);
void ui_update_home(struct App *app);
void ui_update_games(struct App *app);
void ui_update_settings(struct App *app);
void ui_build_modal_search(struct App *app);
void ui_build_modal_detail(struct App *app);
void ui_build_modal_text(struct App *app);
void ui_build_modal_confirm(struct App *app);
void ui_build_modal_choice(struct App *app);
void ui_build_modal_menu(struct App *app);
void ui_build_modal_roots(struct App *app);
void ui_build_modal_mapping(struct App *app);
void ui_build_modal_help(struct App *app);
void ui_build_modal_editor_help(struct App *app);
void ui_build_modal_smb_share(struct App *app);
void ui_build_modal_scan(struct App *app);
void ui_build_apps(struct App *app);
void ui_update_apps(struct App *app);
void ui_build_modal_app(struct App *app);
void ui_update_modal_app(struct App *app);
void ui_build_modal_control(struct App *app);
void ui_build_modal_power(struct App *app);
void ui_update_power_focus(struct App *app);
void ui_build_modal_gamepad(struct App *app);
void ui_gamepad_poll(struct App *app);
void ui_refresh_control_modal(struct App *app);

#endif

void ui_footer_update(struct App *app, const char *title, int count);

void ui_splash(struct App *app, const char *message);
