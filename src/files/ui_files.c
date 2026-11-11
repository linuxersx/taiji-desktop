// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "app.h"
#include "smb_share.h"
#include "external_osk.h"
#include "editor.h"
#include "theme.h"
#include "util.h"
#include "smb.h"
#include "star_icon.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    App *app;
    int index;
    bool pressed;
    bool long_fired;
    uint32_t press_ms;
} FileRowCtx;

typedef struct {
    App *app;
    char **argv;
} FileOpCtx;

/* ── USB MTP ─────────────────────────────────────────────────────────────
 * 真正的 USB 开关在 armdeck-usb-mtp（root），这里只负责调用并把状态
 * 显示在文件菜单左侧栏。切换会重建 USB gadget，adb 会短暂断开。
 */
#define MTP_HELPER "/usr/libexec/armdeck-usb-mtp"
#define MTP_FLAG   "/etc/armdeck-usb-mtp.enabled"
#define MTP_MODE   "/etc/armdeck-usb-mtp.mode"

static void files_mtp_apply(App *app, int focus);

static bool mtp_supported(void)
{
    return access(MTP_HELPER, X_OK) == 0;
}

static bool mtp_enabled(void)
{
    return access(MTP_FLAG, F_OK) == 0;
}

static bool mtp_readonly(void)
{
    FILE *f = fopen(MTP_MODE, "r");
    char buf[16] = "";
    bool ro = false;

    if (!f) return false;
    if (fgets(buf, sizeof(buf), f)) ro = strncmp(buf, "ro", 2) == 0;
    fclose(f);
    return ro;
}

typedef struct {
    App *app;
    char a1[8];
    char a2[8];
} MtpOpCtx;

static void *files_mtp_thread(void *data)
{
    MtpOpCtx *ctx = data;
    App *app = ctx->app;
    pid_t pid = fork();
    int rc = -1;

    if (pid == 0) {
        setsid();
        if (ctx->a2[0])
            execlp("pkexec", "pkexec", MTP_HELPER, ctx->a1, ctx->a2,
                   (char *)NULL);
        else
            execlp("pkexec", "pkexec", MTP_HELPER, ctx->a1, (char *)NULL);
        /* pkexec 不可用时直接执行（已是 root 的场景） */
        if (ctx->a2[0])
            execl(MTP_HELPER, MTP_HELPER, ctx->a1, ctx->a2, (char *)NULL);
        else
            execl(MTP_HELPER, MTP_HELPER, ctx->a1, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    app->files_mtp_result = rc;
    app->files_mtp_done = true;
    free(ctx);
    return NULL;
}

static void files_mtp_start(App *app, const char *a1, const char *a2)
{
    if (app->files_mtp_running) {
        ui_toast(app, "正在切换 USB 传输，请稍候");
        return;
    }
    MtpOpCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    snprintf(ctx->a1, sizeof(ctx->a1), "%s", a1);
    snprintf(ctx->a2, sizeof(ctx->a2), "%s", a2 ? a2 : "");
    app->files_mtp_running = true;
    app->files_mtp_done = false;
    app->files_mtp_result = -1;
    if (!strcmp(a1, "on"))
        ui_toast(app, "正在开启 USB 文件传输（USB 会重连）");
    else if (!strcmp(a1, "off"))
        ui_toast(app, "正在关闭 USB 文件传输");
    else if (a2 && !strcmp(a2, "ro"))
        ui_toast(app, "USB 访问设为只读");
    else
        ui_toast(app, "USB 访问设为读写");
    pthread_t th;
    if (pthread_create(&th, NULL, files_mtp_thread, ctx) != 0) {
        app->files_mtp_running = false;
        free(ctx);
        ui_toast(app, "无法启动切换");
        return;
    }
    pthread_detach(th);
}

static void files_load(App *app);
static void files_scan_locations(App *app);
static void files_sidebar_activate(App *app, int index);
static void files_share_input(App *app, InputAction action);
static void files_share_from_menu(App *app);
static void files_up(App *app);
static void files_enter_dir(App *app, const char *name);
static void files_action_toggle_hidden(App *app);
static void files_action_refresh(App *app);

bool app_files_available(void)
{
    const char *env = getenv("TAIJI_FILES_BIN");
    if (env && *env) return access(env, X_OK) == 0;
    return access("/usr/libexec/taiji/taiji-files", X_OK) == 0;
}

bool app_files_pick_supported(App *app)
{
    if (!app_files_available()) return false;
    return app->text_purpose == TEXT_ROOT || app->text_purpose == TEXT_APP_ICON ||
           app->text_purpose == TEXT_EXTRACT_DIR ||
           app->text_purpose == TEXT_SMB_SHARE_PATH ||
           app->text_purpose == TEXT_OOBE_ROM;
}

void app_files_pick_start(App *app)
{
    if (!app_files_pick_supported(app)) return;
    bool pick_file = app->text_purpose == TEXT_APP_ICON;
    char start[PATH_MAX];
    char *expanded = app->input_buf[0] ? path_absolute(app->input_buf) : NULL;
    if (expanded && fs_is_dir(expanded)) {
        snprintf(start, sizeof(start), "%s", expanded);
    } else if (expanded && fs_exists(expanded)) {
        char parent[PATH_MAX];
        if (fs_parent(expanded, parent, sizeof(parent)))
            snprintf(start, sizeof(start), "%s", parent);
        else
            snprintf(start, sizeof(start), "%s", fs_home());
    } else {
        snprintf(start, sizeof(start), "%s", fs_home());
    }
    free(expanded);

    external_osk_hide(app);
    ui_set_input_active(false);
    app->files_pick_return = app->screen;
    app->files_pick = pick_file ? FILES_PICK_FILE : FILES_PICK_DIR;
    snprintf(app->files_cwd, sizeof(app->files_cwd), "%s", start);
    app->files_focus = 0;
    app->files_message[0] = '\0';
    app_goto_screen(app, SCREEN_FILES);
}

static void files_pick_restore_text(App *app)
{
    Screen screen = app->files_pick_return;
    bool extract = app->text_purpose == TEXT_EXTRACT_DIR;
    app->files_pick = FILES_PICK_NONE;
    app->screen = extract ? SCREEN_FILES : screen;
    app->status_on_bar = false;
    app->nav_on_bar = false;
    if (extract) {
        app->modal = MODAL_NONE;
        ui_set_input_active(false);
        app_rebuild(app);
        return;
    }
    app->modal = MODAL_TEXT;
    ui_set_input_active(true);
    app_rebuild(app);
}

static void files_pick_accept(App *app, const char *path)
{
    if (!path || !*path) return;
    if (app->text_purpose == TEXT_EXTRACT_DIR) {
        app->files_pick = FILES_PICK_NONE;
        app->modal = MODAL_NONE;
        app->screen = SCREEN_FILES;
        app_files_extract_to(app, path);
        if (app->screen == SCREEN_FILES) app_rebuild(app);
        return;
    }
    snprintf(app->input_buf, sizeof(app->input_buf), "%s", path);
    app->input_cursor = (int)strlen(app->input_buf);
    files_pick_restore_text(app);
}

static void files_pick_status_cb(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (app->files_pick == FILES_PICK_DIR) {
        files_pick_accept(app, app->files_cwd);
    } else if (app->files_pick == FILES_PICK_FILE &&
               app->files_focus >= 0 &&
               app->files_focus < app->files_list.count &&
               !app->files_list.items[app->files_focus].is_dir) {
        char path[PATH_MAX];
        fs_join(path, sizeof(path), app->files_cwd,
                app->files_list.items[app->files_focus].name);
        files_pick_accept(app, path);
    } else {
        ui_toast(app, "请选择文件");
    }
}

static void files_pick_up(App *app)
{
    char parent[PATH_MAX];
    if (!fs_parent(app->files_cwd, parent, sizeof(parent))) {
        files_pick_restore_text(app);
        return;
    }
    snprintf(app->files_cwd, sizeof(app->files_cwd), "%s", parent);
    app->files_focus = 0;
    app_files_reload(app);
}

/* ── Sidebar locations, removable media and network shares ────────────────── */

static void files_shares_load(App *app)
{
    app->share_count = 0;
    char *path = path_join(taiji_config_dir(), "shares.tsv");
    char *data = read_file(path, NULL);
    free(path);
    if (!data) return;
    app->share_count = 0;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line && app->share_count < FILE_SHARE_MAX;
         line = strtok_r(NULL, "\n", &save)) {
        line = str_trim(line);
        if (!*line || *line == '#') continue;
        char *f[3] = {line, NULL, NULL};
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = '\0';
        f[1] = t1 + 1;
        char *t2 = strchr(f[1], '\t');
        if (t2) { *t2 = '\0'; f[2] = t2 + 1; }
        FileShare *sh = &app->shares[app->share_count++];
        snprintf(sh->label, sizeof(sh->label), "%s", f[0]);
        snprintf(sh->uri, sizeof(sh->uri), "%s", f[1]);
        snprintf(sh->user, sizeof(sh->user), "%s", f[2] ? f[2] : "");
    }
    free(data);
}

static void files_shares_save(App *app)
{
    size_t cap = 1024, len = 0;
    char *buf = xmalloc(cap);
    buf[0] = '\0';
    for (int i = 0; i < app->share_count; i++) {
        const FileShare *sh = &app->shares[i];
        int need = snprintf(NULL, 0, "%s\t%s\t%s\n", sh->label, sh->uri, sh->user);
        while (len + (size_t)need + 1 > cap) { cap *= 2; buf = xrealloc(buf, cap); }
        len += (size_t)snprintf(buf + len, cap - len, "%s\t%s\t%s\n",
                                sh->label, sh->uri, sh->user);
    }
    char *path = path_join(taiji_config_dir(), "shares.tsv");
    write_file_atomic(path, buf, len);
    free(path);
    free(buf);
}

static void files_loc_add(App *app, int section, FileLocKind kind,
                          const char *label, const char *path,
                          const char *device, const char *uri,
                          const char *user, bool mounted, bool removable)
{
    if (app->files_loc_count >= FILE_LOC_MAX) return;
    for (int i = 0; i < app->files_loc_count; i++) {
        FileLoc *e = &app->files_locs[i];
        if (device && *device && strcmp(e->device, device) == 0) return;
        if (uri && *uri && strcmp(e->uri, uri) == 0) return;
        if (!device && !uri && path && *path && strcmp(e->path, path) == 0) return;
    }
    FileLoc *e = &app->files_locs[app->files_loc_count++];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->section = section;
    e->mounted = mounted;
    e->removable = removable;
    if (label) snprintf(e->label, sizeof(e->label), "%s", label);
    if (path) snprintf(e->path, sizeof(e->path), "%s", path);
    if (device) snprintf(e->device, sizeof(e->device), "%s", device);
    if (uri) snprintf(e->uri, sizeof(e->uri), "%s", uri);
    if (user) snprintf(e->user, sizeof(e->user), "%s", user);
}

static bool lsblk_value(const char *line, const char *key, char *out, size_t n)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "%s=\"", key);
    const char *p = line;
    while ((p = strstr(p, pat)) != NULL) {
        if (p == line || p[-1] == ' ') break;
        p++;
    }
    if (!p) return false;
    p += strlen(pat);
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

/* 位置分组固定为：根目录 → 内置分区 → 可移动介质，避免 lsblk 输出顺序
 * 把组打散后分割线忽隐忽现。 */
static void files_sort_locations(App *app)
{
    int start = -1, end = -1;
    for (int i = 0; i < app->files_loc_count; i++) {
        if (app->files_locs[i].section != 1) continue;
        if (start < 0) start = i;
        end = i;
    }
    if (start < 0 || end <= start) return;
    int n = end - start + 1;
    FileLoc *tmp = xmalloc((size_t)n * sizeof(*tmp));
    int k = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = start; i <= end; i++) {
            bool removable = app->files_locs[i].removable;
            if ((pass == 1) == removable) tmp[k++] = app->files_locs[i];
        }
    }
    memcpy(&app->files_locs[start], tmp, (size_t)n * sizeof(*tmp));
    free(tmp);
}

#define FILES_FAV_MAX 32
static char g_files_favs[FILES_FAV_MAX][PATH_MAX];
static int g_files_fav_count = -1;

static int files_fav_index(const char *path)
{
    if (!path || !*path) return -1;
    for (int i = 0; i < g_files_fav_count; i++)
        if (strcmp(g_files_favs[i], path) == 0) return i;
    return -1;
}

static void files_favs_save(void)
{
    if (g_files_fav_count < 0) return;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/files_favorites", taiji_config_dir());
    size_t total = 0;
    for (int i = 0; i < g_files_fav_count; i++)
        total += strlen(g_files_favs[i]) + 1;
    char *buf = xmalloc(total + 1);
    char *w = buf;
    for (int i = 0; i < g_files_fav_count; i++) {
        size_t len = strlen(g_files_favs[i]);
        memcpy(w, g_files_favs[i], len);
        w += len;
        *w++ = '\n';
    }
    write_file_atomic(path, buf, (size_t)(w - buf));
    free(buf);
}

static void files_favs_seed(void)
{
    const char *home = fs_home();
    static const char *dirs[] = {
        "", "Desktop", "Downloads", "Documents", "Pictures", "Music",
        "Videos", "ROMs",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char path[PATH_MAX];
        if (!*dirs[i]) snprintf(path, sizeof(path), "%s", home);
        else fs_join(path, sizeof(path), home, dirs[i]);
        if (!fs_is_dir(path) || files_fav_index(path) >= 0) continue;
        if (g_files_fav_count >= FILES_FAV_MAX) break;
        snprintf(g_files_favs[g_files_fav_count++], PATH_MAX, "%s", path);
    }
    files_favs_save();
}

static void files_favs_load(void)
{
    if (g_files_fav_count >= 0) return;
    g_files_fav_count = 0;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/files_favorites", taiji_config_dir());
    if (access(path, F_OK) != 0) {
        files_favs_seed();
        return;
    }
    size_t len = 0;
    char *data = read_file(path, &len);
    if (!data) return;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        line = str_trim(line);
        if (!*line || files_fav_index(line) >= 0) continue;
        if (g_files_fav_count >= FILES_FAV_MAX) break;
        snprintf(g_files_favs[g_files_fav_count++], PATH_MAX, "%s", line);
    }
    free(data);
}

void app_files_favorites_reset(void)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/files_favorites", taiji_config_dir());
    unlink(path);
    g_files_fav_count = -1;
    files_favs_load();
}

bool app_files_favorite_contains(const char *path)
{
    files_favs_load();
    return files_fav_index(path) >= 0;
}

bool app_files_favorite_path(App *app, char *out, size_t n)
{
    if (!app || !out || n == 0) return false;
    if (app->files_on_sidebar && app->files_side_focus >= 0 &&
        app->files_side_focus < app->files_loc_count) {
        FileLoc *e = &app->files_locs[app->files_side_focus];
        if (e->path[0] && fs_is_dir(e->path)) {
            snprintf(out, n, "%s", e->path);
            return true;
        }
    }
    if (!app->files_on_sidebar && app->files_focus >= 0 &&
        app->files_focus < app->files_list.count) {
        FsEntry *e = &app->files_list.items[app->files_focus];
        if (e->is_dir) {
            fs_join(out, n, app->files_cwd, e->name);
            return true;
        }
    }
    /* A selected file still favorites the folder it lives in. */
    if (app->files_cwd[0]) {
        snprintf(out, n, "%s", app->files_cwd);
        return true;
    }
    return false;
}

