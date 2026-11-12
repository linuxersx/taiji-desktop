// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_APP_H
#define TAIJI_APP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#include "apps.h"
#include "audio.h"
#include "backend.h"
#include "bluetooth.h"
#include "config.h"
#include "launcher.h"
#include "gpu.h"
#include "network.h"
#include "perf.h"
#include "system_status.h"
#include "system_volume.h"
#include "library.h"
#include "files.h"
#include "tasks.h"
#include "store.h"
#include "icon_index.h"
#include "appstream_index.h"
#include "ui.h"

typedef enum {
    SCREEN_HOME = 0,
    SCREEN_GAMES,
    SCREEN_FAVORITES,
    SCREEN_RECENT,
    SCREEN_APPS,
    SCREEN_STORE,
    SCREEN_FILES,
    SCREEN_SETTINGS,
    SCREEN_OVERVIEW,
    SCREEN_EDITOR,
} Screen;

typedef enum {
    FILES_PICK_NONE = 0,
    FILES_PICK_DIR,
    FILES_PICK_FILE,
} FilesPickMode;

#define FILE_LOC_MAX 48
#define FILE_SHARE_MAX 16

typedef enum {
    FILE_LOC_PATH = 0,
    FILE_LOC_DEVICE,
    FILE_LOC_SHARE,
    FILE_LOC_CONNECT,
    FILE_LOC_MTP,        /* USB MTP 设置入口 */
    FILE_LOC_MANAGE,     /* 共享管理入口 */
} FileLocKind;

typedef struct {
    FileLocKind kind;
    int section; /* 0 favourites, 1 locations, 2 shares */
    char label[128];
    char path[PATH_MAX];
    char device[64];
    char uri[512];
    char user[96];
    bool mounted;
    bool removable;
} FileLoc;

typedef struct {
    char label[128];
    char uri[512];
    char user[96];
} FileShare;

typedef enum {
    STATUS_WIFI = 0,
    STATUS_BATTERY,
    STATUS_VOLUME,
    STATUS_CLOCK,
    STATUS_PAD,
} StatusKind;

typedef enum {
    MODAL_NONE = 0,
    MODAL_SEARCH,
    MODAL_DETAIL,
    MODAL_TEXT,
    MODAL_CONFIRM,
    MODAL_CHOICE,
    MODAL_MENU,
    MODAL_ROOTS,
    MODAL_MAPPING,
    MODAL_HELP,
    MODAL_EDITOR_HELP,
    MODAL_SCAN,
    MODAL_APP,
    MODAL_CONTROL,
    MODAL_POWER,
    MODAL_GAMEPAD,
    MODAL_SMB_SHARE,
    MODAL_OOBE_DATE,
} ModalKind;

typedef enum {
    TEXT_ROOT = 0,
    TEXT_COMMAND,
    TEXT_GAME_COMMAND,
    TEXT_APP_NAME,
    TEXT_APP_COMMAND,
    TEXT_APP_ICON,
    TEXT_WIFI_PASSWORD,
    TEXT_FILE_NAME,
    TEXT_SMB_HOST,
    TEXT_SMB_SHARE,
    TEXT_SMB_USER,
    TEXT_SMB_PASS,
    TEXT_AUTO_DARK,
    TEXT_AUTO_LIGHT,
    TEXT_TIMEZONE,
    TEXT_EXTRACT_DIR,
    TEXT_EDITOR_LINE,
    TEXT_SMB_SHARE_PATH,
    TEXT_SMB_SHARE_NAME,
    TEXT_SMB_AUTH_USER,
    TEXT_SMB_AUTH_PASS,
    TEXT_OOBE_ROM,
} TextPurpose;

typedef enum {
    CHOICE_THEME = 0,
    CHOICE_COLUMNS,
    CHOICE_SORT,
    CHOICE_FILTER,
    CHOICE_UI_SCALE,
    CHOICE_RENDERER,
    CHOICE_LANGUAGE,
    CHOICE_TIMEZONE,
    CHOICE_ARCHIVE_FORMAT,
    CHOICE_OPEN_WITH,
    CHOICE_EXTRACT_TARGET,
    CHOICE_CORE,
} ChoicePurpose;

typedef enum {
    CORE_ITEM_AUTO = 0,
    CORE_ITEM_CORE,
    CORE_ITEM_ALL,
    CORE_ITEM_MANUAL,
    CORE_ITEM_BACK,
} CoreItemKind;

