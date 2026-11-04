// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "smb.h"

#include <stdio.h>
#include <string.h>

#ifdef __linux__
#include <gio/gio.h>

typedef struct {
    GMainLoop *loop;
    bool done;
    bool ok;
    GError *error;
    const char *user;
    const char *password;
} SmbWait;

static void ask_password_cb(GMountOperation *op, const gchar *message,
                            const gchar *default_user, const gchar *default_domain,
                            GAskPasswordFlags flags, gpointer data)
{
    (void)message;
    (void)default_user;
    (void)default_domain;
    SmbWait *w = data;
    if (flags & G_ASK_PASSWORD_NEED_USERNAME) {
        if (w->user && *w->user)
            g_mount_operation_set_username(op, w->user);
    }
    if (flags & G_ASK_PASSWORD_NEED_PASSWORD) {
        if (w->password)
            g_mount_operation_set_password(op, w->password);
    }
    if (flags & G_ASK_PASSWORD_NEED_DOMAIN)
        g_mount_operation_set_domain(op, "WORKGROUP");
    g_mount_operation_set_password_save(op, G_PASSWORD_SAVE_NEVER);
    g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
}

static void ask_question_cb(GMountOperation *op, const gchar *message,
                            gpointer data)
{
    (void)message;
    (void)data;
    g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
}

static void mount_done_cb(GObject *source, GAsyncResult *res, gpointer data)
{
    SmbWait *w = data;
    GError *error = NULL;
    if (!g_file_mount_enclosing_volume_finish(G_FILE(source), res, &error)) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
            w->error = error;
            w->ok = false;
            w->done = true;
            g_main_loop_quit(w->loop);
            return;
        }
        g_clear_error(&error);
    }
    w->ok = true;
    w->done = true;
    g_main_loop_quit(w->loop);
}

bool smb_mount_uri(const char *uri, const char *user, const char *password,
                   char *out_path, size_t out_size,
                   char *err, size_t err_size)
{
    if (out_path && out_size) out_path[0] = '\0';
    if (err && err_size) err[0] = '\0';
    if (!uri || !*uri) {
        if (err) snprintf(err, err_size, "共享地址为空");
        return false;
    }

    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GMainLoop *loop = g_main_loop_new(ctx, FALSE);
    SmbWait w = { .loop = loop, .user = user, .password = password };
    bool ok = false;

    GFile *file = g_file_new_for_uri(uri);
    GError *error = NULL;
    GMount *existing = g_file_find_enclosing_mount(file, NULL, NULL);
    if (!existing) {
        GMountOperation *op = g_mount_operation_new();
        g_signal_connect(op, "ask-password", G_CALLBACK(ask_password_cb), &w);
        g_signal_connect(op, "ask-question", G_CALLBACK(ask_question_cb), &w);
        if (!user || !*user) g_mount_operation_set_anonymous(op, TRUE);
        g_file_mount_enclosing_volume(file, G_MOUNT_MOUNT_NONE, op, NULL,
                                      mount_done_cb, &w);
        g_main_loop_run(loop);
        g_object_unref(op);
        if (!w.ok) {
            if (w.error) {
                if (err) snprintf(err, err_size, "%s", w.error->message);
                g_error_free(w.error);
            } else if (err) {
                snprintf(err, err_size, "无法连接共享文件夹");
            }
            goto out;
        }
    } else {
        g_object_unref(existing);
    }

    GMount *mount = g_file_find_enclosing_mount(file, NULL, &error);
    if (!mount) {
        if (err) snprintf(err, err_size, "%s",
                          error ? error->message : "共享未挂载");
        g_clear_error(&error);
        goto out;
    }
    GFile *root = g_mount_get_root(mount);
    char *path = root ? g_file_get_path(root) : NULL;
    if (!path || !*path) {
        if (err) snprintf(err, err_size, "共享已挂载，但无法获取本地路径");
        g_free(path);
        if (root) g_object_unref(root);
        g_object_unref(mount);
        goto out;
    }
    if (out_path) snprintf(out_path, out_size, "%s", path);
    g_free(path);
    g_object_unref(root);
    g_object_unref(mount);
    ok = true;

out:
    g_object_unref(file);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    return ok;
}

#else

bool smb_mount_uri(const char *uri, const char *user, const char *password,
                   char *out_path, size_t out_size,
                   char *err, size_t err_size)
{
    (void)uri; (void)user; (void)password;
    if (out_path && out_size) out_path[0] = '\0';
    if (err && err_size) snprintf(err, err_size, "当前平台不支持 SMB 挂载");
    return false;
}

#endif