void app_files_favorite_toggle(App *app)
{
    char path[PATH_MAX];
    if (!app_files_favorite_path(app, path, sizeof(path))) {
        ui_toast(app, "没有可收藏的路径");
        return;
    }
    if (!fs_is_dir(path)) {
        ui_toast(app, "只能收藏文件夹");
        return;
    }
    files_favs_load();
    int idx = files_fav_index(path);
    bool added;
    if (idx >= 0) {
        for (int i = idx; i < g_files_fav_count - 1; i++)
            memcpy(g_files_favs[i], g_files_favs[i + 1], PATH_MAX);
        g_files_fav_count--;
        added = false;
    } else if (g_files_fav_count < FILES_FAV_MAX) {
        snprintf(g_files_favs[g_files_fav_count++], PATH_MAX, "%s", path);
        added = true;
    } else {
        ui_toast(app, "收藏数量已满");
        return;
    }
    files_favs_save();
    files_scan_locations(app);
    app->files_message[0] = '\0';
    app_rebuild(app);
    ui_toast(app, added ? "已加入个人收藏" : "已从个人收藏移除");
}

static void files_fav_label(const char *path, char *out, size_t out_size)
{
    static const struct { const char *dir; const char *label; } known[] = {
        {"Desktop", "桌面"}, {"Downloads", "下载"}, {"Documents", "文档"},
        {"Pictures", "图片"}, {"Music", "音乐"}, {"Videos", "影片"},
        {"ROMs", "ROMs"},
    };
    const char *home = fs_home();
    if (strcmp(path, home) == 0) {
        snprintf(out, out_size, "主目录");
        return;
    }
    const char *base = path_basename(path);
    char parent[PATH_MAX];
    if (fs_parent(path, parent, sizeof(parent)) && strcmp(parent, home) == 0) {
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
            if (strcmp(base, known[i].dir) == 0) {
                snprintf(out, out_size, "%s", known[i].label);
                return;
            }
        }
    }
    snprintf(out, out_size, "%s", base);
}

static void files_scan_locations(App *app)
{
    app->files_loc_count = 0;
    const char *home = fs_home();
    files_favs_load();
    for (int i = 0; i < g_files_fav_count; i++) {
        const char *path = g_files_favs[i];
        if (!fs_is_dir(path)) continue;
        char label[128];
        files_fav_label(path, label, sizeof(label));
        files_loc_add(app, 0, FILE_LOC_PATH, label, path,
                      NULL, NULL, NULL, true, false);
    }
    files_loc_add(app, 1, FILE_LOC_PATH, "根目录", "/", NULL, NULL, NULL, true, false);

    char *trash = path_join(home, ".local/share/Trash/files");
    mkdir_p(trash);
    files_loc_add(app, 1, FILE_LOC_PATH, "回收站", trash,
                  NULL, NULL, NULL, true, false);
    free(trash);

    FILE *p = popen("lsblk -P -o NAME,PATH,LABEL,FSTYPE,MOUNTPOINT,RM,TYPE,TRAN,SIZE 2>/dev/null", "r");
    if (p) {
        char line[2048];
        while (fgets(line, sizeof(line), p) && app->files_loc_count < FILE_LOC_MAX) {
            char name[64] = "", dev[PATH_MAX] = "", label[128] = "";
            char fstype[64] = "", mp[PATH_MAX] = "", rm[8] = "";
            char type[24] = "", tran[24] = "", size[32] = "";
            lsblk_value(line, "NAME", name, sizeof(name));
            lsblk_value(line, "PATH", dev, sizeof(dev));
            lsblk_value(line, "LABEL", label, sizeof(label));
            lsblk_value(line, "FSTYPE", fstype, sizeof(fstype));
            lsblk_value(line, "MOUNTPOINT", mp, sizeof(mp));
            lsblk_value(line, "RM", rm, sizeof(rm));
            lsblk_value(line, "TYPE", type, sizeof(type));
            lsblk_value(line, "TRAN", tran, sizeof(tran));
            lsblk_value(line, "SIZE", size, sizeof(size));
            if (!*fstype || strcmp(fstype, "swap") == 0 || strcmp(fstype, "crypto_LUKS") == 0)
                continue;
            if (strcmp(type, "part") != 0 && strcmp(type, "disk") != 0) continue;
            bool mounted = mp[0] == '/';
            bool in_media = strncmp(mp, "/run/media/", 11) == 0 ||
                            strncmp(mp, "/media/", 7) == 0 ||
                            strncmp(mp, "/mnt/", 5) == 0;
            /* RM 是判断可移动介质的权威字段；TRAN=mmc 同时包含内置 eMMC
             * 和 SD 卡，不能再一律当作可移动。RM 缺失时才退回挂载路径。 */
            bool removable = strcmp(rm, "1") == 0 || strcmp(tran, "usb") == 0;
            if (!*rm) removable = in_media;
            if (mounted && (!strcmp(mp, "/") || strncmp(mp, "/boot", 5) == 0 ||
                            strncmp(mp, "/var", 4) == 0 || strncmp(mp, "/usr", 4) == 0))
                continue;
            /* 未挂载的内置分区也要显示，否则在“位置”里只能看到已挂载分区。 */
            char text[192];
            const char *base = *label ? label : (*name ? name : dev);
            if (*size) snprintf(text, sizeof(text), "%s  %s", base, size);
            else snprintf(text, sizeof(text), "%s", base);
            files_loc_add(app, 1, FILE_LOC_DEVICE, text, mounted ? mp : "",
                          dev, NULL, NULL, mounted, removable);
        }
        pclose(p);
    }
    files_sort_locations(app);

    files_shares_load(app);
    for (int i = 0; i < app->share_count; i++)
        files_loc_add(app, 2, FILE_LOC_SHARE, app->shares[i].label, "",
                      NULL, app->shares[i].uri, app->shares[i].user, true, false);
    files_loc_add(app, 2, FILE_LOC_CONNECT, "连接", "", NULL, NULL, NULL, true, false);
    files_loc_add(app, 2, FILE_LOC_MANAGE, "管理", "", NULL, NULL, NULL, true, false);

    if (mtp_supported())
        files_loc_add(app, 3, FILE_LOC_MTP, "MTP", "",
                      NULL, NULL, NULL, mtp_enabled(), false);

    if (app->files_side_focus >= app->files_loc_count)
        app->files_side_focus = app->files_loc_count > 0 ? app->files_loc_count - 1 : 0;
    if (app->files_side_focus < 0) app->files_side_focus = 0;
}

static void files_open_path(App *app, const char *path)
{
    if (!path || !*path || !fs_is_dir(path)) {
        ui_toast(app, "目录不可用");
        return;
    }
    snprintf(app->files_cwd, sizeof(app->files_cwd), "%s", path);
    app->files_focus = 0;
    app->files_on_sidebar = false;
    app->files_message[0] = '\0';
    files_load(app);
    if (app->screen == SCREEN_FILES) app_rebuild(app);
}

/* ── Removable media mount ────────────────────────────────────────────────── */

typedef struct {
    App *app;
    char device[64];
    bool unmount;
} MountCtx;

static void *files_mount_thread(void *data)
{
    MountCtx *ctx = data;
    App *app = ctx->app;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "udisksctl %s -b %s 2>&1",
             ctx->unmount ? "unmount" : "mount", ctx->device);
    FILE *p = popen(cmd, "r");
    char msg[256] = "";
    size_t used = 0;
    if (p) {
        size_t n = fread(msg, 1, sizeof(msg) - 1, p);
        msg[n] = '\0';
        used = n;
        int rc = pclose(p);
        app->files_mount_ok = rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
    } else {
        app->files_mount_ok = false;
    }
    if (!app->files_mount_ok && used == 0)
        snprintf(msg, sizeof(msg), ctx->unmount ? "无法卸载设备" : "无法执行 udisksctl");
    char *nl = strchr(msg, '\n');
    if (nl) *nl = '\0';
    snprintf(app->files_mount_msg, sizeof(app->files_mount_msg), "%s", msg);
    app->files_mount_done = true;
    free(ctx);
    return NULL;
}

static void files_mount_start(App *app, int index)
{
    if (app->files_mount_running) {
        ui_toast(app, "正在挂载设备");
        return;
    }
    if (index < 0 || index >= app->files_loc_count) return;
    FileLoc *e = &app->files_locs[index];
    if (!e->device[0]) return;
    MountCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->unmount = false;
    snprintf(ctx->device, sizeof(ctx->device), "%s", e->device);
    snprintf(app->files_mount_device, sizeof(app->files_mount_device), "%s", e->device);
    app->files_mount_path[0] = '\0';
    app->files_mount_unmount = false;
    app->files_mount_running = true;
    app->files_mount_done = false;
    app->files_mount_ok = false;
    app->files_mount_msg[0] = '\0';
    ui_toast(app, "正在挂载...");
    pthread_t th;
    if (pthread_create(&th, NULL, files_mount_thread, ctx) != 0) {
        app->files_mount_running = false;
        free(ctx);
        ui_toast(app, "无法启动挂载");
        return;
    }
    pthread_detach(th);
}

static bool files_mount_is_system(const char *path)
{
    if (!path || !*path) return true;
    if (strcmp(path, "/") == 0 || strncmp(path, "/boot", 5) == 0 ||
        strncmp(path, "/home", 5) == 0 || strncmp(path, "/var", 4) == 0 ||
        strncmp(path, "/usr", 4) == 0)
        return true;
    return false;
}

static void files_unmount_start(App *app, int index)
{
    if (app->files_mount_running) {
        ui_toast(app, "正在处理存储设备");
        return;
    }
    if (app->files_op_running) {
        ui_toast(app, "文件操作进行中，暂不能卸载");
        return;
    }
    if (index < 0 || index >= app->files_loc_count) return;
    FileLoc *e = &app->files_locs[index];
    if (e->kind != FILE_LOC_DEVICE || !e->mounted || !e->device[0]) {
        ui_toast(app, "请选择已挂载的分区");
        return;
    }
    if (!e->removable || files_mount_is_system(e->path) ||
        (strncmp(e->path, "/run/media/", 11) != 0 &&
         strncmp(e->path, "/media/", 7) != 0 &&
         strncmp(e->path, "/mnt/", 5) != 0)) {
        ui_toast(app, "系统分区不能卸载");
        return;
    }
    MountCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->unmount = true;
    snprintf(ctx->device, sizeof(ctx->device), "%s", e->device);
    snprintf(app->files_mount_device, sizeof(app->files_mount_device), "%s", e->device);
    snprintf(app->files_mount_path, sizeof(app->files_mount_path), "%s", e->path);
    app->files_mount_unmount = true;
    app->files_mount_running = true;
    app->files_mount_done = false;
    app->files_mount_ok = false;
    app->files_mount_msg[0] = '\0';
    ui_toast(app, "正在卸载分区...");
    pthread_t th;
    if (pthread_create(&th, NULL, files_mount_thread, ctx) != 0) {
        app->files_mount_running = false;
        free(ctx);
        ui_toast(app, "无法启动卸载");
        return;
    }
    pthread_detach(th);
}

/* ── SMB shares ───────────────────────────────────────────────────────────── */

typedef struct {
    App *app;
    char uri[512];
    char user[128];
    char pass[256];
} SmbCtx;

static void *files_smb_thread(void *data)
{
    SmbCtx *ctx = data;
    App *app = ctx->app;
    app->smb_ok = smb_mount_uri(ctx->uri, ctx->user, ctx->pass,
                                app->smb_path, sizeof(app->smb_path),
                                app->smb_msg, sizeof(app->smb_msg));
    app->smb_done = true;
    free(ctx);
    return NULL;
}

static void files_smb_start(App *app)
{
    if (app->smb_running) {
        ui_toast(app, "正在连接共享");
        return;
    }
    char uri[512];
    snprintf(uri, sizeof(uri), "smb://%s/%s", app->smb_host, app->smb_share);
    SmbCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    snprintf(ctx->uri, sizeof(ctx->uri), "%s", uri);
    snprintf(ctx->user, sizeof(ctx->user), "%s", app->smb_user);
    snprintf(ctx->pass, sizeof(ctx->pass), "%s", app->smb_pass);
    app->smb_running = true;
    app->smb_done = false;
    app->smb_ok = false;
    app->smb_path[0] = '\0';
    app->smb_msg[0] = '\0';
    ui_toast(app, "正在连接共享...");
    pthread_t th;
    if (pthread_create(&th, NULL, files_smb_thread, ctx) != 0) {
        app->smb_running = false;
        free(ctx);
        ui_toast(app, "无法启动共享连接");
        return;
    }
    pthread_detach(th);
}

static void files_smb_begin(App *app)
{
    app->smb_host[0] = '\0';
    app->smb_share[0] = '\0';
    app->smb_user[0] = '\0';
    app->smb_pass[0] = '\0';
    app_open_text(app, TEXT_SMB_HOST, 0, "连接共享文件夹", "",
                  "输入电脑地址，例如 192.168.1.10 或 nas/media");
}

void app_files_smb_text_done(App *app)
{
    char value[2048];
    snprintf(value, sizeof(value), "%s", app->input_buf);
    char *v = str_trim(value);
    if (app->text_purpose == TEXT_SMB_HOST) {
        if (*v == '\0') { ui_toast(app, "请输入电脑地址"); return; }
        char host[256] = "", share[128] = "", user[128] = "";
        char *p = v;
        if (strncmp(p, "smb://", 6) == 0) p += 6;
        char *at = strchr(p, '@');
        char *slash = strchr(p, '/');
        if (at && (!slash || at < slash)) {
            *at = '\0';
            snprintf(user, sizeof(user), "%s", p);
            p = at + 1;
        }
        slash = strchr(p, '/');
        if (slash) {
            *slash = '\0';
            char *rest = slash + 1;
            while (*rest == '/') rest++;
            snprintf(share, sizeof(share), "%s", rest);
        }
        snprintf(host, sizeof(host), "%s", p);
        if (!*host) { ui_toast(app, "地址无效"); return; }
        snprintf(app->smb_host, sizeof(app->smb_host), "%s", host);
        snprintf(app->smb_share, sizeof(app->smb_share), "%s", share);
        if (user[0]) snprintf(app->smb_user, sizeof(app->smb_user), "%s", user);
        if (!app->smb_share[0]) {
            app_open_text(app, TEXT_SMB_SHARE, 0, "共享名称", "",
                          "输入共享文件夹名称，例如 media");
        } else {
            app_open_text(app, TEXT_SMB_USER, 0, "共享用户名", app->smb_user,
                          "留空表示访客访问");
        }
        return;
    }
    if (app->text_purpose == TEXT_SMB_SHARE) {
        while (*v == '/') v++;
        size_t vlen = strlen(v);
        while (vlen > 0 && v[vlen - 1] == '/') v[--vlen] = '\0';
        if (*v == '\0') { ui_toast(app, "请输入共享名称"); return; }
        snprintf(app->smb_share, sizeof(app->smb_share), "%s", v);
        app_open_text(app, TEXT_SMB_USER, 0, "共享用户名", app->smb_user,
                      "留空表示访客访问");
        return;
    }
    if (app->text_purpose == TEXT_SMB_USER) {
        snprintf(app->smb_user, sizeof(app->smb_user), "%s", v);
        app->smb_pass[0] = '\0';
        app_open_text(app, TEXT_SMB_PASS, 0, "共享密码", "",
                      "留空表示无密码或访客访问");
        return;
    }
    if (app->text_purpose == TEXT_SMB_PASS) {
        snprintf(app->smb_pass, sizeof(app->smb_pass), "%s", app->input_buf);
        app_close_modal(app);
        files_smb_start(app);
        return;
    }
}


