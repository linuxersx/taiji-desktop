// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "smb_share.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static bool have_cmd(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/bin:/usr/sbin:/sbin";
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

static const char *smb_helper_path(void)
{
    const char *env = getenv("TAIJI_SMB_HELPER");
    return env && *env ? env : "/usr/libexec/taiji/taiji-smb-helper";
}

static bool helper_run(const char *args, char *out, size_t out_size)
{
    const char *helper = smb_helper_path();
    if (access(helper, X_OK) != 0) {
        if (out && out_size) out[0] = '\0';
        return false;
    }
    char cmd[2048];
    if (geteuid() == 0)
        snprintf(cmd, sizeof(cmd), "timeout 10 %s %s 2>&1", helper, args);
    else
        snprintf(cmd, sizeof(cmd), "timeout 12 pkexec %s %s 2>&1", helper, args);
    FILE *f = popen(cmd, "r");
    if (!f) {
        if (out && out_size) out[0] = '\0';
        return false;
    }
    size_t len = 0;
    if (out && out_size) {
        size_t n;
        while ((n = fread(out + len, 1, out_size - len - 1, f)) > 0) {
            len += n;
            if (len + 1 >= out_size) break;
        }
        out[len] = '\0';
    }
    int rc = pclose(f);
    return rc == 0;
}

static void set_err(char *err, size_t n, const char *msg)
{
    if (err && n) snprintf(err, n, "%s", msg ? msg : "");
}

static void quote_arg(char *dst, size_t dst_size, const char *src)
{
    char *q = shell_quote(src ? src : "");
    snprintf(dst, dst_size, "%s", q);
    free(q);
}

void smb_local_refresh(App *app)
{
    SmbUi *ui = &app->smb_ui;
    ui->local_count = 0;
    char out[8192];
    bool ok = helper_run("list", out, sizeof(out));
    ui->helper_ok = ok || access(smb_helper_path(), X_OK) == 0;
    if (!ok || !out[0]) {
        if (ok) ui->helper_ok = true;
        return;
    }
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line && ui->local_count < SMB_LOCAL_MAX;
         line = strtok_r(NULL, "\n", &save)) {
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = '\0';
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t2 = '\0';
        SmbLocalShare *s = &ui->local[ui->local_count];
        snprintf(s->name, sizeof(s->name), "%s", line);
        snprintf(s->path, sizeof(s->path), "%s", t1 + 1);
        s->writable = atoi(t2 + 1) == 0;
        ui->local_count++;
    }
}

void smb_service_query(App *app)
{
    char out[128];
    if (!helper_run("status", out, sizeof(out))) {
        app->smb_ui.service_on = false;
        snprintf(app->smb_ui.service_msg, sizeof(app->smb_ui.service_msg),
                 "共享服务不可用");
        return;
    }
    int active = 0, installed = 0;
    if (sscanf(out, "%d %d", &active, &installed) != 2) {
        app->smb_ui.service_on = false;
        snprintf(app->smb_ui.service_msg, sizeof(app->smb_ui.service_msg),
                 "共享服务状态未知");
        return;
    }
    app->smb_ui.service_on = active != 0;
    if (!installed)
        snprintf(app->smb_ui.service_msg, sizeof(app->smb_ui.service_msg),
                 "未安装 samba");
    else
        snprintf(app->smb_ui.service_msg, sizeof(app->smb_ui.service_msg),
                 active ? "运行中" : "已停止");
}

bool smb_local_add(App *app, const char *name, const char *path, bool writable,
                   char *err, size_t err_size)
{
    char qn[256], qp[2200], args[2600];
    quote_arg(qn, sizeof(qn), name);
    quote_arg(qp, sizeof(qp), path);
    snprintf(args, sizeof(args), "add %s %s %d", qn, qp, writable ? 0 : 1);
    char msg[256] = "";
    if (!helper_run(args, msg, sizeof(msg))) {
        char *t = str_trim(msg);
        set_err(err, err_size, *t ? t : "无法添加共享，请检查名称和目录");
        return false;
    }
    smb_local_refresh(app);
    smb_service_query(app);
    return true;
}