typedef struct {
    char label[160];
    char core[64];
    char path[512];
    CoreItemKind kind;
    bool compatible;
} CoreChoiceItem;

typedef enum {
    CONFIRM_QUIT = 0,
    CONFIRM_REMOVE_ROOT,
    CONFIRM_RESCAN,
    CONFIRM_RESET_MAP,
    CONFIRM_DELETE_APP,
    CONFIRM_PAUSE_LIMIT,
    CONFIRM_PASTE_OVERWRITE,
    CONFIRM_FILE_DELETE_PERM,
    CONFIRM_EDITOR_DISCARD,
    CONFIRM_RERUN_OOBE,
    CONFIRM_RESET_TAIJI,
} ConfirmPurpose;

typedef enum {
    MENU_SEARCH = 0,
    MENU_SCAN,
    MENU_ADD_ROOT,
    MENU_SORT,
    MENU_FILTER,
    MENU_THEME,
    MENU_SETTINGS,
    MENU_HELP,
    MENU_ADD_APP,
    MENU_IMPORT_DESKTOP,
    MENU_FILES,
    MENU_FILE_NEW,
    MENU_FILE_OPEN,
    MENU_FILE_EXTRACT,
    MENU_FILE_COMPRESS,
    MENU_FILE_RENAME,
    MENU_FILE_COPY,
    MENU_FILE_CUT,
    MENU_FILE_PASTE,
    MENU_FILE_TRASH,
    MENU_FILE_DELETE,
    MENU_FILE_HIDDEN,
    MENU_FILE_HOME,
    MENU_FILE_ROOT,
    MENU_FILE_OPEN_WITH,
    MENU_FILE_FAVORITE,
    MENU_FILE_EDIT,
    MENU_FILE_NEW_TEXT,
    MENU_FILE_SHARE,
    MENU_FILE_PROPERTIES,
    MENU_QUIT,
    MENU_BACK,
} MenuAction;

typedef struct {
    char id[80];
    char name[160];
    char short_name[40];
    char logo[1024];
    char cover[1024];
    int count;
    uint32_t accent;
} Collection;

typedef enum {
    OOBE_STEP_WELCOME = 0,
    OOBE_STEP_LANGUAGE,
    OOBE_STEP_TIMEZONE,
    OOBE_STEP_THEME,
    OOBE_STEP_NETWORK,
    OOBE_STEP_GAMES,
    OOBE_STEP_OTHER,
    OOBE_STEP_COUNT
} OobeStep;

#define SMB_SCAN_MAX 64
#define SMB_SCAN_SHARES 16
#define SMB_LOCAL_MAX 64

typedef struct {
    char addr[64];
    char name[96];
    int share_count;
    char shares[SMB_SCAN_SHARES][96];
} SmbScannedHost;

typedef struct {
    char name[96];
    char path[PATH_MAX];
    bool writable;
} SmbLocalShare;

typedef enum {
    FILES_PANEL_LIST = 0,
    FILES_PANEL_CONNECT,
    FILES_PANEL_MANAGE,
} FilesPanel;

typedef struct {
    int panel;
    int focus;
    int view;                 /* connect panel: 0 servers, 1 shares */
    bool scan_running;
    bool scan_done;
    bool scan_ok;
    char scan_msg[192];
    SmbScannedHost hosts[SMB_SCAN_MAX];
    int host_count;
    int host_sel;
    int share_sel;
    int share_count;
    char shares[SMB_SCAN_SHARES][96];
    SmbLocalShare local[SMB_LOCAL_MAX];
    int local_count;
    bool service_on;
    bool helper_ok;
    char service_msg[128];
    int edit_index;           /* local share being edited, -1 = new */
    int edit_focus;
    char pending_path[PATH_MAX];
    bool pending_writable;
    char user[128];
    char pass[256];
    char auth_host[64];
    bool auth_required;
} SmbUi;

#define EDITOR_MAX_LINES 20000
#define EDITOR_LINE_MAX 2000

typedef struct {
    char path[PATH_MAX];
    char name[160];
    char **lines;
    int count;
    int cap;
    int focus;
    int col;
    int want_col;
    int top;
    bool dirty;
    bool active;
    bool editing;
    char status[160];
} EditorState;