static void files_free_marks(App *app)
{
    free(app->files_marked);
    app->files_marked = NULL;
    app->files_marked_count = 0;
}

static void files_load(App *app)
{
    fs_list_free(&app->files_list);
    fs_list_dir(app->files_cwd, app->files_show_hidden, &app->files_list);
    files_free_marks(app);
    if (app->files_list.count > 0) {
        app->files_marked = calloc((size_t)app->files_list.count, sizeof(bool));
        if (app->files_focus >= app->files_list.count)
            app->files_focus = app->files_list.count - 1;
        if (app->files_focus < 0) app->files_focus = 0;
    } else {
        app->files_focus = 0;
    }
    app->files_loaded = true;
}

void app_files_enter(App *app)
{
    if (!app->files_cwd[0])
        snprintf(app->files_cwd, sizeof(app->files_cwd), "%s", fs_home());
    files_load(app);
    files_scan_locations(app);
    app->files_message[0] = '\0';
}

void app_files_reload(App *app)
{
    files_load(app);
    app->files_message[0] = '\0';
    if (app->screen == SCREEN_FILES) app_rebuild(app);
}

static bool files_current_path(App *app, char *out, size_t n)
{
    if (app->files_focus < 0 || app->files_focus >= app->files_list.count) return false;
    fs_join(out, n, app->files_cwd, app->files_list.items[app->files_focus].name);
    return true;
}

static char **files_collect(App *app, int *out_n)
{
    int n = 0;
    char **v = xmalloc(sizeof(char *) * 256);
    if (app->files_marked_count > 0) {
        for (int i = 0; i < app->files_list.count; i++) {
            if (app->files_marked[i]) {
                char p[PATH_MAX];
                fs_join(p, sizeof(p), app->files_cwd, app->files_list.items[i].name);
                v[n++] = xstrdup(p);
            }
        }
    } else if (app->files_focus >= 0 && app->files_focus < app->files_list.count) {
        char p[PATH_MAX];
        fs_join(p, sizeof(p), app->files_cwd,
                app->files_list.items[app->files_focus].name);
        v[n++] = xstrdup(p);
    }
    *out_n = n;
    return v;
}

static void files_free_paths(char **v, int n)
{
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

static void *files_op_thread(void *data)
{
    FileOpCtx *ctx = data;
    App *app = ctx->app;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execvp(ctx->argv[0], ctx->argv);
        _exit(127);
    }
    if (pid < 0) {
        app->files_op_result = -1;
        app->files_op_done = true;
    } else {
        app->files_op_pid = pid;
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        app->files_op_result = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        app->files_op_done = true;
    }
    for (int i = 0; ctx->argv[i]; i++) free(ctx->argv[i]);
    free(ctx->argv);
    free(ctx);
    return NULL;
}

static void files_begin_op(App *app, char **argv, const char *label)
{
    if (app->files_op_running) {
        for (int i = 0; argv[i]; i++) free(argv[i]);
        free(argv);
        return;
    }
    int n = 0;
    while (argv[n]) n++;
    char **copy = xmalloc(sizeof(char *) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) copy[i] = xstrdup(argv[i]);
    copy[n] = NULL;
    for (int i = 0; i < n; i++) free(argv[i]);
    free(argv);
    FileOpCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->argv = copy;
    app->files_op_running = true;
    app->files_op_done = false;
    app->files_op_result = 0;
    app->files_op_pid = -1;
    ui_toast(app, label ? label : "正在处理...");
    pthread_t th;
    if (pthread_create(&th, NULL, files_op_thread, ctx) != 0) {
        app->files_op_running = false;
        files_free_paths(copy, n);
        free(ctx);
        ui_toast(app, "无法启动文件操作");
        return;
    }
    pthread_detach(th);
}

void app_files_poll(App *app)
{
    if (app->files_mount_running && app->files_mount_done) {
        bool unmount = app->files_mount_unmount;
        char mount_path[PATH_MAX];
        snprintf(mount_path, sizeof(mount_path), "%s", app->files_mount_path);
        app->files_mount_running = false;
        app->files_mount_done = false;
        app->files_mount_unmount = false;
        files_scan_locations(app);
        if (unmount) {
            if (app->files_mount_ok) {
                size_t n = strlen(mount_path);
                bool under = n > 1 && strncmp(app->files_cwd, mount_path, n) == 0 &&
                             (app->files_cwd[n] == '/' || app->files_cwd[n] == '\0');
                if (under) {
                    snprintf(app->files_cwd, sizeof(app->files_cwd), "%s", fs_home());
                    files_load(app);
                }
                ui_toast(app, "分区已卸载");
            } else {
                ui_toast(app, app->files_mount_msg[0] ? app->files_mount_msg
                                                      : "卸载失败");
            }
            if (app->screen == SCREEN_FILES) app_rebuild(app);
        } else if (app->files_mount_ok) {
            const char *target = NULL;
            for (int i = 0; i < app->files_loc_count; i++) {
                FileLoc *e = &app->files_locs[i];
                if (e->kind == FILE_LOC_DEVICE && e->mounted &&
                    strcmp(e->device, app->files_mount_device) == 0) {
                    target = e->path;
                    break;
                }
            }
            if (target && *target) {
                ui_toast(app, "设备已挂载");
                files_open_path(app, target);
            } else {
                ui_toast(app, "已挂载，但找不到挂载位置");
                if (app->screen == SCREEN_FILES) app_rebuild(app);
            }
        } else {
            ui_toast(app, app->files_mount_msg[0] ? app->files_mount_msg : "挂载失败");
            if (app->screen == SCREEN_FILES) app_rebuild(app);
        }
    }

    if (app->smb_ui.scan_done) {
        app->smb_ui.scan_done = false;
        app->smb_ui.scan_running = false;
        if (app->screen == SCREEN_FILES) app_rebuild(app);
    }

    if (app->smb_running && app->smb_done) {
        app->smb_running = false;
        app->smb_done = false;
        if (app->smb_ok) {
            char label[192], uri[512];
            snprintf(label, sizeof(label), "%s/%s", app->smb_host, app->smb_share);
            snprintf(uri, sizeof(uri), "smb://%s/%s", app->smb_host, app->smb_share);
            bool exists = false;
            for (int i = 0; i < app->share_count; i++)
                if (strcmp(app->shares[i].uri, uri) == 0) exists = true;
            if (!exists && app->share_count < FILE_SHARE_MAX) {
                FileShare *sh = &app->shares[app->share_count++];
                snprintf(sh->label, sizeof(sh->label), "%s", label);
                snprintf(sh->uri, sizeof(sh->uri), "%s", uri);
                snprintf(sh->user, sizeof(sh->user), "%s", app->smb_user);
                files_shares_save(app);
            }
            files_scan_locations(app);
            ui_toast(app, "共享已连接");
            files_open_path(app, app->smb_path);
        } else {
            ui_toast(app, app->smb_msg[0] ? app->smb_msg : "共享连接失败");
            if (app->screen == SCREEN_FILES) app_rebuild(app);
        }
    }

    if (app->files_mtp_running && app->files_mtp_done) {
        app->files_mtp_running = false;
        app->files_mtp_done = false;
        files_scan_locations(app);
        if (app->files_mtp_result == 0)
            ui_toast(app, "USB 文件传输状态已更新");
        else
            ui_toast(app, "USB 文件传输切换失败");
        if (app->screen == SCREEN_FILES) app_rebuild(app);
    }

    if (!app->files_op_running || !app->files_op_done) return;
    app->files_op_running = false;
    app->files_op_done = false;
    if (app->files_op_result != 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "文件操作失败（%d）", app->files_op_result);
        ui_toast(app, msg);
    } else {
        ui_toast(app, "操作完成");
    }
    app_files_reload(app);
}

static void files_toggle_hidden(App *app)
{
    app->files_show_hidden = !app->files_show_hidden;
    files_load(app);
    if (app->screen == SCREEN_FILES) app_rebuild(app);
}

static void files_copy(App *app, bool cut)
{
    int n = 0;
    char **v = files_collect(app, &n);
    if (n == 0) { free(v); return; }
    for (int i = 0; i < app->files_clip_count; i++) free(app->files_clip[i]);
    free(app->files_clip);
    app->files_clip = v;
    app->files_clip_count = n;
    app->files_clip_cut = cut;
    char msg[96];
    snprintf(msg, sizeof(msg), cut ? "已剪切 %d 项" : "已复制 %d 项", n);
    ui_toast(app, msg);
}

static bool files_has_conflict(App *app)
{
    for (int i = 0; i < app->files_clip_count; i++) {
        const char *base = strrchr(app->files_clip[i], '/');
        base = base ? base + 1 : app->files_clip[i];
        char dest[PATH_MAX];
        fs_join(dest, sizeof(dest), app->files_cwd, base);
        if (fs_exists(dest)) return true;
    }
    return false;
}

void app_files_paste_now(App *app)
{
    if (app->files_clip_count == 0) return;
    int n = app->files_clip_count;
    char **argv = xmalloc(sizeof(char *) * (size_t)(n + 5));
    int k = 0;
    argv[k++] = xstrdup(app->files_clip_cut ? "mv" : "cp");
    argv[k++] = xstrdup(app->files_clip_cut ? "-f" : "-a");
    argv[k++] = xstrdup("--");
    for (int i = 0; i < n; i++) argv[k++] = xstrdup(app->files_clip[i]);
    argv[k++] = xstrdup(app->files_cwd);
    argv[k] = NULL;
    files_begin_op(app, argv, app->files_clip_cut ? "正在移动..." : "正在复制...");
}

static void files_paste(App *app)
{
    if (app->files_clip_count == 0) {
        ui_toast(app, "剪贴板为空");
        return;
    }
    if (files_has_conflict(app)) {
        app_open_confirm(app, CONFIRM_PASTE_OVERWRITE, 0, "目标已存在",
                         "覆盖同名文件并继续？");
        return;
    }
    app_files_paste_now(app);
}

static void files_delete(App *app, bool trash)
{
    int n = 0;
    char **v = files_collect(app, &n);
    if (n == 0) { free(v); return; }
    char **argv = xmalloc(sizeof(char *) * (size_t)(n + 4));
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
    files_begin_op(app, argv, trash ? "正在移到回收站..." : "正在永久删除...");
}

void app_files_delete_permanent(App *app)
{
    files_delete(app, false);
}

void app_files_text_done(App *app)
{
    int action = app->modal_arg;
    char name[512];
    snprintf(name, sizeof(name), "%s", app->input_buf);
    char *trimmed = str_trim(name);
    if (!*trimmed) { app_close_modal(app); return; }
    if (action == 1) {
        char path[PATH_MAX];
        fs_join(path, sizeof(path), app->files_cwd, trimmed);
        if (mkdir(path, 0755) != 0)
            snprintf(app->files_message, sizeof(app->files_message), "创建失败：%s", strerror(errno));
        app_close_modal(app);
        app_files_reload(app);
    } else if (action == 2) {
        char old[PATH_MAX], path[PATH_MAX];
        if (!files_current_path(app, old, sizeof(old))) { app_close_modal(app); return; }
        fs_join(path, sizeof(path), app->files_cwd, trimmed);
        if (rename(old, path) != 0)
            snprintf(app->files_message, sizeof(app->files_message), "重命名失败：%s", strerror(errno));
        app_close_modal(app);
        app_files_reload(app);
    } else if (action == 4) {
        char path[PATH_MAX];
        fs_join(path, sizeof(path), app->files_cwd, trimmed);
        app_close_modal(app);
        editor_open(app, path);
    } else if (action == 3) {
        char *expanded = expand_home(trimmed);
        if (fs_is_dir(expanded)) {
            snprintf(app->files_cwd, sizeof(app->files_cwd), "%s", expanded);
            app->files_focus = 0;
            app_close_modal(app);
            app_files_reload(app);
        } else {
            ui_toast(app, "目录不存在");
            free(expanded);
            return;
        }
        free(expanded);
    }
}

static void files_properties(App *app)
{
    if (app->files_list.count == 0) return;
    FsEntry *e = &app->files_list.items[app->files_focus];
    snprintf(app->file_info_name, sizeof(app->file_info_name), "%s", e->name);
    snprintf(app->file_info_type, sizeof(app->file_info_type), "%s",
             e->is_dir ? "目录" : "文件");
    if (e->is_dir)
        snprintf(app->file_info_size, sizeof(app->file_info_size), "—");
    else
        fs_format_size(e->size, app->file_info_size, sizeof(app->file_info_size));
    fs_format_time(e->mtime, app->file_info_time, sizeof(app->file_info_time));
    snprintf(app->file_info_path, sizeof(app->file_info_path), "%s", app->files_cwd);
    if (app->modal != MODAL_NONE) app_close_modal(app);
    app->menu_info = true;
    app->modal = MODAL_MENU;
    app->menu_focus = 0;
    ui_build_modal(app);
}

static bool files_cmd_in_path(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin:/usr/local/bin";
    char *copy = xstrdup(path);
    char *save = NULL;
    bool found = false;
    for (char *dir = strtok_r(copy, ":", &save); dir && !found;
         dir = strtok_r(NULL, ":", &save)) {
        char *p = path_join(dir, name);
        found = access(p, X_OK) == 0;
        free(p);
    }
    free(copy);
    return found;
}

static const char *files_7z_cmd(void)
{
    static const char *names[] = {"7zz", "7z", "7za"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (files_cmd_in_path(names[i])) return names[i];
    }
    return NULL;
}

bool files_is_archive(const char *name)
{
    static const char *exts[] = {
        ".7z", ".zip", ".rar", ".tar.gz", ".tgz", ".tar.xz", ".txz",
        ".tar.bz2", ".tbz2", ".tbz", ".tar", ".gz", ".xz", ".bz2",
        ".zst", ".tzst", ".lz4", ".cab", ".iso", ".wim", ".arj", ".lzh"
    };
    size_t len = strlen(name);
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t el = strlen(exts[i]);
        if (len > el && strcasecmp(name + len - el, exts[i]) == 0) return true;
    }
    return false;
}

static void files_archive_stem(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s", name);
    static const char *exts[] = {
        ".tar.gz", ".tar.xz", ".tar.bz2", ".tzst", ".tgz", ".txz",
        ".tbz2", ".tbz", ".7z", ".zip", ".rar", ".tar", ".gz",
        ".xz", ".bz2", ".zst", ".lz4", ".cab", ".iso", ".wim"
    };
    size_t len = strlen(out);
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t el = strlen(exts[i]);
        if (len > el && strcasecmp(out + len - el, exts[i]) == 0) {
            out[len - el] = '\0';
            return;
        }
    }
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}

static void files_unique_dir(const char *base, char *out, size_t n)
{
    snprintf(out, n, "%s", base);
    if (!fs_exists(out)) return;
    for (int i = 2; i < 100; i++) {
        snprintf(out, n, "%s_%d", base, i);
        if (!fs_exists(out)) return;
    }
}

static void files_unique_file(const char *base, const char *ext, char *out, size_t n)
{
    snprintf(out, n, "%s%s", base, ext);
    if (!fs_exists(out)) return;
    for (int i = 2; i < 100; i++) {
        snprintf(out, n, "%s_%d%s", base, i, ext);
        if (!fs_exists(out)) return;
    }
}