bool smb_local_set(App *app, const char *name, const char *path, bool writable,
                   char *err, size_t err_size)
{
    char qn[256], qp[2200], args[2600];
    quote_arg(qn, sizeof(qn), name);
    quote_arg(qp, sizeof(qp), path);
    snprintf(args, sizeof(args), "set %s %s %d", qn, qp, writable ? 0 : 1);
    char msg[256] = "";
    if (!helper_run(args, msg, sizeof(msg))) {
        char *t = str_trim(msg);
        set_err(err, err_size, *t ? t : "无法保存共享设置");
        return false;
    }
    smb_local_refresh(app);
    return true;
}

bool smb_local_remove(App *app, const char *name, char *err, size_t err_size)
{
    char qn[256], args[320];
    quote_arg(qn, sizeof(qn), name);
    snprintf(args, sizeof(args), "remove %s", qn);
    char msg[256] = "";
    if (!helper_run(args, msg, sizeof(msg))) {
        char *t = str_trim(msg);
        set_err(err, err_size, *t ? t : "无法删除共享");
        return false;
    }
    smb_local_refresh(app);
    return true;
}

bool smb_service_set(App *app, bool on, char *err, size_t err_size)
{
    const char *op = on ? "enable" : "disable";
    if (!helper_run(op, NULL, 0)) {
        set_err(err, err_size, on ? "无法开启共享服务" : "无法停止共享服务");
        return false;
    }
    smb_service_query(app);
    return true;
}

/* ── LAN scan ─────────────────────────────────────────────────────────────── */

static void avahi_unescape(const char *in, char *out, size_t out_size)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_size; ) {
        if (in[i] == '\\' && in[i + 1] >= '0' && in[i + 1] <= '9') {
            int v = 0, k = 0;
            while (k < 3 && in[i + 1 + k] >= '0' && in[i + 1 + k] <= '9') {
                v = v * 10 + (in[i + 1 + k] - '0');
                k++;
            }
            out[j++] = (char)(v & 0xff);
            i += 1 + (size_t)k;
            continue;
        }
        if (in[i] == '\\' && in[i + 1]) {
            out[j++] = in[i + 1];
            i += 2;
            continue;
        }
        out[j++] = in[i++];
    }
    out[j] = '\0';
}

static int mdns_lookup(char *names, char *addrs, int max)
{
    if (!have_cmd("avahi-browse")) return 0;
    FILE *f = popen("timeout 4 avahi-browse -rt -p _smb._tcp 2>/dev/null", "r");
    if (!f) return 0;
    int n = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f) && n < max) {
        if (line[0] != '=') continue;
        char *fields[12];
        int nf = 0;
        char *p = line;
        while (nf < 12) {
            fields[nf++] = p;
            char *semi = strchr(p, ';');
            if (!semi) break;
            *semi = '\0';
            p = semi + 1;
        }
        if (nf < 9) continue;
        const char *name = fields[3];
        const char *addr = fields[7];
        if (!addr || !*addr || strchr(addr, ':')) continue;
        char clean[96];
        avahi_unescape(name, clean, sizeof(clean));
        snprintf(names + (size_t)n * 96, 96, "%s", clean);
        snprintf(addrs + (size_t)n * 64, 64, "%s", addr);
        n++;
    }
    pclose(f);
    return n;
}

typedef struct {
    App *app;
} ScanCtx;

static bool scan_is_local_addr(const char *addr)
{
    if (!addr || !*addr) return true;
    if (strncmp(addr, "127.", 4) == 0) return true;
    struct ifaddrs *ifa = NULL;
    bool local = false;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *p = ifa; p && !local; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            char buf[64];
            if (!inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr,
                           buf, sizeof(buf)))
                continue;
            if (strcmp(buf, addr) == 0) local = true;
        }
        freeifaddrs(ifa);
    }
    return local;
}