typedef struct App {
    Backend backend;
    Config config;
    Library library;
    AppList apps;
    AppRuntime runtime;
    SystemStatus sys;
    SystemVolume sysvol;
    WifiState wifi;
    BtState bt;
    PerfState perf;
    GpuState gpu;
    int control_focus;
    int control_col;
    int control_radio;
    bool control_expanded;
    int active_theme;
    uint32_t theme_auto_check;
    int power_module;
    int power_focus;
    int control_view;
    bool control_closing;
    bool control_osd;
    uint32_t control_osd_until;
    bool status_on_bar;
    int status_focus;
    bool control_return_status;
    int control_return_status_focus;
    uint32_t control_opened_at;
    char wifi_connect_ssid[128];
    Launcher launcher;
    Ui ui;
    UiLayout layout;
    TaskList tasks;
    int overview_focus;
    Screen overview_return_screen;
    bool overview_animate;
    int overview_slide_dir;   /* -1 enter from left, +1 enter from right */
    bool overview_kill_armed;
    int overview_kill_index;
    int overview_close_pending;  /* index queued after the swipe-up animation */
    int pending_input_action;    /* gesture-generated input, run from ui_tick */
    int pending_overview_focus;  /* gesture-selected card, run from ui_tick */
    int pending_overview_activate;
    bool pending_overview_close;
    uint32_t overview_kill_at;

    Screen screen;
    ModalKind modal;
    bool scan_pending;
    bool scan_running;
    volatile bool scan_done;
    bool scan_ok;
    pthread_t scan_thread;
    Library scan_result;
    bool desktop_scan_running;
    volatile bool desktop_scan_done;
    bool desktop_scan_notify;
    int desktop_added, desktop_updated, desktop_removed;
    pthread_t desktop_scan_thread;
    AppList desktop_scan_result;
    bool dirty;

    int home_focus;
    int home_page;

    char collection_id[80];
    char collection_name[160];
    char selected_game_id[80];
    int game_focus;
    int game_page;
    int app_focus;
    int app_focus_want;
    int app_page;
    bool app_badge;
    bool apps_refresh_pending;
    uint32_t desktop_recheck_at;
    int app_cat_focus;
    bool app_on_categories;
    bool app_reset_focus;
    bool apps_visited;
    bool nav_on_bar;
    bool modal_closing;
    int nav_focus;
    bool ime_visible;
    int ext_osk_fd;
    bool ext_osk_attached;
    char ext_osk_buf[1024];
    size_t ext_osk_len;
    bool ime_shift;
    bool ime_handling;
    char app_cat[32];
    int app_filter[64];
    int app_filter_count;
    int app_edit_index;
    int app_edit_row;
    int app_edit_page;
    bool app_edit_from_text;
    bool ui_suspended;
    bool external_paused;
    int sort_mode;
    int filter_mode;
    int grid_columns;

    int settings_cat;
    int settings_row;
    bool settings_on_rail;
    bool settings_about;
    bool settings_license;
    int settings_page;

    int menu_focus;
    bool menu_info;
    char file_info_name[256];
    char file_info_type[32];
    char file_info_size[32];
    char file_info_time[64];
    char file_info_path[PATH_MAX];
    int choice_focus;
    CoreChoiceItem core_items[64];
    int core_item_count;
    char core_platform_id[80];
    char core_pending_game_id[80];
    bool core_picker_launch;
    bool core_picker_all;
    bool core_scrolled;
    int confirm_focus;
    int roots_focus;
    char modal_title[160];
    char modal_body[1024];
    char modal_hint[512];
    char input_buf[2048];
    int input_cursor;
    TextPurpose text_purpose;
    ChoicePurpose choice_purpose;
    ConfirmPurpose confirm_purpose;
    int modal_arg;

    char search_buf[256];
    int search_focus;

    int mapping_step;
    int mapping_values[4];

    char toast_text[512];
    uint32_t toast_until;

    /* previous state for launcher bookkeeping */
    char active_game_id[80];
    char active_app_id[64];
    char pending_app_id[64];
    char pending_evict_id[64];

    char files_cwd[PATH_MAX];
    FsList files_list;
    int files_focus;
    bool *files_marked;
    int files_marked_count;
    bool files_show_hidden;
    bool files_loaded;
    EditorState editor;
    SmbUi smb_ui;
    bool config_missing;
    bool oobe;
    int oobe_step;
    int oobe_focus;
    bool oobe_finishing;
    int oobe_tz;
    int oobe_date_field;
    int oobe_date[5];
    int oobe_tz_scroll;
    int oobe_content_scroll;
    int oobe_last_step;
    char oobe_rom[PATH_MAX];
    FilesPickMode files_pick;
    FilesPanel files_panel;
    Screen files_pick_return;
    FileLoc files_locs[FILE_LOC_MAX];
    int files_loc_count;
    int files_side_focus;
    bool files_on_sidebar;
    bool files_sidebar_open;
    bool files_mount_running;
    volatile bool files_mount_done;
    bool files_mount_ok;
    bool files_mount_unmount;
    char files_mount_device[64];
    char files_mount_path[PATH_MAX];
    char files_mount_msg[256];
    bool files_mtp_running;
    volatile bool files_mtp_done;
    int files_mtp_result;
    bool files_mtp_panel;
    int files_mtp_focus;
    FileShare shares[FILE_SHARE_MAX];
    int share_count;
    bool smb_running;
    volatile bool smb_done;
    bool smb_ok;
    char smb_path[PATH_MAX];
    char smb_msg[256];
    char smb_host[256];
    char smb_share[128];
    char smb_user[128];
    char smb_pass[256];
    char **files_clip;
    int files_clip_count;
    bool files_clip_cut;
    bool files_op_running;
    volatile bool files_op_done;
    volatile int files_op_result;
    pid_t files_op_pid;
    char files_message[160];
    char extract_archive[PATH_MAX];

    StorePackageList store_all;
    StorePackageList store_refresh_result;
    int *store_filter;
    int store_filter_count;
    int store_focus;
    int store_side_focus;
    int store_category;
    bool store_settings;
    int store_settings_focus;
    bool store_dropdown_open;
    int store_dropdown_focus;
    int store_scroll_pending;
    int store_view_row;
    int store_visible_rows;
    int store_update_count;
    int store_detail_index;
    int store_cols;
    int store_first;
    bool store_on_sidebar;
    bool store_loaded;
    bool store_detail;
    bool store_search_edit;
    bool store_refreshing;
    volatile bool store_refresh_done;
    volatile bool store_refresh_ok;
    volatile bool store_refresh_cancel;
    pthread_t store_thread;
    char store_error[256];
    bool store_task_running;
    volatile bool store_task_done;
    volatile bool store_task_ok;
    volatile bool store_task_cancel;
    int store_task_kind;
    int store_task_mirror;
    char store_task_stage[256];
    char store_task_name[160];
    int store_task_percent;
    char store_task_line[256];
    char store_task_output[200][256];
    int store_task_output_count;
    char store_featured[32][128];
    int store_featured_count;
    IconIndex store_icons;
    AppStreamIndex store_appstream;
    bool store_appstream_ready;
    bool store_icons_ready;
    bool store_icons_building;
    volatile bool store_icons_done;
    pthread_t store_icons_thread;
    char store_task_error[256];
    pthread_t store_task_thread;
    StoreBackend *store_task_backend;
    char **store_task_ids;
    int store_task_id_count;
    char store_search[96];
    char files_open_with_path[PATH_MAX];
    char files_open_with_mime[128];
    int files_open_with_apps[64];
    int files_open_with_count;
    int files_open_with_default;
    bool files_open_with_builtin;
    char pad_left_label[8];
    char pad_right_label[8];
    int64_t launch_started;

    bool text_input_active;
    int exit_code;
    char renderer_name[96];
    char renderer_info[192];
} App;