void app_files_extract(App *app)
{
    char path[PATH_MAX];
    if (!files_current_path(app, path, sizeof(path))) return;
    FsEntry *e = &app->files_list.items[app->files_focus];
    if (e->is_dir || !files_is_archive(e->name)) {
        ui_toast(app, "请选择一个压缩文件");
        return;
    }
    snprintf(app->extract_archive, sizeof(app->extract_archive), "%s", path);
    app_open_choice(app, CHOICE_EXTRACT_TARGET, "解压压缩包");
}

void app_files_extract_to(App *app, const char *dir)
{
    if (!app->extract_archive[0]) return;
    if (!dir || !*dir) return;
    const char *z = files_7z_cmd();
    if (!z) {
        ui_toast(app, "未找到 7-Zip（7zz / 7z）");
        return;
    }
    const char *base_name = strrchr(app->extract_archive, '/');
    base_name = base_name ? base_name + 1 : app->extract_archive;
    char stem[512];
    files_archive_stem(base_name, stem, sizeof(stem));
    char base[PATH_MAX];
    fs_join(base, sizeof(base), dir, stem[0] ? stem : "extracted");
    char target[PATH_MAX];
    files_unique_dir(base, target, sizeof(target));
    if (mkdir(target, 0755) != 0 && errno != EEXIST) {
        ui_toast(app, "无法创建解压目录");
        return;
    }
    char opt[PATH_MAX + 4];
    snprintf(opt, sizeof(opt), "-o%s", target);
    char **argv = xmalloc(sizeof(char *) * 7);
    int k = 0;
    argv[k++] = xstrdup(z);
    argv[k++] = xstrdup("x");
    argv[k++] = xstrdup("-y");
    argv[k++] = xstrdup(opt);
    argv[k++] = xstrdup("--");
    argv[k++] = xstrdup(app->extract_archive);
    argv[k] = NULL;
    files_begin_op(app, argv, "正在解压...");
}

void app_files_extract_here(App *app)
{
    app_files_extract_to(app, app->files_cwd);
}

void app_files_pick_extract_dir(App *app)
{
    if (!app->extract_archive[0]) return;
    app->text_purpose = TEXT_EXTRACT_DIR;
    app->files_pick_return = SCREEN_FILES;
    app->files_pick = FILES_PICK_DIR;
    app->files_focus = 0;
    app->files_message[0] = '\0';
    app_goto_screen(app, SCREEN_FILES);
}

void app_files_compress_now(App *app, int format)
{
    int n = 0;
    char **v = files_collect(app, &n);
    if (n == 0) {
        free(v);
        return;
    }
    const char *z = files_7z_cmd();
    if (!z) {
        ui_toast(app, "未找到 7-Zip（7zz / 7z）");
        files_free_paths(v, n);
        return;
    }
    const char *type = "-t7z";
    const char *ext = ".7z";
    if (format == 1) { type = "-tzip"; ext = ".zip"; }
    else if (format == 2) { type = "-ttar"; ext = ".tar"; }

    char base[PATH_MAX];
    if (n == 1) {
        const char *name = strrchr(v[0], '/');
        name = name ? name + 1 : v[0];
        files_archive_stem(name, base, sizeof(base));
        if (!base[0]) snprintf(base, sizeof(base), "archive");
    } else {
        const char *cwd_name = strrchr(app->files_cwd, '/');
        cwd_name = cwd_name ? cwd_name + 1 : app->files_cwd;
        if (!cwd_name || !*cwd_name) cwd_name = "archive";
        snprintf(base, sizeof(base), "%s", cwd_name);
    }
    char stem[PATH_MAX];
    fs_join(stem, sizeof(stem), app->files_cwd, base);
    char dest[PATH_MAX];
    files_unique_file(stem, ext, dest, sizeof(dest));

    char **argv = xmalloc(sizeof(char *) * (size_t)(n + 6));
    int k = 0;
    argv[k++] = xstrdup(z);
    argv[k++] = xstrdup("a");
    argv[k++] = xstrdup(type);
    argv[k++] = xstrdup(dest);
    argv[k++] = xstrdup("--");
    for (int i = 0; i < n; i++) argv[k++] = xstrdup(v[i]);
    argv[k] = NULL;
    files_free_paths(v, n);
    files_begin_op(app, argv, "正在压缩...");
}

static bool files_spawn_argv(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        execvp(argv[0], argv);
        _exit(127);
    }
    return true;
}

static void files_mime_type(const char *path, char *out, size_t n)
{
    out[0] = '\0';
    char *q = shell_quote(path);
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "xdg-mime query filetype %s 2>/dev/null", q);
    free(q);
    FILE *f = popen(cmd, "r");
    if (f) {
        if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = '\0';
        pclose(f);
    }
    if (out[0]) return;
    char *q2 = shell_quote(path);
    snprintf(cmd, sizeof(cmd), "file -b --mime-type %s 2>/dev/null", q2);
    free(q2);
    f = popen(cmd, "r");
    if (f) {
        if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = '\0';
        pclose(f);
    }
}

static bool files_desktop_supports_mime(const char *path, const char *mime)
{
    if (!path || !*path || !mime || !*mime) return false;
    size_t len = 0;
    char *data = read_file(path, &len);
    if (!data) return false;
    bool supported = false;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line && !supported;
         line = strtok_r(NULL, "\n", &save)) {
        line = str_trim(line);
        if (strncmp(line, "MimeType=", 9) != 0) continue;
        char *list = line + 9;
        char *msave = NULL;
        for (char *tok = strtok_r(list, ";", &msave); tok;
             tok = strtok_r(NULL, ";", &msave)) {
            tok = str_trim(tok);
            if (strcasecmp(tok, mime) == 0) {
                supported = true;
                break;
            }
        }
    }
    free(data);
    return supported;
}

static void files_default_desktop_id(const char *mime, char *out, size_t n)
{
    out[0] = '\0';
    if (!mime || !*mime) return;
    char *qm = shell_quote(mime);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "xdg-mime query default %s 2>/dev/null", qm);
    free(qm);
    FILE *f = popen(cmd, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = '\0';
    pclose(f);
}

void app_files_open_with_begin(App *app)
{
    if (app->files_focus < 0 || app->files_focus >= app->files_list.count) return;
    FsEntry *e = &app->files_list.items[app->files_focus];
    if (e->is_dir) {
        ui_toast(app, "文件夹不能选择打开方式");
        return;
    }
    fs_join(app->files_open_with_path, sizeof(app->files_open_with_path),
            app->files_cwd, e->name);
    bool builtin = editor_is_text_path(app->files_open_with_path);
    if (app->apps.count <= 0 && !builtin) {
        ui_toast(app, "没有可用的应用");
        return;
    }
    files_mime_type(app->files_open_with_path, app->files_open_with_mime,
                    sizeof(app->files_open_with_mime));

    char default_id[256] = "";
    files_default_desktop_id(app->files_open_with_mime, default_id,
                             sizeof(default_id));
    int n = 0;
    int default_index = -1;

    /* 当前默认应用排第一。 */
    for (int i = 0; i < app->apps.count && n < 64; i++) {
        const char *desk = app->apps.apps[i].desktop_file;
        const char *base = desk[0] ? strrchr(desk, '/') : NULL;
        base = base ? base + 1 : desk;
        if (default_id[0] && strcasecmp(base, default_id) == 0) {
            default_index = n;
            app->files_open_with_apps[n++] = i;
            break;
        }
    }
    /* 再列出声明支持该 MIME 的应用。 */
    for (int i = 0; i < app->apps.count && n < 64; i++) {
        bool dup = false;
        for (int j = 0; j < n; j++)
            if (app->files_open_with_apps[j] == i) dup = true;
        if (dup) continue;
        if (files_desktop_supports_mime(app->apps.apps[i].desktop_file,
                                        app->files_open_with_mime)) {
            app->files_open_with_apps[n++] = i;
        }
    }
    /* 其余应用作为兜底，保证总能手动指定。 */
    for (int i = 0; i < app->apps.count && n < 64; i++) {
        bool dup = false;
        for (int j = 0; j < n; j++)
            if (app->files_open_with_apps[j] == i) dup = true;
        if (!dup) app->files_open_with_apps[n++] = i;
    }
    app->files_open_with_count = n;
    app->files_open_with_default = default_index;
    app->files_open_with_builtin = builtin;
    app_open_choice(app, CHOICE_OPEN_WITH, "打开方式");
}

static void files_command_with_path(const char *cmd, const char *path,
                                     char *out, size_t out_size)
{
    char *q = shell_quote(path);
    const char *codes[] = { "%f", "%F", "%u", "%U" };
    const char *hit = NULL;
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char *p = strstr(cmd, codes[i]);
        if (p && (!hit || p < hit)) hit = p;
    }
    if (!hit) {
        snprintf(out, out_size, "%s %s", cmd, q);
    } else {
        size_t pre = (size_t)(hit - cmd);
        const char *post = hit + 2;
        snprintf(out, out_size, "%.*s%s%s", (int)pre, cmd, q, post);
    }
    free(q);
}

void app_files_open_with(App *app, int app_index, bool set_default)
{
    if (app_index < 0 || app_index >= app->apps.count) return;
    if (!app->files_open_with_path[0]) return;
    AppEntry *entry = &app->apps.apps[app_index];
    const char *path = app->files_open_with_path;
    const char *mime = app->files_open_with_mime;

    bool default_set = false;
    if (set_default && entry->desktop_file[0] && mime[0] &&
        files_cmd_in_path("xdg-mime")) {
        const char *base = strrchr(entry->desktop_file, '/');
        base = base ? base + 1 : entry->desktop_file;
        char *qid = shell_quote(base);
        char *qmime = shell_quote(mime);
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "xdg-mime default %s %s >/dev/null 2>&1",
                 qid, qmime);
        free(qid);
        free(qmime);
        FILE *f = popen(cmd, "r");
        if (f) default_set = pclose(f) == 0;
    }

    bool launched = false;
    bool terminal_app = launcher_entry_needs_terminal(entry);
    if (terminal_app) {
        AppEntry tmp = *entry;
        tmp.mode = APP_MODE_HANDOFF;
        tmp.on_return = APP_ON_RETURN_PAUSE;
        tmp.terminal = APP_TERMINAL_YES;
        tmp.background = false;
        snprintf(tmp.id, sizeof(tmp.id), "file-open-%d", app_index);
        files_command_with_path(entry->command, path,
                                tmp.command, sizeof(tmp.command));
        char *err = NULL;
        launched = launcher_start_app(&app->launcher, &tmp, &err);
        if (!launched && err) {
            ui_toast(app, err);
            free(err);
            return;
        }
        free(err);
    }
    if (!launched && entry->desktop_file[0] && files_cmd_in_path("gio")) {
        char *argv[] = {"gio", "launch", entry->desktop_file,
                        (char *)path, NULL};
        launched = files_spawn_argv(argv);
    }
    if (!launched) launched = fs_open_default(path);

    char msg[320];
    if (launched) {
        if (set_default && default_set)
            snprintf(msg, sizeof(msg), "已用 %s 打开，并设为默认打开方式", entry->name);
        else
            snprintf(msg, sizeof(msg), "已用 %s 打开", entry->name);
    } else {
        snprintf(msg, sizeof(msg), "无法打开文件");
    }
    ui_toast(app, msg);
}

void app_files_menu_action(App *app, MenuAction action)
{
    switch (action) {
        case MENU_FILE_NEW:
            app_open_text(app, TEXT_FILE_NAME, 1, "新建文件夹", "", "输入文件夹名称");
            break;
        case MENU_FILE_EXTRACT:
            app_files_extract(app);
            break;
        case MENU_FILE_COMPRESS:
            if (app->files_list.count > 0)
                app_open_choice(app, CHOICE_ARCHIVE_FORMAT, "压缩格式");
            break;
        case MENU_FILE_OPEN_WITH:
            app_files_open_with_begin(app);
            break;
        case MENU_FILE_NEW_TEXT:
            app_open_text(app, TEXT_FILE_NAME, 4, "新建文本文件", "untitled.md",
                          "输入文件名（.txt / .md / .html）");
            break;
        case MENU_FILE_SHARE:
            files_share_from_menu(app);
            break;
        case MENU_FILE_OPEN:
            if (app->files_focus >= 0 && app->files_focus < app->files_list.count) {
                FsEntry *e = &app->files_list.items[app->files_focus];
                if (e->is_dir) {
                    files_enter_dir(app, e->name);
                } else {
                    char path[PATH_MAX];
                    fs_join(path, sizeof(path), app->files_cwd, e->name);
                    if (!fs_has_default_app(path))
                        ui_toast(app, "未安装可用的关联程序");
                    else if (fs_open_default(path))
                        ui_toast(app, "正在打开文件...");
                    else
                        ui_toast(app, "未安装可用的关联程序");
                }
            }
            break;
        case MENU_FILE_RENAME:
            if (app->files_list.count > 0)
                app_open_text(app, TEXT_FILE_NAME, 2, "重命名",
                              app->files_list.items[app->files_focus].name, "输入新名称");
            break;
        case MENU_FILE_COPY: files_copy(app, false); break;
        case MENU_FILE_CUT: files_copy(app, true); break;
        case MENU_FILE_PASTE: files_paste(app); break;
        case MENU_FILE_TRASH: files_delete(app, true); break;
        case MENU_FILE_DELETE:
            app_open_confirm(app, CONFIRM_FILE_DELETE_PERM, 0, "永久删除？",
                             "删除后无法恢复，确定继续吗？");
            break;
        case MENU_FILE_HIDDEN: files_toggle_hidden(app); break;
        case MENU_FILE_HOME:
            snprintf(app->files_cwd, sizeof(app->files_cwd), "%s", fs_home());
            app->files_focus = 0;
            app_files_reload(app);
            break;
        case MENU_FILE_ROOT:
            snprintf(app->files_cwd, sizeof(app->files_cwd), "/");
            app->files_focus = 0;
            app_files_reload(app);
            break;
        case MENU_FILE_FAVORITE: app_files_favorite_toggle(app); break;
        case MENU_FILE_PROPERTIES: files_properties(app); break;
        default: break;
    }
}

static void files_enter_dir(App *app, const char *name)
{
    char path[PATH_MAX];
    fs_join(path, sizeof(path), app->files_cwd, name);
    if (!fs_is_dir(path)) return;
    snprintf(app->files_cwd, sizeof(app->files_cwd), "%s", path);
    app->files_focus = 0;
    app_files_reload(app);
}

static void files_up(App *app)
{
    char parent[PATH_MAX];
    if (!fs_parent(app->files_cwd, parent, sizeof(parent))) {
        ui_toast(app, "已在根目录");
        return;
    }
    snprintf(app->files_cwd, sizeof(app->files_cwd), "%s", parent);
    app->files_focus = 0;
    app_files_reload(app);
}

static void files_activate(App *app)
{
    if (app->files_focus < 0 || app->files_focus >= app->files_list.count) return;
    FsEntry *e = &app->files_list.items[app->files_focus];
    if (e->is_dir) {
        files_enter_dir(app, e->name);
        return;
    }
    if (files_is_archive(e->name)) {
        app_files_extract(app);
        return;
    }
    char path[PATH_MAX];
    fs_join(path, sizeof(path), app->files_cwd, e->name);
    if (!fs_has_default_app(path)) {
        ui_toast(app, "未安装可用的关联程序");
        return;
    }
    if (fs_open_default(path)) {
        char msg[300];
        snprintf(msg, sizeof(msg), "正在打开 %s", e->name);
        ui_toast(app, msg);
    } else {
        ui_toast(app, "未安装可用的关联程序");
    }
}