static void scan_add_host(App *app, const char *addr, const char *name)
{
    SmbUi *ui = &app->smb_ui;
    if (ui->host_count >= SMB_SCAN_MAX) return;
    if (scan_is_local_addr(addr)) return;
    for (int i = 0; i < ui->host_count; i++)
        if (strcmp(ui->hosts[i].addr, addr) == 0) {
            if (name && *name && (!ui->hosts[i].name[0] ||
                                  strcmp(ui->hosts[i].name, ui->hosts[i].addr) == 0))
                snprintf(ui->hosts[i].name, sizeof(ui->hosts[i].name), "%s", name);
            return;
        }
    SmbScannedHost *h = &ui->hosts[ui->host_count++];
    memset(h, 0, sizeof(*h));
    snprintf(h->addr, sizeof(h->addr), "%s", addr);
    snprintf(h->name, sizeof(h->name), "%s", (name && *name) ? name : addr);
    h->share_count = -1;
}

static void *scan_thread(void *data)
{
    App *app = ((ScanCtx *)data)->app;
    free(data);
    SmbUi *ui = &app->smb_ui;

    char mdns_names[64 * 96], mdns_addrs[64 * 64];
    int mdns_n = mdns_lookup(mdns_names, mdns_addrs, 64);

    struct ifaddrs *ifa = NULL;
    unsigned int base = 0;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
            base = ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
            break;
        }
        freeifaddrs(ifa);
    }
    if (base == 0) {
        for (int i = 0; i < mdns_n; i++)
            scan_add_host(app, mdns_addrs + (size_t)i * 64,
                          mdns_names + (size_t)i * 96);
        ui->scan_ok = ui->host_count > 0;
        snprintf(ui->scan_msg, sizeof(ui->scan_msg),
                 ui->scan_ok ? "发现 %d 台服务器" : "没有可用的网络连接",
                 ui->host_count);
        ui->scan_done = true;
        return NULL;
    }
    /* Always probe the local /24; larger networks are limited to it. */
    unsigned int net = base & 0xffffff00u;
    struct pollfd fds[256];
    int ips[256];
    int nfds = 0;
    for (int i = 1; i <= 254 && nfds < 254; i++) {
        unsigned int ip = net | (unsigned int)i;
        if (ip == base) continue;
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(445);
        sa.sin_addr.s_addr = htonl(ip);
        connect(fd, (struct sockaddr *)&sa, sizeof(sa));
        fds[nfds].fd = fd;
        fds[nfds].events = POLLOUT;
        fds[nfds].revents = 0;
        ips[nfds] = (int)ip;
        nfds++;
        if (nfds >= 254) break;
    }
    if (nfds > 0) {
        poll(fds, (nfds_t)nfds, 350);
        for (int i = 0; i < nfds; i++) {
            int err = 0;
            socklen_t elen = sizeof(err);
            bool ok = false;
            if (fds[i].revents & POLLOUT) {
                if (getsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 &&
                    err == 0)
                    ok = true;
            }
            if (!ok) { close(fds[i].fd); continue; }
            struct in_addr a;
            a.s_addr = htonl((unsigned int)ips[i]);
            char buf[64];
            inet_ntop(AF_INET, &a, buf, sizeof(buf));
            scan_add_host(app, buf, NULL);
            close(fds[i].fd);
        }
    }
    for (int i = 0; i < mdns_n; i++)
        scan_add_host(app, mdns_addrs + (size_t)i * 64,
                      mdns_names + (size_t)i * 96);

    ui->scan_ok = ui->host_count > 0;
    if (!ui->scan_ok)
        snprintf(ui->scan_msg, sizeof(ui->scan_msg),
                 "未发现 SMB 服务器（本网段 %u.%u.%u.0/24）",
                 (net >> 24) & 0xff, (net >> 16) & 0xff, (net >> 8) & 0xff);
    else
        snprintf(ui->scan_msg, sizeof(ui->scan_msg), "发现 %d 台服务器",
                 ui->host_count);
    ui->scan_done = true;
    return NULL;
}

void smb_scan_start(App *app)
{
    SmbUi *ui = &app->smb_ui;
    if (ui->scan_running) return;
    ui->scan_running = true;
    ui->scan_done = false;
    ui->scan_ok = false;
    ui->host_count = 0;
    ui->host_sel = 0;
    ui->view = 0;
    snprintf(ui->scan_msg, sizeof(ui->scan_msg), "正在扫描局域网...");
    ScanCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    pthread_t th;
    if (pthread_create(&th, NULL, scan_thread, ctx) != 0) {
        free(ctx);
        ui->scan_running = false;
        ui->scan_done = true;
        snprintf(ui->scan_msg, sizeof(ui->scan_msg), "无法启动扫描");
        return;
    }
    pthread_detach(th);
}