void app_init(App *app);
void app_run(App *app);
void app_free(App *app);

void app_input(App *app, InputAction action, const char *text);
void app_rebuild(App *app);
void app_refresh(App *app);
void app_rescan(App *app);

int app_collections(App *app, Collection *out, int max);
int app_total_collections(App *app);
bool app_current_collection(App *app, Collection *out);
Game *app_selected_game(App *app);
void app_enter_collection(App *app, const char *id);
void app_open_game(App *app, const char *game_id);
void app_launch_game(App *app, Game *game);
void app_launch_external(App *app);
void app_resume_external(App *app);
void app_resume_app(App *app, const char *id);
bool app_task_state(App *app, const char *id, char *out, size_t out_size);
void app_refresh_runtime(App *app);
void app_set_ui_suspended(App *app, bool suspended);
void app_open_app_editor(App *app, int index);
void app_store_enter(App *app);
void app_store_poll(App *app);
void app_store_free(App *app);
void input_store(App *app, InputAction action, const char *text);
void app_open_control_center(App *app);
void app_open_control_center_view(App *app, int view);
void app_open_power_menu(App *app);
void app_power_action(App *app, int action);
void app_open_overview(App *app);
void app_close_overview(App *app);
void app_activate_task(App *app, int index);
void app_close_task(App *app, int index);
void app_control_set_perf(App *app, int mode);
void app_control_set_gpu(App *app, int mode);
int app_ui_scale_max(App *app);
int app_ui_scale_values(int *out, int cap, int max);
int app_effective_ui_scale(App *app);
void app_control_toggle_wifi(App *app);
void app_control_toggle_bluetooth(App *app);
void app_control_select_radio(App *app, int radio);
void app_control_rescan_wifi(App *app);
void app_control_activate(App *app);
void app_control_connect(App *app, int index);
void app_control_connect_bluetooth(App *app, int index);
void app_control_volume(App *app, int delta);
void app_control_volume_set(App *app, int volume);
void app_brightness_set(App *app, int percent);
void app_brightness_step(App *app, int delta);
void app_set_language(App *app, int lang);
int app_timezone_option_count(App *app);
const char *app_timezone_option_id(App *app, int index);
const char *app_timezone_option_label(App *app, int index);
void app_filter_apps(App *app);
void app_update_pad_labels(App *app);
int app_selected_app_index(App *app);
AppEntry *app_selected_entry(App *app);
void app_set_app_category(App *app, const char *id);
void app_toggle_favorite(App *app, Game *game);
void app_open_text(App *app, TextPurpose purpose, int arg, const char *title,
                   const char *value, const char *hint);