static void files_pick_activate(App *app)
{
    if (app->files_focus < 0 || app->files_focus >= app->files_list.count) return;
    FsEntry *e = &app->files_list.items[app->files_focus];
    if (e->is_dir) {
        files_enter_dir(app, e->name);
        return;
    }
    if (app->files_pick == FILES_PICK_FILE) {
        char path[PATH_MAX];
        if (files_current_path(app, path, sizeof(path))) files_pick_accept(app, path);
    } else {
        ui_toast(app, "请选择目录");
    }
}

static void files_smb_connect_uri(App *app, const char *uri, const char *user)
{
    const char *p = uri;
    if (strncmp(p, "smb://", 6) == 0) p += 6;
    char host[256] = "", share[128] = "";
    const char *slash = strchr(p, '/');
    if (slash) {
        size_t n = (size_t)(slash - p);
        if (n >= sizeof(host)) n = sizeof(host) - 1;
        memcpy(host, p, n);
        host[n] = '\0';
        snprintf(share, sizeof(share), "%s", slash + 1);
    } else {
        snprintf(host, sizeof(host), "%s", p);
    }
    snprintf(app->smb_host, sizeof(app->smb_host), "%s", host);
    snprintf(app->smb_share, sizeof(app->smb_share), "%s", share);
    snprintf(app->smb_user, sizeof(app->smb_user), "%s", user ? user : "");
    app->smb_pass[0] = '\0';
    if (!app->smb_user[0]) {
        files_smb_start(app);
    } else {
        app_open_text(app, TEXT_SMB_PASS, 0, "共享密码", "",
                      "输入密码后连接共享文件夹");
    }
}

static void files_smb_prompt_saved(App *app, const FileLoc *e)
{
    files_smb_connect_uri(app, e->uri, e->user);
}

static void files_open_location(App *app, const char *path)
{
    if (!path || !*path) {
        ui_toast(app, "路径不可用");
        return;
    }
    if (fs_is_dir(path)) {
        files_open_path(app, path);
        return;
    }
    if (!fs_exists(path)) {
        ui_toast(app, "收藏路径不存在");
        return;
    }
    char parent[PATH_MAX];
    if (!fs_parent(path, parent, sizeof(parent)))
        snprintf(parent, sizeof(parent), "/");
    snprintf(app->files_cwd, sizeof(app->files_cwd), "%s", parent);
    app->files_on_sidebar = false;
    app->files_message[0] = '\0';
    files_load(app);
    const char *base = path_basename(path);
    for (int i = 0; i < app->files_list.count; i++) {
        if (strcmp(app->files_list.items[i].name, base) == 0) {
            app->files_focus = i;
            break;
        }
    }
    if (app->screen == SCREEN_FILES) app_rebuild(app);
}

static void files_sidebar_activate(App *app, int index)
{
    if (index < 0 || index >= app->files_loc_count) return;
    FileLoc *e = &app->files_locs[index];
    app->files_side_focus = index;
    app->files_on_sidebar = false;
    if (app->layout.w < 400) app->files_sidebar_open = false;
    if (e->kind == FILE_LOC_PATH || e->kind == FILE_LOC_SHARE) {
        app->files_panel = FILES_PANEL_LIST;
        if (e->kind == FILE_LOC_PATH) files_open_location(app, e->path);
        else files_smb_prompt_saved(app, e);
    } else if (e->kind == FILE_LOC_DEVICE) {
        if (e->mounted && e->path[0]) files_open_path(app, e->path);
        else files_mount_start(app, index);
    } else if (e->kind == FILE_LOC_CONNECT) {
        app->files_panel = FILES_PANEL_CONNECT;
        app->smb_ui.focus = 0;
        app->smb_ui.view = 0;
        app->files_on_sidebar = false;
        app_rebuild(app);
    } else if (e->kind == FILE_LOC_MANAGE) {
        app->files_panel = FILES_PANEL_MANAGE;
        app->smb_ui.focus = 0;
        app->files_on_sidebar = false;
        smb_local_refresh(app);
        smb_service_query(app);
        app_rebuild(app);
    } else if (e->kind == FILE_LOC_MTP) {
        app->files_mtp_panel = true;
        app->files_mtp_focus = 0;
        app->files_on_sidebar = false;
        app_rebuild(app);
    }
}

static void files_action_toggle_hidden(App *app)
{
    files_toggle_hidden(app);
    ui_toast(app, app->files_show_hidden ? "已显示隐藏文件" : "已隐藏隐藏文件");
}

static void files_action_refresh(App *app)
{
    files_load(app);
    files_scan_locations(app);
    app->files_message[0] = '\0';
    app_rebuild(app);
    ui_toast(app, "已刷新");
}

static void input_files_sidebar(App *app, InputAction action)
{
    if (action == IN_SELECT) {
        audio_play(SND_NAV);
        files_unmount_start(app, app->files_side_focus);
        return;
    }
    if (action == IN_START && app->files_pick != FILES_PICK_NONE) {
        files_pick_accept(app, app->files_cwd);
        return;
    }
    if (action == IN_UP) {
        if (app->files_side_focus > 0) app->files_side_focus--;
        audio_play(SND_NAV);
        ui_update_files(app);
        return;
    }
    if (action == IN_DOWN) {
        if (app->files_side_focus + 1 < app->files_loc_count) app->files_side_focus++;
        audio_play(SND_NAV);
        ui_update_files(app);
        return;
    }
    if (action == IN_CONFIRM || action == IN_A) {
        audio_play(SND_ENTER);
        files_sidebar_activate(app, app->files_side_focus);
        return;
    }
    if (action == IN_CANCEL || action == IN_B || action == IN_RIGHT) {
        audio_play(SND_BACK);
        app->files_on_sidebar = false;
        if (app->layout.w < 400) app->files_sidebar_open = false;
        if (app->screen == SCREEN_FILES) app_rebuild(app);
        else ui_update_files(app);
        return;
    }
    if (action == IN_X || action == IN_START) {
        app->modal = MODAL_MENU;
        app->menu_focus = 0;
        ui_build_modal(app);
        return;
    }
    if (action == IN_Y) {
        files_scan_locations(app);
        if (app->screen == SCREEN_FILES) app_rebuild(app);
        return;
    }
}

static void input_files_pick(App *app, InputAction action)
{
    if (action == IN_UP) {
        if (app->files_focus > 0) {
            app->files_focus--;
            audio_play(SND_NAV);
            ui_update_files(app);
        }
        return;
    }
    if (action == IN_DOWN) {
        if (app->files_focus + 1 < app->files_list.count) {
            app->files_focus++;
            audio_play(SND_NAV);
            ui_update_files(app);
        }
        return;
    }
    if (action == IN_LEFT) {
        if (app->files_loc_count > 0) {
            audio_play(SND_NAV);
            app->files_on_sidebar = true;
            app->files_sidebar_open = true;
            for (int i = 0; i < app->files_loc_count; i++) {
                if (app->files_locs[i].path[0] &&
                    strcmp(app->files_locs[i].path, app->files_cwd) == 0) {
                    app->files_side_focus = i;
                    break;
                }
            }
            if (app->screen == SCREEN_FILES) app_rebuild(app);
            else ui_update_files(app);
        }
        return;
    }
    if (action == IN_RIGHT) {
        int last = app->files_list.count - 1;
        app->files_focus = app->files_focus + 10 < last ? app->files_focus + 10 :
                           (last > 0 ? last : 0);
        ui_update_files(app);
        return;
    }
    if (action == IN_CONFIRM || action == IN_A) {
        audio_play(SND_ENTER);
        files_pick_activate(app);
        return;
    }
    if (action == IN_START || action == IN_Y) {
        if (app->files_pick == FILES_PICK_DIR) {
            files_pick_accept(app, app->files_cwd);
        } else if (app->files_focus >= 0 &&
                   app->files_focus < app->files_list.count &&
                   !app->files_list.items[app->files_focus].is_dir) {
            char path[PATH_MAX];
            if (files_current_path(app, path, sizeof(path)))
                files_pick_accept(app, path);
        } else {
            ui_toast(app, "请选择文件");
        }
        return;
    }
    if (action == IN_CANCEL || action == IN_B) {
        audio_play(SND_BACK);
        files_pick_up(app);
        return;
    }
    if (action == IN_X) {
        app->modal = MODAL_MENU;
        app->menu_focus = 0;
        ui_build_modal(app);
        return;
    }
}

void input_files(App *app, InputAction action)
{
    if (app->files_mtp_panel) {
        if (action == IN_UP || action == IN_DOWN) {
            app->files_mtp_focus = (app->files_mtp_focus + 1) % 2;
            app_rebuild(app);
            audio_play(SND_NAV);
        } else if (action == IN_CONFIRM || action == IN_A) {
            audio_play(SND_ENTER);
            files_mtp_apply(app, app->files_mtp_focus);
            app_rebuild(app);
        } else if (action == IN_CANCEL || action == IN_B) {
            app->files_mtp_panel = false;
            app_rebuild(app);
            audio_play(SND_BACK);
        } else if (action == IN_LEFT) {
            app->files_mtp_panel = false;
            app->files_on_sidebar = true;
            for (int i = 0; i < app->files_loc_count; i++)
                if (app->files_locs[i].kind == FILE_LOC_MTP)
                    app->files_side_focus = i;
            app_rebuild(app);
            audio_play(SND_NAV);
        }
        return;
    }
    if (app->files_panel != FILES_PANEL_LIST) {
        if (app->files_on_sidebar) { input_files_sidebar(app, action); return; }
        files_share_input(app, action);
        return;
    }
    if (action == IN_LT) {
        files_action_toggle_hidden(app);
        return;
    }
    if (action == IN_RT) {
        files_action_refresh(app);
        return;
    }
    if (app->files_on_sidebar) {
        input_files_sidebar(app, action);
        return;
    }
    if (app->files_pick != FILES_PICK_NONE) {
        input_files_pick(app, action);
        return;
    }
    if (app->files_op_running) {
        if (action == IN_CANCEL || action == IN_B) {
            if (app->files_op_pid > 0) kill(-app->files_op_pid, SIGTERM);
            ui_toast(app, "正在取消文件操作");
        }
        return;
    }
    if (action == IN_UP) {
        if (app->files_focus > 0) {
            app->files_focus--;
            audio_play(SND_NAV);
            ui_update_files(app);
        }
        return;
    }
    if (action == IN_DOWN) {
        if (app->files_focus + 1 < app->files_list.count) {
            app->files_focus++;
            audio_play(SND_NAV);
            ui_update_files(app);
        }
        return;
    }
    if (action == IN_LEFT) {
        if (app->files_loc_count > 0) {
            audio_play(SND_NAV);
            app->files_on_sidebar = true;
            app->files_sidebar_open = true;
            for (int i = 0; i < app->files_loc_count; i++) {
                if (app->files_locs[i].path[0] &&
                    strcmp(app->files_locs[i].path, app->files_cwd) == 0) {
                    app->files_side_focus = i;
                    break;
                }
            }
            if (app->screen == SCREEN_FILES) app_rebuild(app);
            else ui_update_files(app);
        }
        return;
    }
    if (action == IN_RIGHT) {
        int last = app->files_list.count - 1;
        app->files_focus = app->files_focus + 10 < last ? app->files_focus + 10 : (last > 0 ? last : 0);
        audio_play(SND_NAV);
        ui_update_files(app);
        return;
    }
    if (action == IN_X || action == IN_START) {
        app->modal = MODAL_MENU;
        app->menu_focus = 0;
        ui_build_modal(app);
        return;
    }
    if (action == IN_Y) {
        if (app->files_list.count > 0 && app->files_marked) {
            audio_play(SND_NAV);
            app->files_marked[app->files_focus] = !app->files_marked[app->files_focus];
            app->files_marked_count += app->files_marked[app->files_focus] ? 1 : -1;
            ui_update_files(app);
        }
        return;
    }
    if (action == IN_CONFIRM || action == IN_A) {
        audio_play(SND_ENTER);
        files_activate(app);
        return;
    }
    if (action == IN_CANCEL || action == IN_B) {
        audio_play(SND_BACK);
        files_up(app);
        return;
    }
}

typedef struct {
    App *app;
    int index;
} SideRowCtx;

typedef struct {
    int name_x, name_w;
    int date_x, date_w;
    int size_x, size_w;
    int kind_x, kind_w;
    int star_x, star_w;
} FileCols;

static FileCols files_columns(int w, int row_h)
{
    FileCols c;
    memset(&c, 0, sizeof(c));
    c.star_w = row_h >= 34 ? 22 : 18;
    c.date_w = w >= 480 ? 130 : 0;
    c.kind_w = w >= 560 ? 78 : 0;
    c.size_w = w >= 340 ? 76 : 0;
    int right = w - 6;
    c.star_x = right - c.star_w;
    right = c.star_x - 6;
    if (c.kind_w) { c.kind_x = right - c.kind_w; right = c.kind_x - 8; }
    if (c.size_w) { c.size_x = right - c.size_w; right = c.size_x - 8; }
    if (c.date_w) { c.date_x = right - c.date_w; right = c.date_x - 8; }
    c.name_x = 34;
    c.name_w = right - c.name_x;
    if (c.name_w < 40) c.name_w = 40;
    return c;
}

static const char *files_kind_label(const FsEntry *e)
{
    if (e->is_dir) return "文件夹";
    const char *dot = strrchr(e->name, '.');
    if (!dot || !dot[1]) return "文件";
    const char *ext = dot + 1;
    if (strcasecmp(ext, "png") == 0 || strcasecmp(ext, "jpg") == 0 ||
        strcasecmp(ext, "jpeg") == 0 || strcasecmp(ext, "webp") == 0 ||
        strcasecmp(ext, "gif") == 0 || strcasecmp(ext, "bmp") == 0)
        return "图像";
    if (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mkv") == 0 ||
        strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mov") == 0 ||
        strcasecmp(ext, "webm") == 0)
        return "影片";
    if (strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "flac") == 0 ||
        strcasecmp(ext, "ogg") == 0 || strcasecmp(ext, "wav") == 0 ||
        strcasecmp(ext, "m4a") == 0)
        return "音频";
    if (strcasecmp(ext, "zip") == 0 || strcasecmp(ext, "7z") == 0 ||
        strcasecmp(ext, "rar") == 0 || strcasecmp(ext, "tar") == 0 ||
        strcasecmp(ext, "gz") == 0)
        return "压缩包";
    if (strcasecmp(ext, "iso") == 0 || strcasecmp(ext, "img") == 0)
        return "磁盘映像";
    if (strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "md") == 0 ||
        strcasecmp(ext, "pdf") == 0 || strcasecmp(ext, "ini") == 0 ||
        strcasecmp(ext, "conf") == 0)
        return "文稿";
    return "文件";
}

static void files_tool_up_cb(lv_event_t *e) { files_up(lv_event_get_user_data(e)); }
static void files_tool_refresh_cb(lv_event_t *e)
{
    files_action_refresh(lv_event_get_user_data(e));
}
static void files_tool_hidden_cb(lv_event_t *e)
{
    files_action_toggle_hidden(lv_event_get_user_data(e));
}
static void files_tool_side_cb(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    app->files_sidebar_open = !app->files_sidebar_open;
    app->files_on_sidebar = false;
    app_rebuild(app);
}