/* ── Share listing on demand ──────────────────────────────────────────────── */

typedef struct {
    App *app;
    char addr[64];
    char user[128];
    char pass[256];
    int host;
} ShareCtx;

static void *share_thread(void *data)
{
    ShareCtx *ctx = data;
    App *app = ctx->app;
    SmbUi *ui = &app->smb_ui;
    char cmd[1024];
    if (ctx->user[0]) {
        char auth[512];
        snprintf(auth, sizeof(auth), "%s%%%s", ctx->user, ctx->pass);
        char *q = shell_quote(auth);
        snprintf(cmd, sizeof(cmd),
                 "timeout 8 smbclient -L %s -U %s -m SMB3 -t 5 2>&1",
                 ctx->addr, q);
        free(q);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "timeout 8 smbclient -L %s -N -m SMB3 -t 5 2>&1", ctx->addr);
    }
    FILE *f = popen(cmd, "r");
    ui->share_sel = 0;
    ui->auth_required = false;
    int count = 0;
    char all[4096];
    size_t used = 0;
    all[0] = '\0';
    if (f) {
        char line[512];
        bool in_list = false;
        while (fgets(line, sizeof(line), f)) {
            if (used + strlen(line) + 1 < sizeof(all)) {
                memcpy(all + used, line, strlen(line));
                used += strlen(line);
                all[used] = '\0';
            }
            if (strstr(line, "Sharename")) { in_list = true; continue; }
            if (!in_list) continue;
            if (strncmp(line, "---", 3) == 0) continue;
            if (line[0] == '\n' || line[0] == ' ') continue;
            char name[96] = "";
            if (sscanf(line, "%95s", name) != 1) continue;
            if (!strcasecmp(name, "IPC$")) continue;
            if (strstr(line, "Disk") == NULL) continue;
            if (count < SMB_SCAN_SHARES)
                snprintf(ui->shares[count], sizeof(ui->shares[count]), "%s", name);
            count++;
        }
        pclose(f);
    }
    if (strstr(all, "LOGON_FAILURE") || strstr(all, "ACCESS_DENIED") ||
        strstr(all, "NT_STATUS_ACCOUNT_DISABLED"))
        ui->auth_required = true;
    if (count > SMB_SCAN_SHARES) count = SMB_SCAN_SHARES;
    if (ctx->host >= 0 && ctx->host < ui->host_count)
        ui->hosts[ctx->host].share_count = count;
    ui->share_sel = 0;
    ui->scan_done = true; /* reuse the poll hook to refresh the panel */
    if (count > 0)
        snprintf(ui->scan_msg, sizeof(ui->scan_msg), "%d 个共享", count);
    else if (ui->auth_required)
        snprintf(ui->scan_msg, sizeof(ui->scan_msg), "需要登录才能查看共享");
    else
        snprintf(ui->scan_msg, sizeof(ui->scan_msg), "没有可用的共享");
    free(ctx);
    return NULL;
}

void smb_fetch_shares(App *app, const char *addr)
{
    SmbUi *ui = &app->smb_ui;
    ShareCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->host = ui->host_sel;
    snprintf(ctx->addr, sizeof(ctx->addr), "%s", addr ? addr : "");
    snprintf(ctx->user, sizeof(ctx->user), "%s", ui->user);
    snprintf(ctx->pass, sizeof(ctx->pass), "%s", ui->pass);
    ui->scan_running = true;
    ui->scan_done = false;
    snprintf(ui->scan_msg, sizeof(ui->scan_msg), "正在读取共享...");
    pthread_t th;
    if (pthread_create(&th, NULL, share_thread, ctx) != 0) {
        free(ctx);
        ui->scan_running = false;
        ui->scan_done = true;
        snprintf(ui->scan_msg, sizeof(ui->scan_msg), "无法读取共享列表");
        return;
    }
    pthread_detach(th);
}
