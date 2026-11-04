// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <gio/gio.h>
#include <glib.h>
#include <glib-unix.h>

#include <limits.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const char *PORTAL_XML =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.FileChooser'>"
    "    <method name='OpenFile'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SaveFile'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SaveFiles'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static GMutex g_lock;

typedef struct {
    GDBusMethodInvocation *inv;
    char mode[16];
    bool multiple;
    char start[PATH_MAX];
    char filename[512];
    char title[256];
} Job;

static const char *files_bin(void)
{
    const char *env = getenv("TAIJI_FILES_BIN");
    return env && *env ? env : "/usr/libexec/taiji/taiji-files";
}

static void options_parse(GVariant *opts, Job *job)
{
    gboolean b = FALSE;
    if (g_variant_lookup(opts, "multiple", "b", &b)) job->multiple = b;

    gboolean directory = FALSE;
    if (g_variant_lookup(opts, "directory", "b", &directory) && directory)
        snprintf(job->mode, sizeof(job->mode), "folder");

    const gchar *s = NULL;
    if (g_variant_lookup(opts, "filename", "&s", &s) && s)
        snprintf(job->filename, sizeof(job->filename), "%s", s);
    else if (g_variant_lookup(opts, "current_name", "&s", &s) && s)
        snprintf(job->filename, sizeof(job->filename), "%s", s);

    GVariant *folder = g_variant_lookup_value(opts, "current_folder",
                                              G_VARIANT_TYPE_BYTESTRING);
    if (folder) {
        const char *p = g_variant_get_bytestring(folder);
        if (p && *p) snprintf(job->start, sizeof(job->start), "%s", p);
        g_variant_unref(folder);
    }
}

static void return_result(GDBusMethodInvocation *inv, guint response,
                          GPtrArray *uris)
{
    GVariantBuilder rb;
    g_variant_builder_init(&rb, G_VARIANT_TYPE_VARDICT);
    if (uris && uris->len > 0) {
        g_variant_builder_add(&rb, "{sv}", "uris",
            g_variant_new_strv((const gchar *const *)uris->pdata, (gssize)uris->len));
    }
    g_dbus_method_invocation_return_value(
        inv, g_variant_new("(u@a{sv})", response, g_variant_builder_end(&rb)));
}

static gpointer job_thread(gpointer data)
{
    Job *job = data;
    g_mutex_lock(&g_lock);

    char tmpl[] = "/tmp/taiji-files-XXXXXX";
    int fd = g_mkstemp(tmpl);
    if (fd >= 0) close(fd);
    unlink(tmpl);

    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, (gpointer)files_bin());
    g_ptr_array_add(args, "--mode");
    g_ptr_array_add(args, job->mode);
    g_ptr_array_add(args, "--output");
    g_ptr_array_add(args, tmpl);
    g_ptr_array_add(args, "--title");
    g_ptr_array_add(args, job->title[0] ? job->title : "选择文件");
    if (job->start[0] && g_file_test(job->start, G_FILE_TEST_IS_DIR)) {
        g_ptr_array_add(args, "--start");
        g_ptr_array_add(args, job->start);
    }
    if (job->multiple) g_ptr_array_add(args, "--multiple");
    if (job->filename[0]) {
        g_ptr_array_add(args, "--filename");
        g_ptr_array_add(args, job->filename);
    }
    g_ptr_array_add(args, NULL);

    gchar *out = NULL, *err = NULL;
    gint status = 0;
    GError *error = NULL;
    gboolean ok = g_spawn_sync(NULL, (gchar **)args->pdata, NULL,
                               G_SPAWN_SEARCH_PATH, NULL, NULL,
                               &out, &err, &status, &error);
    GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
    if (ok && status == 0) {
        gchar *content = NULL;
        gsize len = 0;
        if (g_file_get_contents(tmpl, &content, &len, NULL)) {
            char *save = NULL;
            for (char *line = strtok_r(content, "\n", &save); line;
                 line = strtok_r(NULL, "\n", &save)) {
                g_strstrip(line);
                if (!*line) continue;
                gchar *uri = g_filename_to_uri(line, NULL, NULL);
                if (uri) g_ptr_array_add(uris, uri);
            }
            g_free(content);
        }
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (!ok || code != 0 || uris->len == 0) {
        guint resp = (!ok) ? 2 : 1;
        return_result(job->inv, resp, NULL);
        if (!ok && error) fprintf(stderr, "[taiji-files-portal] spawn: %s\n", error->message);
        if (err && *err) fprintf(stderr, "[taiji-files-portal] picker: %s\n", err);
    } else {
        return_result(job->inv, 0, uris);
    }
    g_clear_error(&error);
    g_free(out);
    g_free(err);
    g_ptr_array_free(args, TRUE);
    g_ptr_array_free(uris, TRUE);
    unlink(tmpl);
    g_object_unref(job->inv);
    free(job);
    g_mutex_unlock(&g_lock);
    return NULL;
}

static void handle_method(GDBusConnection *conn, const gchar *sender,
                          const gchar *path, const gchar *iface,
                          const gchar *method, GVariant *params,
                          GDBusMethodInvocation *inv, gpointer user_data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)user_data;
    const char *handle = NULL, *app_id = NULL, *parent = NULL, *title = NULL;
    GVariant *opts = NULL;
    g_variant_get(params, "(&o&s&s&s@a{sv})", &handle, &app_id, &parent,
                  &title, &opts);
    Job *job = calloc(1, sizeof(*job));
    if (!job) {
        g_variant_unref(opts);
        return_result(inv, 2, NULL);
        return;
    }
    job->inv = g_object_ref(inv);
    if (strcmp(method, "SaveFile") == 0 || strcmp(method, "SaveFiles") == 0)
        snprintf(job->mode, sizeof(job->mode), "save");
    else
        snprintf(job->mode, sizeof(job->mode), "open");
    snprintf(job->title, sizeof(job->title), "%s", title ? title : "");
    options_parse(opts, job);
    g_variant_unref(opts);
    GThread *th = g_thread_new("picker", job_thread, job);
    if (th) g_thread_unref(th);
    (void)handle; (void)app_id; (void)parent;
}

static const GDBusInterfaceVTable VTABLE = {
    .method_call = handle_method,
    .get_property = NULL,
    .set_property = NULL,
};

static void on_bus(GDBusConnection *conn, const gchar *name, gpointer user)
{
    (void)name; (void)user;
    GError *error = NULL;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(PORTAL_XML, &error);
    if (!info) {
        fprintf(stderr, "[taiji-files-portal] xml: %s\n", error->message);
        g_clear_error(&error);
        return;
    }
    guint id = g_dbus_connection_register_object(
        conn, "/org/freedesktop/portal/desktop", info->interfaces[0],
        &VTABLE, NULL, NULL, &error);
    if (id == 0) {
        fprintf(stderr, "[taiji-files-portal] register: %s\n", error->message);
        g_clear_error(&error);
    } else {
        fprintf(stderr, "[taiji-files-portal] ready\n");
    }
    g_dbus_node_info_unref(info);
}

static gboolean on_signal(gpointer user)
{
    GMainLoop *loop = user;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(void)
{
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_signal, loop);
    g_unix_signal_add(SIGINT, on_signal, loop);
    g_bus_own_name(G_BUS_TYPE_SESSION,
                   "org.freedesktop.impl.portal.desktop.taiji",
                   G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
                   G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                   on_bus, NULL, NULL, NULL, NULL);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    return 0;
}