static int files_keycap_width(const char *key)
{
    const char *label = (key && *key) ? key : "L2";
    int w = ui_unscale_x(lv_text_get_width(label,
                                           (uint32_t)strlen(label), FONT_XS, 0)) + 8;
    return ui_clamp(w, 18, 28);
}

static lv_obj_t *files_tool_button(App *app, lv_obj_t *parent, int x, int y,
                                   const char *icon, const char *key,
                                   lv_event_cb_t cb)
{
    int kw = files_keycap_width(key);
    int gw = key && *key ? kw + 2 + 24 : 24;
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_pos(g, ui_scale_x(x), ui_scale_y(y));
    lv_obj_set_size(g, ui_scale_x(gw), ui_scale_y(24));
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);
    int bx = 0;
    if (key && *key) {
        lv_obj_t *k = ui_rect(g, 0, 4, kw, 16, 5, PAL.card);
        lv_obj_set_style_bg_opa(k, 185, 0);
        lv_obj_set_style_border_width(k, 1, 0);
        lv_obj_set_style_border_color(k, lv_color_hex(PAL.border), 0);
        lv_obj_set_style_border_opa(k, 100, 0);
        ui_label_center(k, key, FONT_XS, PAL.text3, 0, 1, kw, 14);
        bx = kw + 2;
    }
    lv_obj_t *b = ui_rect(g, bx, 0, 24, 24, 8, PAL.card);
    lv_obj_set_style_bg_opa(b, 150, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(PAL.border), 0);
    lv_obj_set_style_border_opa(b, 90, 0);
    ui_label_center(b, icon, FONT_SM, PAL.text2, 0, 4, 24, 16);
    lv_obj_add_event_cb(g, cb, LV_EVENT_CLICKED, app);
    return g;
}

static void file_row_pressed(lv_event_t *e)
{
    FileRowCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    ctx->pressed = true;
    ctx->press_ms = lv_tick_get();
}

static void file_row_released(lv_event_t *e)
{
    FileRowCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->pressed) return;
    ctx->pressed = false;
    lv_indev_t *indev = lv_indev_active();
    if (indev && (lv_indev_get_scroll_obj(indev) ||
                  lv_indev_get_scroll_dir(indev) != LV_DIR_NONE ||
                  lv_indev_get_gesture_dir(indev) != LV_DIR_NONE))
        return;
    if (lv_tick_get() - ctx->press_ms < 450) return;
    App *app = ctx->app;
    ctx->long_fired = true;
    app->files_on_sidebar = false;
    app->files_focus = ctx->index;
    ui_update_files(app);
    audio_play(SND_NAV_STRONG);
    app_input(app, IN_Y, NULL);
}

static void file_row_clicked(lv_event_t *e)
{
    FileRowCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->long_fired) {
        ctx->long_fired = false;
        return;
    }
    audio_play(SND_ENTER);
    App *app = ctx->app;
    if (app->files_on_sidebar) {
        app->files_on_sidebar = false;
        if (app->layout.w < 400) app->files_sidebar_open = false;
    }
    /* Android-style: one tap opens the file/folder immediately. */
    app->files_focus = ctx->index;
    ui_update_files(app);
    if (app->files_pick != FILES_PICK_NONE) files_pick_activate(app);
    else files_activate(app);
}

static void file_row_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void side_row_clicked(lv_event_t *e)
{
    audio_play(SND_ENTER);
    SideRowCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    App *app = ctx->app;
    app->files_on_sidebar = true;
    app->files_side_focus = ctx->index;
    files_sidebar_activate(app, ctx->index);
}

static void side_row_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void files_build_sidebar(App *app, int x, int y, int w, int h, bool overlay)
{
    lv_obj_t *panel = ui_rect(app->ui.root, x, y, w, h, 14,
                              overlay ? PAL.surface : PAL.card);
    lv_obj_set_style_bg_opa(panel, overlay ? 250 : 105, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border), 0);
    lv_obj_set_style_border_opa(panel, overlay ? 160 : 70, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    ui_scrollbar_style(panel);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    app->ui.files_side_panel = panel;
    app->ui.files_side_row_count = app->files_loc_count;
    if (overlay)
        files_tool_button(app, panel, w - 28, 4, LV_SYMBOL_CLOSE, NULL, files_tool_side_cb);

    int row_h = app->layout.rail_row_h;
    lv_font_t *row_font = row_h < 22 ? FONT_XS :
                          (app->layout.w < 600 ? FONT_SM : FONT_MD);
    int row_line = ui_unscale_y(lv_font_get_line_height(row_font));
    int cy = 6;
    int section = -1;
    int loc_group = -1;
    static const char *section_names[4] = {"个人收藏", "位置", "共享", "USB"};
    for (int i = 0; i < app->files_loc_count; i++) {
        FileLoc *e = &app->files_locs[i];
        if (e->section != section) {
            section = e->section;
            loc_group = -1;
            int sec_h = ui_text_height(FONT_XS);
            ui_label(panel, section_names[section < 0 ? 0 : (section > 3 ? 3 : section)],
                     FONT_XS, PAL.text3, 10, cy + 3,
                     w - (overlay ? 44 : 20), sec_h);
            cy += sec_h + 4;
        }
        if (e->section == 1) {
            int group = 0;
            if (e->kind == FILE_LOC_DEVICE) group = e->removable ? 2 : 1;
            if (loc_group >= 0 && group != loc_group) {
                lv_obj_t *divider = ui_rect(panel, 12, cy + 3, w - 24, 2, 1, PAL.text3);
                lv_obj_set_style_bg_opa(divider, 220, 0);
                cy += 10;
            }
            loc_group = group;
        }
        lv_obj_t *row = ui_rect(panel, 4, cy, w - 8, row_h, 8, PAL.accent);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        const char *icon = LV_SYMBOL_DIRECTORY;
        uint32_t color = PAL.text2;
        if (e->kind == FILE_LOC_DEVICE) {
            icon = LV_SYMBOL_DRIVE;
            color = e->mounted ? PAL.text2 : PAL.text3;
        } else if (e->kind == FILE_LOC_SHARE) {
            icon = LV_SYMBOL_WIFI;
            color = PAL.text2;
        } else if (e->kind == FILE_LOC_CONNECT) {
            icon = LV_SYMBOL_PLUS;
            color = PAL.accent2;
        } else if (e->kind == FILE_LOC_MANAGE) {
            icon = LV_SYMBOL_SETTINGS;
            color = PAL.text2;
        } else if (e->kind == FILE_LOC_MTP) {
            icon = LV_SYMBOL_USB;
            color = mtp_enabled() ? PAL.accent2 : PAL.text3;
        } else if (!strcmp(e->path, "/")) {
            icon = LV_SYMBOL_DRIVE;
        }
        if (strcmp(e->label, "回收站") == 0) {
            icon = LV_SYMBOL_TRASH;
            color = PAL.text2;
        }
        int icon_w = row_line + 4 > 20 ? row_line + 4 : 20;
        int icon_x = 12;
        int label_x = icon_x + icon_w + 10;
        ui_label_center(row, icon, row_font, color,
                        icon_x, (row_h - row_line) / 2, icon_w, row_line + 2);
        lv_obj_t *label = ui_label(row, e->label, row_font, color,
                                   label_x, (row_h - row_line) / 2,
                                   w - label_x - 10, row_line + 2);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        if (!e->mounted && e->kind == FILE_LOC_DEVICE)
            lv_obj_set_style_text_color(label, lv_color_hex(PAL.text3), 0);
        SideRowCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->index = i;
        lv_obj_add_event_cb(row, side_row_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, side_row_free, LV_EVENT_DELETE, ctx);
        app->ui.files_side_rows[i] = row;
        cy += row_h + 2;
    }
    app->ui.files_side_panel = panel;
}

static void files_mtp_apply(App *app, int focus)
{
    if (app->files_mtp_running) {
        ui_toast(app, "正在切换 USB 传输，请稍候");
        return;
    }
    if (focus == 0)
        files_mtp_start(app, mtp_enabled() ? "off" : "on", NULL);
    else
        files_mtp_start(app, "mode", mtp_readonly() ? "rw" : "ro");
    app->files_mtp_focus = focus;
}

typedef struct {
    App *app;
    int focus;
} MtpOptionCtx;

static void mtp_option_clicked(lv_event_t *e)
{
    MtpOptionCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->app) return;
    files_mtp_apply(ctx->app, ctx->focus);
    if (ctx->app->screen == SCREEN_FILES) app_rebuild(ctx->app);
}

static void mtp_option_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void files_build_mtp_panel(App *app, int x, int y, int w, int h)
{
    if (h < 80) h = 80;
    lv_obj_t *panel = ui_rect(app->ui.root, x, y, w, h, 18, PAL.surface);
    lv_obj_set_style_bg_opa(panel, 255, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border), 0);

    int inner = ui_clamp(w * 4 / 100, 14, 28);
    int title_h = ui_text_height(FONT_XL);
    ui_label(panel, "MTP", FONT_XL, PAL.text, inner, 12, w - inner * 2,
             title_h);
    int sub_h = ui_text_height(FONT_XS);
    char sub[192];
    if (app->files_mtp_running)
        snprintf(sub, sizeof(sub), "正在切换，USB 会短暂断开");
    else
        snprintf(sub, sizeof(sub), "状态：%s   %s",
                 mtp_enabled() ? "已启用" : "未启用",
                 mtp_readonly() ? "只读" : "读写");
    ui_label(panel, sub, FONT_XS, PAL.text3, inner, 12 + title_h + 4,
             w - inner * 2, sub_h);

    int top = 12 + title_h + sub_h + 16;
    int rh = ui_clamp((h - top - 16) / 2, 46, 72);
    const char *labels[2] = { "USB 文件传输", "访问权限" };
    char values[2][32];
    snprintf(values[0], sizeof(values[0]), "%s", mtp_enabled() ? "开" : "关");
    snprintf(values[1], sizeof(values[1]), "%s", mtp_readonly() ? "只读" : "读写");
    for (int i = 0; i < 2; i++) {
        int ry = top + i * (rh + 8);
        bool focus = app->files_mtp_focus == i;
        lv_obj_t *row = ui_rect(panel, inner, ry, w - inner * 2, rh, 12,
                                focus ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(row, focus ? 42 : 60, 0);
        lv_obj_set_style_border_width(row, focus ? 2 : 0, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(PAL.accent), 0);
        if (focus) ui_set_focus_ring(row, true, PAL.accent);
        int line = ui_text_height(FONT_SM);
        int vy = ui_text_height(FONT_XS);
        int text_h = line > vy ? line : vy;
        ui_label(row, labels[i], FONT_SM, PAL.text, 16,
                 (rh - text_h) / 2, (w - inner * 2) * 60 / 100, line);
        lv_obj_t *vv = ui_label(row, values[i], FONT_XS, PAL.accent2,
                                (w - inner * 2) * 62 / 100,
                                (rh - text_h) / 2,
                                (w - inner * 2) * 36 / 100, vy);
        lv_obj_set_style_text_align(vv, LV_TEXT_ALIGN_RIGHT, 0);
        MtpOptionCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->focus = i;
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, mtp_option_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, mtp_option_free, LV_EVENT_DELETE, ctx);
    }
}

/* ── Share panel: LAN connect + local share management ───────────────────── */

static void files_share_input(App *app, InputAction action);

typedef struct {
    App *app;
    int index;
} SharePanelCtx;

static void share_panel_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void share_panel_clicked(lv_event_t *e)
{
    SharePanelCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    ctx->app->smb_ui.focus = ctx->index;
    files_share_input(ctx->app, IN_CONFIRM);
    app_rebuild(ctx->app);
}

static void share_panel_clickable(App *app, lv_obj_t *obj, int index)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    SharePanelCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->index = index;
    lv_obj_add_event_cb(obj, share_panel_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(obj, share_panel_free, LV_EVENT_DELETE, ctx);
}

static int share_manage_rows(App *app)
{
    return app->smb_ui.local_count + 1 + app->share_count;
}

static int share_focus_rows(App *app)
{
    SmbUi *ui = &app->smb_ui;
    if (app->files_panel == FILES_PANEL_CONNECT)
        return ui->view == 1 ? (int)ui->share_count : 2 + ui->host_count;
    return share_manage_rows(app);
}

static void files_share_from_menu(App *app)
{
    SmbUi *ui = &app->smb_ui;
    char path[PATH_MAX];
    bool sel = app->files_focus >= 0 && app->files_focus < app->files_list.count;
    if (sel && app->files_list.items[app->files_focus].is_dir) {
        fs_join(path, sizeof(path), app->files_cwd,
                app->files_list.items[app->files_focus].name);
    } else {
        snprintf(path, sizeof(path), "%s", app->files_cwd);
    }
    smb_local_refresh(app);
    int found = -1;
    for (int i = 0; i < ui->local_count; i++)
        if (strcmp(ui->local[i].path, path) == 0) { found = i; break; }
    ui->edit_index = found;
    ui->edit_focus = 0;
    if (found < 0) {
        snprintf(ui->pending_path, sizeof(ui->pending_path), "%s", path);
        ui->pending_writable = false;
    }
    app->modal = MODAL_SMB_SHARE;
    ui_build_modal(app);
}

static void files_share_new(App *app)
{
    app->smb_ui.pending_path[0] = '\0';
    app_open_text(app, TEXT_SMB_SHARE_PATH, 0, "共享目录", fs_home(),
                  "输入要共享的目录，按 Y 可浏览选择");
}

void app_files_share_text_done(App *app)
{
    SmbUi *ui = &app->smb_ui;
    if (app->text_purpose == TEXT_SMB_SHARE_PATH) {
        char value[2048];
        snprintf(value, sizeof(value), "%s", app->input_buf);
        char *trimmed = str_trim(value);
        char *abs = path_absolute(trimmed);
        if (!abs || !fs_is_dir(abs)) {
            ui_toast(app, "目录不存在");
            free(abs);
            return;
        }
        snprintf(ui->pending_path, sizeof(ui->pending_path), "%s", abs);
        free(abs);
        const char *base = strrchr(ui->pending_path, '/');
        base = base ? base + 1 : ui->pending_path;
        char name[96];
        snprintf(name, sizeof(name), "%s", *base ? base : "share");
        app_open_text(app, TEXT_SMB_SHARE_NAME, 0, "共享名称", name,
                      "局域网中显示的共享名称");
        return;
    }
    if (app->text_purpose == TEXT_SMB_SHARE_NAME) {
        char value[256];
        snprintf(value, sizeof(value), "%s", app->input_buf);
        char *name = str_trim(value);
        if (!*name) { ui_toast(app, "请输入共享名称"); return; }
        char err[192] = "";
        bool ok = smb_local_add(app, name, ui->pending_path, false, err, sizeof(err));
        external_osk_hide(app);
        app->modal = MODAL_NONE;
        app->modal_closing = false;
        ui_set_input_active(false);
        ui_close_modal_visual(app);
        ui->focus = share_manage_rows(app) > 0 ? ui->local_count : 0;
        app_rebuild(app);
        ui_toast(app, ok ? "共享已添加" : (err[0] ? err : "添加共享失败"));
    }
}