void app_open_confirm(App *app, ConfirmPurpose purpose, int arg,
                      const char *title, const char *body);
void app_open_choice(App *app, ChoicePurpose purpose, const char *title);
void app_do_choice(App *app, int index, int value);
void app_settings_rows(App *app, int *count);
bool app_settings_row_info(App *app, int row, char *title, size_t title_size,
                           char *value, size_t value_size, int *kind);
int app_settings_page_size(int cat);
void app_settings_activate(App *app, int row);
void app_settings_adjust(App *app, int row, int dir);
const char *app_settings_category_name(int cat, const char **subtitle);
int app_settings_category_count(void);
int app_settings_rail_count(void);
int app_settings_rail_cat(int index);
int app_settings_rail_index(int cat);
void app_close_modal(App *app);
void app_menu_labels(App *app, const char **labels, MenuAction *actions, int *count);
void app_run_menu_action(App *app, MenuAction action);
void app_apply_gamepad_config(App *app);
void app_save_settings(App *app);
void app_goto_screen(App *app, Screen screen);
void app_files_enter(App *app);
void app_files_reload(App *app);
void app_files_poll(App *app);
void app_files_text_done(App *app);
void app_files_paste_now(App *app);
void app_files_delete_permanent(App *app);
void app_files_extract(App *app);
void app_files_extract_here(App *app);
void app_files_extract_to(App *app, const char *dir);
void app_files_pick_extract_dir(App *app);
void app_files_open_with_begin(App *app);
void app_files_open_with(App *app, int app_index, bool set_default);
void app_files_compress_now(App *app, int format);
void app_files_menu_action(App *app, MenuAction action);
bool app_files_favorite_path(App *app, char *out, size_t n);
bool app_files_favorite_contains(const char *path);
void app_files_favorite_toggle(App *app);
void input_files(App *app, InputAction action);
bool app_files_available(void);
bool app_files_pick_supported(App *app);
void app_files_favorites_reset(void);
void app_files_pick_start(App *app);
void app_files_smb_text_done(App *app);
void app_files_share_text_done(App *app);
void app_oobe_rom_done(App *app);
void app_oobe_date_open(App *app);
void app_oobe_date_apply(App *app);
void app_oobe_date_set_field(App *app, int field);
void oobe_input(App *app, InputAction action);
void oobe_finish(App *app);
void oobe_start(App *app, bool prefill);
void app_reset_settings(App *app);
void app_oobe_apply_language(App *app, int lang);
void app_oobe_apply_theme(App *app, int preset, bool auto_mode);
void app_oobe_cycle_ui_scale(App *app, int dir);
void app_oobe_cycle_refresh(App *app, int dir);
void app_oobe_cycle_render(App *app, int dir);
void app_oobe_cycle_paused(App *app, int dir);
int app_timezone_current_option(App *app);
void app_files_smb_auth_done(App *app);

#endif