void app_files_smb_auth_done(App *app)
{
    SmbUi *ui = &app->smb_ui;
    if (app->text_purpose == TEXT_SMB_AUTH_USER) {
        char value[256];
        snprintf(value, sizeof(value), "%s", app->input_buf);
        char *user = str_trim(value);
        if (!*user) {
            external_osk_hide(app);
            app->modal = MODAL_NONE;
            app->modal_closing = false;
            ui_set_input_active(false);
            ui_close_modal_visual(app);
            app_rebuild(app);
            return;
        }
        snprintf(ui->user, sizeof(ui->user), "%s", user);
        app_open_text(app, TEXT_SMB_AUTH_PASS, 0, "共享密码", "",
                      "输入密码，留空表示无密码");
        return;
    }
    if (app->text_purpose == TEXT_SMB_AUTH_PASS) {
        snprintf(ui->pass, sizeof(ui->pass), "%s", app->input_buf);
        external_osk_hide(app);
        app->modal = MODAL_NONE;
        app->modal_closing = false;
        ui_set_input_active(false);
        ui_close_modal_visual(app);
        app_rebuild(app);
        if (ui->host_sel >= 0 && ui->host_sel < ui->host_count)
            smb_fetch_shares(app, ui->hosts[ui->host_sel].addr);
    }
}

static void files_share_open_settings(App *app, int index)
{
    SmbUi *ui = &app->smb_ui;
    if (index < 0 || index >= ui->local_count) return;
    ui->edit_index = index;
    ui->edit_focus = 0;
    app->modal = MODAL_SMB_SHARE;
    ui_build_modal(app);
}

static void files_share_mount(App *app, const char *host, const char *share)
{
    snprintf(app->smb_host, sizeof(app->smb_host), "%s", host ? host : "");
    snprintf(app->smb_share, sizeof(app->smb_share), "%s", share ? share : "");
    snprintf(app->smb_user, sizeof(app->smb_user), "%s", app->smb_ui.user);
    snprintf(app->smb_pass, sizeof(app->smb_pass), "%s", app->smb_ui.pass);
    files_smb_start(app);
}

static void files_share_remove_bookmark(App *app, int index)
{
    if (index < 0 || index >= app->share_count) return;
    for (int i = index; i + 1 < app->share_count; i++)
        app->shares[i] = app->shares[i + 1];
    app->share_count--;
    files_shares_save(app);
    files_scan_locations(app);
    ui_toast(app, "已移除收藏");
    app_rebuild(app);
}

static void files_share_input(App *app, InputAction action)
{
    SmbUi *ui = &app->smb_ui;
    int rows = share_focus_rows(app);
    if (rows < 1) rows = 1;

    bool on_buttons = app->files_panel == FILES_PANEL_CONNECT && ui->view == 0 &&
                      ui->focus < 2;
    if (action == IN_LEFT) {
        /* Only the left-most item leaves the panel; the two side-by-side
         * buttons step left/right between each other first. */
        if (on_buttons && ui->focus == 1) {
            ui->focus = 0;
            app_rebuild(app);
            audio_play(SND_NAV);
            return;
        }
        app->files_on_sidebar = true;
        app_rebuild(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_RIGHT) {
        if (on_buttons && ui->focus == 0) {
            ui->focus = 1;
            app_rebuild(app);
            audio_play(SND_NAV);
        }
        return;
    }
    if (action == IN_UP) {
        if (on_buttons) return;
        if (app->files_panel == FILES_PANEL_CONNECT && ui->focus == 2) {
            ui->focus = 1;                 /* first host -> buttons */
            app_rebuild(app);
            audio_play(SND_NAV);
            return;
        }
        if (ui->focus > 0) ui->focus--;
        app_rebuild(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_DOWN) {
        if (on_buttons) {
            if (ui->host_count > 0) {
                ui->focus = 2;
                app_rebuild(app);
                audio_play(SND_NAV);
            }
            return;
        }
        if (ui->focus + 1 < rows) ui->focus++;
        app_rebuild(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_X) {
        if (app->files_panel == FILES_PANEL_CONNECT) {
            if (ui->view == 1 && ui->host_sel < ui->host_count)
                smb_fetch_shares(app, ui->hosts[ui->host_sel].addr);
            else
                smb_scan_start(app);
            app_rebuild(app);
            audio_play(SND_NAV);
        } else {
            char err[160] = "";
            bool on = !ui->service_on;
            if (smb_service_set(app, on, err, sizeof(err)))
                ui_toast(app, on ? "共享服务已开启" : "共享服务已停止");
            else
                ui_toast(app, err[0] ? err : "切换共享服务失败");
            app_rebuild(app);
        }
        return;
    }
    if (action == IN_Y) {
        if (app->files_panel == FILES_PANEL_CONNECT) {
            files_smb_begin(app);
            return;
        }
        int local_n = ui->local_count;
        if (ui->focus < local_n) {
            /* delete the selected local share */
            SmbLocalShare *sh = &ui->local[ui->focus];
            char name[128];
            snprintf(name, sizeof(name), "%s", sh->name);
            char err[160] = "";
            if (smb_local_remove(app, name, err, sizeof(err))) {
                ui_toast(app, "共享已删除");
                if (ui->focus >= share_manage_rows(app))
                    ui->focus = share_manage_rows(app) > 0 ? share_manage_rows(app) - 1 : 0;
            } else {
                ui_toast(app, err[0] ? err : "删除共享失败");
            }
        } else if (ui->focus == local_n) {
            files_share_new(app);
            return;
        } else {
            files_share_remove_bookmark(app, ui->focus - local_n - 1);
            return;
        }
        app_rebuild(app);
        return;
    }
    if (action == IN_CONFIRM || action == IN_A) {
        if (app->files_panel == FILES_PANEL_CONNECT) {
            if (ui->view == 0) {
                if (ui->focus == 0) { smb_scan_start(app); app_rebuild(app); }
                else if (ui->focus == 1) files_smb_begin(app);
                else {
                    int idx = ui->focus - 2;
                    if (idx >= 0 && idx < ui->host_count) {
                        if (strcmp(ui->auth_host, ui->hosts[idx].addr) != 0) {
                            ui->auth_host[0] = '\0';
                            ui->user[0] = '\0';
                            ui->pass[0] = '\0';
                            snprintf(ui->auth_host, sizeof(ui->auth_host), "%s",
                                     ui->hosts[idx].addr);
                        }
                        ui->host_sel = idx;
                        ui->view = 1;
                        ui->focus = 0;
                        ui->share_count = 0;
                        smb_fetch_shares(app, ui->hosts[idx].addr);
                        app_rebuild(app);
                    }
                }
            } else {
                if (ui->share_count == 0) {
                    app_open_text(app, TEXT_SMB_AUTH_USER, 0, "共享用户名",
                                  ui->user, "输入这台服务器的账号，留空取消");
                } else if (ui->focus >= 0 && ui->focus < ui->share_count) {
                    files_share_mount(app, ui->hosts[ui->host_sel].addr,
                                      ui->shares[ui->focus]);
                }
            }
            return;
        }
        int local_n = ui->local_count;
        if (ui->focus < local_n) {
            files_share_open_settings(app, ui->focus);
        } else if (ui->focus == local_n) {
            files_share_new(app);
        } else {
            int idx = ui->focus - local_n - 1;
            if (idx >= 0 && idx < app->share_count)
                files_smb_connect_uri(app, app->shares[idx].uri,
                                      app->shares[idx].user);
        }
        return;
    }
    if (action == IN_CANCEL || action == IN_B) {
        if (app->files_panel == FILES_PANEL_CONNECT && ui->view == 1) {
            ui->view = 0;
            ui->focus = 2 + ui->host_sel;
            app_rebuild(app);
            audio_play(SND_BACK);
            return;
        }
        app->files_panel = FILES_PANEL_LIST;
        app->smb_ui.view = 0;
        app_rebuild(app);
        audio_play(SND_BACK);
        return;
    }
}

static void files_share_panel(App *app, int x, int w)
{
    UiLayout *L = &app->layout;
    SmbUi *ui = &app->smb_ui;
    int top = L->content_y;
    int bottom = L->footer_y - 8;
    int title_h = ui_text_height(FONT_LG);
    int small_h = ui_text_height(FONT_XS);
    bool connect = app->files_panel == FILES_PANEL_CONNECT;

    ui_label(app->ui.root, connect ? "连接共享" : "共享管理", FONT_LG, PAL.text,
             x, top, w, title_h);
    char sub[256];
    if (connect)
        snprintf(sub, sizeof(sub), "%s", ui->scan_msg[0] ? ui->scan_msg
                 : "扫描局域网，或手动输入服务器地址");
    else
        snprintf(sub, sizeof(sub), "%s", ui->service_msg[0] ? ui->service_msg
                 : "本机共享与已连接的共享");
    lv_obj_t *sub_label = ui_label(app->ui.root, sub, FONT_XS, PAL.text3,
                                   x, top + title_h, w, small_h);
    lv_label_set_long_mode(sub_label, LV_LABEL_LONG_DOT);

    int body_y = top + title_h + small_h + 10;
    if (connect && ui->view == 0) {
        int bh = ui_clamp(L->h * 10 / 100, 36, 54);
        int gap = 10;
        int bw = (w - gap) / 2;
        static const char *names[2] = { "扫描局域网", "手动输入" };
        static const char *icons[2] = { LV_SYMBOL_REFRESH, LV_SYMBOL_EDIT };
        int fh = ui_text_height(FONT_SM);
        for (int i = 0; i < 2; i++) {
            bool focus = ui->focus == i;
            lv_obj_t *b = ui_rect(app->ui.root, x + i * (bw + gap), body_y,
                                  bw, bh, 12, focus ? PAL.accent : PAL.card);
            lv_obj_set_style_bg_opa(b, focus ? 60 : 45, 0);
            if (focus) ui_set_focus_ring(b, true, PAL.accent);
            char txt[96];
            snprintf(txt, sizeof(txt), "%s  %s", icons[i], names[i]);
            ui_label_center(b, txt, FONT_SM, focus ? PAL.text : PAL.text2,
                            0, (bh - fh) / 2, bw, fh);
            share_panel_clickable(app, b, i);
        }
        body_y += bh + 10;
    }

    int list_h = bottom - body_y;
    if (list_h < 50) list_h = 50;
    lv_obj_t *list = lv_obj_create(app->ui.root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, ui_scale_x(x), ui_scale_y(body_y));
    lv_obj_set_size(list, ui_scale_x(w), ui_scale_y(list_h));
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    ui_scrollbar_style(list);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    int row_h = ui_clamp(L->h * 8 / 100, 34, 56);
    int gap_row = 4;
    int y = 0;
    lv_obj_t *focus_obj = NULL;
    int name_h = ui_text_height(FONT_SM);
    int hint_h = ui_text_height(FONT_XS);

    if (connect) {
        if (ui->view == 1) {
            for (int i = 0; i < ui->share_count; i++) {
                bool focus = ui->focus == i;
                lv_obj_t *r = ui_rect(list, 0, y, w, row_h, 8,
                                      focus ? PAL.accent : PAL.card);
                lv_obj_set_style_bg_opa(r, focus ? 55 : 40, 0);
                if (focus) ui_set_focus_ring(r, true, PAL.accent);
                ui_label(r, LV_SYMBOL_DIRECTORY, FONT_MD, PAL.accent2, 12,
                         (row_h - name_h) / 2, 22, name_h);
                lv_obj_t *nl = ui_label(r, ui->shares[i], FONT_SM, PAL.text,
                                        42, (row_h - name_h) / 2,
                                        w - 54, name_h);
                lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
                share_panel_clickable(app, r, i);
                if (focus) focus_obj = r;
                y += row_h + gap_row;
            }
            if (ui->share_count == 0) {
                const char *msg = ui->scan_running ? "正在读取共享..." :
                                  (ui->auth_required ?
                                   "需要登录才能查看共享，按 A 输入账号密码" :
                                   "没有找到可用的共享");
                ui_label(list, msg, FONT_SM, PAL.text3, 6, 8, w - 12, name_h);
            }
        } else {
            for (int i = 0; i < ui->host_count; i++) {
                int idx = 2 + i;
                bool focus = ui->focus == idx;
                lv_obj_t *r = ui_rect(list, 0, y, w, row_h, 8,
                                      focus ? PAL.accent : PAL.card);
                lv_obj_set_style_bg_opa(r, focus ? 55 : 40, 0);
                if (focus) ui_set_focus_ring(r, true, PAL.accent);
                ui_label(r, LV_SYMBOL_DRIVE, FONT_MD, PAL.accent2, 12,
                         (row_h - name_h) / 2, 22, name_h);
                int name_w = w * 34 / 100;
                int addr_x = 42 + name_w + 12;
                lv_obj_t *nl = ui_label(r, ui->hosts[i].name, FONT_SM, PAL.text,
                                        42, (row_h - name_h) / 2,
                                        name_w, name_h);
                lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
                lv_obj_t *al = ui_label(r, ui->hosts[i].addr, FONT_XS, PAL.text3,
                                        addr_x, (row_h - hint_h) / 2,
                                        w * 70 / 100 - addr_x, hint_h);
                lv_label_set_long_mode(al, LV_LABEL_LONG_DOT);
                char st[48];
                if (ui->hosts[i].share_count < 0)
                    snprintf(st, sizeof(st), "查看共享");
                else
                    snprintf(st, sizeof(st), "%d 个共享", ui->hosts[i].share_count);
                lv_obj_t *sl = ui_label(r, st, FONT_XS, PAL.text3,
                                        w * 72 / 100, (row_h - hint_h) / 2,
                                        w * 26 / 100, hint_h);
                lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_RIGHT, 0);
                share_panel_clickable(app, r, idx);
                if (focus) focus_obj = r;
                y += row_h + gap_row;
            }
            if (ui->host_count == 0)
                ui_label(list, ui->scan_running ? "正在扫描局域网..." :
                         "还没有发现服务器，点「扫描局域网」",
                         FONT_SM, PAL.text3, 6, 8, w - 12, name_h);
        }
    } else {
        ui_label(list, "本机共享", FONT_XS, PAL.text3, 6, 2, w - 12, hint_h);
        y += hint_h + 4;
        for (int i = 0; i < ui->local_count; i++) {
            bool focus = ui->focus == i;
            lv_obj_t *r = ui_rect(list, 0, y, w, row_h, 8,
                                  focus ? PAL.accent : PAL.card);
            lv_obj_set_style_bg_opa(r, focus ? 55 : 40, 0);
            if (focus) ui_set_focus_ring(r, true, PAL.accent);
            SmbLocalShare *sh = &ui->local[i];
            lv_obj_t *nl = ui_label(r, sh->name, FONT_SM, PAL.text, 12,
                                    (row_h - name_h) / 2, w * 32 / 100, name_h);
            lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
            lv_obj_t *pl = ui_label(r, sh->path, FONT_XS, PAL.text3,
                                    w * 34 / 100, (row_h - hint_h) / 2,
                                    w * 42 / 100, hint_h);
            lv_label_set_long_mode(pl, LV_LABEL_LONG_DOT);
            lv_obj_t *al = ui_label(r, sh->writable ? "读写" : "只读",
                                    FONT_XS, sh->writable ? PAL.warning : PAL.success,
                                    w * 78 / 100, (row_h - hint_h) / 2,
                                    w * 20 / 100, hint_h);
            lv_obj_set_style_text_align(al, LV_TEXT_ALIGN_RIGHT, 0);
            share_panel_clickable(app, r, i);
            if (focus) focus_obj = r;
            y += row_h + gap_row;
        }
        {
            int idx = ui->local_count;
            bool focus = ui->focus == idx;
            lv_obj_t *r = ui_rect(list, 0, y, w, row_h, 8,
                                  focus ? PAL.accent : PAL.card);
            lv_obj_set_style_bg_opa(r, focus ? 55 : 40, 0);
            if (focus) ui_set_focus_ring(r, true, PAL.accent);
            ui_label(r, LV_SYMBOL_PLUS, FONT_MD, PAL.accent2, 12,
                     (row_h - name_h) / 2, 22, name_h);
            ui_label(r, "新建共享", FONT_SM, PAL.text, 42,
                     (row_h - name_h) / 2, w - 54, name_h);
            share_panel_clickable(app, r, idx);
            if (focus) focus_obj = r;
            y += row_h + gap_row + 8;
        }
        ui_label(list, "已连接的共享", FONT_XS, PAL.text3, 6, y, w - 12, hint_h);
        y += hint_h + 4;
        for (int j = 0; j < app->share_count; j++) {
            int idx = ui->local_count + 1 + j;
            bool focus = ui->focus == idx;
            lv_obj_t *r = ui_rect(list, 0, y, w, row_h, 8,
                                  focus ? PAL.accent : PAL.card);
            lv_obj_set_style_bg_opa(r, focus ? 55 : 40, 0);
            if (focus) ui_set_focus_ring(r, true, PAL.accent);
            ui_label(r, LV_SYMBOL_WIFI, FONT_MD, PAL.text2, 12,
                     (row_h - name_h) / 2, 22, name_h);
            lv_obj_t *nl = ui_label(r, app->shares[j].label, FONT_SM, PAL.text,
                                    42, (row_h - name_h) / 2, w * 46 / 100, name_h);
            lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
            lv_obj_t *ul = ui_label(r, app->shares[j].uri, FONT_XS, PAL.text3,
                                    w * 50 / 100, (row_h - hint_h) / 2,
                                    w * 48 / 100, hint_h);
            lv_label_set_long_mode(ul, LV_LABEL_LONG_DOT);
            share_panel_clickable(app, r, idx);
            if (focus) focus_obj = r;
            y += row_h + gap_row;
        }
        if (app->share_count == 0)
            ui_label(list, "还没有收藏的共享", FONT_XS, PAL.text3, 6, y,
                     w - 12, hint_h);
    }
    if (focus_obj) ui_scroll_into_view(focus_obj, false);

    if (connect)
        ui_footer_pill(app, "连接共享", ui->host_count,
                       ui->view == 1 ? "A 连接" : "A 选择", "B 返回",
                       ui->view == 1 ? "X 刷新" : "X 扫描", "Y 手动");
    else
        ui_footer_pill(app, "共享管理", ui->local_count + app->share_count,
                       "A 打开", "B 返回", "X 服务", "Y 新建/移除");
}

void ui_build_files(App *app)
{
    if (!app->files_loaded) app_files_enter(app);
    UiLayout *L = &app->layout;
    ui_screen_background(app, NULL, PAL.accent);

    bool narrow = L->w < 400;
    bool show_side = !narrow || app->files_sidebar_open;
    bool overlay = narrow && app->files_sidebar_open;
    int gap = ui_clamp(L->w * 12 / 1000, 6, 24);
    /* Same rail position/height as settings, slightly narrower. */
    int side_w = overlay ? ui_clamp(L->rail_w, 170, 230) :
                           ui_clamp(L->w * 19 / 100, 88, 240);
    int content_x = L->pad;
    int content_w = L->w - L->pad * 2;
    if (show_side && !overlay) {
        content_x = L->rail_x + side_w + gap;
        content_w = L->w - content_x - L->pad;
    }
    if (content_w < 80) content_w = 80;

    if (app->files_panel != FILES_PANEL_LIST) {
        files_share_panel(app, content_x, content_w);
        if (show_side) {
            int side_x = L->rail_x;
            int side_y = overlay ? L->content_y + 30 : L->rail_y;
            int side_h = overlay ? L->footer_y - side_y - 4 : L->rail_h;
            if (side_h < 60) side_h = 60;
            files_build_sidebar(app, side_x, side_y, side_w, side_h, overlay);
        }
        ui_update_files(app);
        return;
    }

    int toolbar_y = L->content_y;
    int content_right = content_x + content_w;
    bool show_keys = content_w >= 310;
    int group_w = show_keys ?
        files_keycap_width(backend_trigger_left_label(&app->backend)) + 2 + 24 : 24;
    int btns = narrow ? 3 : 2;
    int btns_w = btns * group_w + (btns - 1) * 4;
    int status_w = content_w >= 460 ? 150 : 90;
    int status_x = content_right - btns_w - status_w - 4;
    int path_x = content_x + 28;
    int path_w = status_x - path_x - 6;
    if (path_w < 40) path_w = 40;

    files_tool_button(app, app->ui.root, content_x, toolbar_y, LV_SYMBOL_LEFT,
                      NULL, files_tool_up_cb);
    int toolbar_text_h = ui_text_height(FONT_XS);
    lv_obj_t *path = ui_label(app->ui.root, app->files_cwd, FONT_XS, PAL.text2,
                              path_x, toolbar_y + 4, path_w, toolbar_text_h);
    lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
    app->ui.files_path_label = path;

    app->ui.files_status_label = ui_label(app->ui.root, "", FONT_XS, PAL.text3,
                                          status_x, toolbar_y + 4, status_w,
                                          toolbar_text_h);
    lv_obj_set_style_text_align(app->ui.files_status_label, LV_TEXT_ALIGN_RIGHT, 0);
    if (app->files_pick != FILES_PICK_NONE) {
        lv_obj_add_flag(app->ui.files_status_label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(app->ui.files_status_label, files_pick_status_cb,
                            LV_EVENT_CLICKED, app);
    }

    int bx = content_right - group_w;
    app->ui.files_refresh_btn = files_tool_button(app, app->ui.root, bx, toolbar_y,
                      LV_SYMBOL_REFRESH,
                      show_keys ? backend_trigger_right_label(&app->backend) : NULL,
                      files_tool_refresh_cb);
    bx -= group_w + 4;
    app->ui.files_eye_btn = files_tool_button(app, app->ui.root, bx, toolbar_y,
                      app->files_show_hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE,
                      show_keys ? backend_trigger_left_label(&app->backend) : NULL,
                      files_tool_hidden_cb);
    if (narrow) {
        bx -= group_w + 4;
        files_tool_button(app, app->ui.root, bx, toolbar_y, LV_SYMBOL_LIST,
                          NULL, files_tool_side_cb);
    }

    int header_y = toolbar_y + 27;
    int list_y = header_y + 16;
    int list_h = L->footer_y - list_y - 4;
    if (list_h < 40) list_h = 40;
    int row_h = ui_clamp(L->h * 9 / 100, 32, 64);
    int gap_row = 3;
    int row_w = content_w;
    FileCols cols = files_columns(row_w, row_h);

    if (row_w >= 360) {
        ui_label(app->ui.root, "名称", FONT_XS, PAL.text3,
                 content_x + cols.name_x, header_y, cols.name_w,
                 ui_text_height(FONT_XS));
        if (cols.date_w)
            ui_label(app->ui.root, "修改日期", FONT_XS, PAL.text3,
                     content_x + cols.date_x, header_y, cols.date_w,
                     ui_text_height(FONT_XS));
        if (cols.size_w) {
            lv_obj_t *sh = ui_label(app->ui.root, "大小", FONT_XS, PAL.text3,
                                    content_x + cols.size_x, header_y, cols.size_w,
                                    ui_text_height(FONT_XS));
            lv_obj_set_style_text_align(sh, LV_TEXT_ALIGN_RIGHT, 0);
        }
        if (cols.kind_w)
            ui_label(app->ui.root, "种类", FONT_XS, PAL.text3,
                     content_x + cols.kind_x, header_y, cols.kind_w,
                     ui_text_height(FONT_XS));
    }

    lv_obj_t *list = lv_obj_create(app->ui.root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, ui_scale_x(content_x), ui_scale_y(list_y));
    lv_obj_set_size(list, ui_scale_x(row_w), ui_scale_y(list_h));
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    ui_scrollbar_style(list);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    app->ui.files_row_count = 0;
    for (int i = 0; i < app->files_list.count && i < 128; i++) {
        FsEntry *it = &app->files_list.items[i];
        lv_obj_t *r = ui_rect(list, 0, i * (row_h + gap_row), row_w, row_h, 8, PAL.card);
        lv_obj_set_style_bg_opa(r, 45, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        ui_label_center(r, it->is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                        row_h >= 40 ? FONT_MD : FONT_SM,
                        it->is_dir ? PAL.accent2 : PAL.text3,
                        8, (row_h - 20) / 2, 22, 20);
        lv_font_t *name_font = row_h >= 40 ? FONT_MD : FONT_SM;
        int name_h = ui_text_height(name_font);
        lv_obj_t *name = ui_label(r, it->name, name_font,
                                  PAL.text, cols.name_x, (row_h - name_h) / 2,
                                  cols.name_w, name_h);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        if (cols.date_w) {
            char tm[40];
            fs_format_time(it->mtime, tm, sizeof(tm));
            int date_h = ui_text_height(FONT_XS);
            ui_label(r, tm, FONT_XS, PAL.text3, cols.date_x,
                     (row_h - date_h) / 2, cols.date_w, date_h);
        }
        char sz[32];
        fs_format_size(it->size, sz, sizeof(sz));
        if (cols.size_w) {
            int size_h = ui_text_height(FONT_XS);
            lv_obj_t *sl = ui_label(r, it->is_dir ? "—" : sz, FONT_XS, PAL.text3,
                                    cols.size_x, (row_h - size_h) / 2,
                                    cols.size_w, size_h);
            lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_RIGHT, 0);
        }
        if (cols.kind_w) {
            int kind_h = ui_text_height(FONT_XS);
            ui_label(r, files_kind_label(it), FONT_XS, PAL.text3,
                     cols.kind_x, (row_h - kind_h) / 2, cols.kind_w, kind_h);
        }
        lv_obj_t *star = lv_image_create(r);
        lv_obj_remove_flag(star, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_style_all(star);
        lv_obj_set_size(star, ui_scale_x(cols.star_w - 4), ui_scale_y(cols.star_w - 4));
        lv_obj_set_pos(star, ui_scale_x(cols.star_x), ui_scale_y((row_h - cols.star_w + 4) / 2));
        lv_image_set_src(star, &taiji_star_icon);
        lv_obj_set_style_image_recolor(star, lv_color_hex(PAL.warning), 0);
        lv_obj_set_style_image_recolor_opa(star, LV_OPA_COVER, 0);
        lv_obj_add_flag(star, LV_OBJ_FLAG_HIDDEN);
        app->ui.files_marks[i] = star;

        FileRowCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->index = i;
        ctx->pressed = false;
        ctx->long_fired = false;
        ctx->press_ms = 0;
        lv_obj_add_event_cb(r, file_row_pressed, LV_EVENT_PRESSED, ctx);
        lv_obj_add_event_cb(r, file_row_released, LV_EVENT_RELEASED, ctx);
        lv_obj_add_event_cb(r, file_row_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(r, file_row_free, LV_EVENT_DELETE, ctx);
        app->ui.files_rows[i] = r;
        app->ui.files_row_count++;
    }
    if (app->files_list.count == 0) {
        int empty_h = ui_text_height(FONT_MD);
        ui_label_center(list, "这个文件夹是空的", FONT_MD, PAL.text3,
                        0, list_h / 2 - empty_h / 2, row_w, empty_h);
    }

    if (show_side) {
        int side_x = L->rail_x;
        int side_y = overlay ? L->content_y + 30 : L->rail_y;
        int side_h = overlay ? L->footer_y - side_y - 4 : L->rail_h;
        if (side_h < 60) side_h = 60;
        files_build_sidebar(app, side_x, side_y, side_w, side_h, overlay);
    }

    if (app->files_mtp_panel) {
        files_build_mtp_panel(app, content_x, L->content_y, content_w,
                              L->footer_y - L->content_y - 4);
        ui_footer_pill(app, "MTP", 2, "A 切换", "B 返回", "", "");
        ui_update_files(app);
        return;
    }

    if (app->files_on_sidebar) {
        ui_footer_pill(app, "文件", app->files_list.count,
                       "A 打开/挂载", "B 返回", "X 操作", "Y 刷新");
    } else if (app->files_pick == FILES_PICK_DIR) {
        ui_footer_pill(app, "选择目录", app->files_list.count,
                       "A 进入", "B 上级", "X 操作", "Y 选中");
    } else if (app->files_pick == FILES_PICK_FILE) {
        ui_footer_pill(app, "选择文件", app->files_list.count,
                       "A 选择", "B 上级", "X 操作", "Y 选中");
    } else {
        ui_footer_pill(app, "文件", app->files_list.count,
                       "A 打开", "B 返回", "X 操作", "Y 选中");
    }
    ui_update_files(app);
}

void ui_update_files(App *app)
{
    for (int i = 0; i < app->ui.files_row_count; i++) {
        lv_obj_t *r = app->ui.files_rows[i];
        if (!r) continue;
        bool sel = !app->files_on_sidebar && i == app->files_focus;
        bool marked = app->files_marked && i < app->files_list.count && app->files_marked[i];
        lv_obj_set_style_bg_color(r, lv_color_hex(sel ? PAL.accent : PAL.card), 0);
        lv_obj_set_style_bg_opa(r, sel ? 55 : 45, 0);
        ui_set_focus_ring(r, sel, ui_focus_accent(i));
        lv_obj_t *star = app->ui.files_marks[i];
        if (star) {
            if (marked) lv_obj_clear_flag(star, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(star, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < app->files_loc_count && i < 48; i++) {
        lv_obj_t *row = app->ui.files_side_rows[i];
        if (!row) continue;
        bool here = app->files_locs[i].path[0] &&
                    strcmp(app->files_locs[i].path, app->files_cwd) == 0;
        bool focused = app->files_on_sidebar && i == app->files_side_focus;
        lv_obj_set_style_bg_color(row, lv_color_hex(here ? PAL.accent : PAL.card), 0);
        lv_obj_set_style_bg_opa(row, here ? 45 : 0, 0);
        ui_set_focus_ring(row, focused, ui_focus_accent(1000 + i));
    }
    if (app->ui.files_status_label) {
        char buf[160];
        if (app->files_message[0])
            snprintf(buf, sizeof(buf), "%s", app->files_message);
        else if (app->files_pick == FILES_PICK_DIR)
            ui_button_hint(app, buf, sizeof(buf), "{START} 选此目录");
        else if (app->files_pick == FILES_PICK_FILE)
            ui_button_hint(app, buf, sizeof(buf), "{A} / {START} 选择");
        else if (app->files_marked_count > 0)
            snprintf(buf, sizeof(buf), "%d 项 / 已选 %d", app->files_list.count,
                     app->files_marked_count);
        else
            snprintf(buf, sizeof(buf), "%d 项", app->files_list.count);
        lv_label_set_text(app->ui.files_status_label, buf);
        lv_obj_set_style_text_color(app->ui.files_status_label,
            lv_color_hex(app->files_message[0] ? PAL.warning :
                         (app->files_pick != FILES_PICK_NONE ? PAL.accent2 : PAL.text3)), 0);
    }
    if (!app->files_on_sidebar &&
        app->files_focus >= 0 && app->files_focus < app->ui.files_row_count &&
        app->ui.files_rows[app->files_focus])
        ui_scroll_into_view(app->ui.files_rows[app->files_focus], true);
    if (app->files_side_focus >= 0 &&
        app->files_side_focus < app->ui.files_side_row_count &&
        app->ui.files_side_rows[app->files_side_focus])
        ui_scroll_into_view(app->ui.files_side_rows[app->files_side_focus], true);
}
