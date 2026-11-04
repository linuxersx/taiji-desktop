// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "out of memory\n"); abort(); }
    return q;
}

char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return xstrdup(""); }
    char *buf = xmalloc((size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

char *str_trim(char *s)
{
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

bool str_ieq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

bool str_contains_icase(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) return true;
    size_t n = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, n) == 0) return true;
    }
    return false;
}

void str_lower(char *s)
{
    for (; s && *s; s++) *s = (char)tolower((unsigned char)*s);
}

char *path_join(const char *a, const char *b)
{
    if (!a || !*a) return xstrdup(b ? b : "");
    if (!b || !*b) return xstrdup(a);
    size_t la = strlen(a);
    while (la > 1 && a[la - 1] == '/') la--;
    if (b[0] == '/') return xstrdup(b);
    return xasprintf("%.*s/%s", (int)la, a, b);
}

const char *path_basename(const char *path)
{
    if (!path) return "";
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

const char *path_extension(const char *path)
{
    const char *base = path_basename(path);
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return "";
    return dot + 1;
}

char *path_dirname_dup(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    if (!slash) return xstrdup(".");
    if (slash == path) return xstrdup("/");
    size_t n = (size_t)(slash - path);
    char *out = xmalloc(n + 1);
    memcpy(out, path, n);
    out[n] = '\0';
    return out;
}

char *path_absolute(const char *path)
{
    if (!path || !*path) return xstrdup("");
    char *expanded = expand_home(path);
    if (expanded[0] == '/') return expanded;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return expanded;
    char *out = path_join(cwd, expanded);
    free(expanded);
    return out;
}

char *expand_home(const char *path)
{
    if (!path) return xstrdup("");
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (!home) home = "";
        return path_join(home, path[1] == '/' ? path + 2 : "");
    }
    return xstrdup(path);
}

bool file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool mkdir_p(const char *path)
{
    if (!path || !*path) return false;
    char *tmp = xstrdup(path);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return false; }
            *p = '/';
        }
    }
    bool ok = mkdir(tmp, 0755) == 0 || errno == EEXIST;
    free(tmp);
    return ok;
}

char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *data = xmalloc((size_t)sz + 1);
    size_t got = fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[got] = '\0';
    if (out_len) *out_len = got;
    return data;
}

bool write_file_atomic(const char *path, const char *data, size_t len)
{
    char *dir = path_dirname_dup(path);
    mkdir_p(dir);
    free(dir);
    char *tmp = xasprintf("%s.tmp.%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) { unlink(tmp); free(tmp); return false; }
    if (rename(tmp, path) != 0) { unlink(tmp); free(tmp); return false; }
    free(tmp);
    return true;
}

char **list_dir(const char *path, int *out_count)
{
    *out_count = 0;
    DIR *d = opendir(path);
    if (!d) return NULL;
    char **items = NULL;
    int count = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (e->d_name[0] == '.') continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            items = xrealloc(items, (size_t)cap * sizeof(char *));
        }
        items[count++] = xstrdup(e->d_name);
    }
    closedir(d);
    *out_count = count;
    return items;
}

void free_strv(char **v, int count)
{
    if (!v) return;
    for (int i = 0; i < count; i++) free(v[i]);
    free(v);
}

static bool is_word_char(unsigned char c)
{
    return isalnum(c) || c >= 0x80;
}

char *normalize_title(const char *name)
{
    if (!name) return xstrdup("");
    char *base = pretty_rom_name(name);
    size_t n = strlen(base);
    char *out = xmalloc(n + 2);
    size_t j = 0;
    bool last_space = true;
    for (size_t i = 0; base[i]; i++) {
        unsigned char c = (unsigned char)base[i];
        if (is_word_char(c)) {
            out[j++] = (char)tolower(c);
            last_space = false;
        } else if (!last_space) {
            out[j++] = ' ';
            last_space = true;
        }
    }
    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';
    free(base);
    return out;
}

char *pretty_rom_name(const char *filename)
{
    if (!filename) return xstrdup("");
    char *base = xstrdup(path_basename(filename));
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    for (char *p = base; *p; p++) if (*p == '_') *p = ' ';
    /* Drop common region/revision/disc tags, the way frontends display names. */
    char *out = xmalloc(strlen(base) + 1);
    size_t j = 0;
    int depth = 0;
    for (size_t i = 0; base[i]; i++) {
        char c = base[i];
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') { if (depth > 0) depth--; }
        else if (depth == 0) out[j++] = c;
    }
    out[j] = '\0';
    char *trimmed = str_trim(out);
    char *res = xstrdup(trimmed);
    free(out);
    free(base);
    /* collapse double spaces */
    char *w = res;
    for (char *r = res; *r; r++) {
        if (*r == ' ' && w > res && w[-1] == ' ') continue;
        *w++ = *r;
    }
    *w = '\0';
    return res;
}

char *escape_field(const char *s)
{
    if (!s) return xstrdup("");
    size_t n = strlen(s);
    char *out = xmalloc(n * 2 + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        switch (c) {
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\t': out[j++] = '\\'; out[j++] = 't'; break;
            case '\n': out[j++] = '\\'; out[j++] = 'n'; break;
            case '\r': out[j++] = '\\'; out[j++] = 'r'; break;
            default: out[j++] = c;
        }
    }
    out[j] = '\0';
    return out;
}

char *unescape_field(const char *s)
{
    if (!s) return xstrdup("");
    size_t n = strlen(s);
    char *out = xmalloc(n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            switch (s[i]) {
                case 't': out[j++] = '\t'; break;
                case 'n': out[j++] = '\n'; break;
                case 'r': out[j++] = '\r'; break;
                case '\\': out[j++] = '\\'; break;
                default: out[j++] = s[i];
            }
        } else out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

static void append_utf8(char **buf, size_t *len, size_t *cap, uint32_t cp)
{
    if (*len + 5 > *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *buf = xrealloc(*buf, *cap);
    }
    if (cp < 0x80) (*buf)[(*len)++] = (char)cp;
    else if (cp < 0x800) {
        (*buf)[(*len)++] = (char)(0xC0 | (cp >> 6));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        (*buf)[(*len)++] = (char)(0xE0 | (cp >> 12));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        (*buf)[(*len)++] = (char)(0xF0 | (cp >> 18));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    }
    (*buf)[*len] = '\0';
}

char *html_decode(const char *s)
{
    if (!s) return xstrdup("");
    char *out = xmalloc(strlen(s) + 1);
    size_t len = 0, cap = strlen(s) + 1;
    out[0] = '\0';
    for (size_t i = 0; s[i]; i++) {
        if (s[i] != '&') {
            if (len + 2 > cap) { cap *= 2; out = xrealloc(out, cap); }
            out[len++] = s[i]; out[len] = '\0';
            continue;
        }
        const char *semi = strchr(s + i, ';');
        if (!semi || semi - (s + i) > 12) {
            out[len++] = s[i]; out[len] = '\0';
            continue;
        }
        char ent[16];
        size_t el = (size_t)(semi - (s + i) - 1);
        if (el >= sizeof(ent)) el = sizeof(ent) - 1;
        memcpy(ent, s + i + 1, el);
        ent[el] = '\0';
        uint32_t cp = 0;
        if (ent[0] == '#') {
            if (ent[1] == 'x' || ent[1] == 'X') cp = (uint32_t)strtoul(ent + 2, NULL, 16);
            else cp = (uint32_t)strtoul(ent + 1, NULL, 10);
        } else if (strcmp(ent, "amp") == 0) cp = '&';
        else if (strcmp(ent, "lt") == 0) cp = '<';
        else if (strcmp(ent, "gt") == 0) cp = '>';
        else if (strcmp(ent, "quot") == 0) cp = '"';
        else if (strcmp(ent, "apos") == 0) cp = '\'';
        else if (strcmp(ent, "nbsp") == 0) cp = ' ';
        if (cp) {
            append_utf8(&out, &len, &cap, cp);
            i = (size_t)(semi - s);
        } else {
            out[len++] = s[i]; out[len] = '\0';
        }
    }
    return out;
}

char *shell_quote(const char *s)
{
    if (!s) return xstrdup("''");
    size_t n = strlen(s);
    char *out = xmalloc(n * 4 + 3);
    size_t j = 0;
    out[j++] = '\'';
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\'') { memcpy(out + j, "'\\''", 4); j += 4; }
        else out[j++] = s[i];
    }
    out[j++] = '\'';
    out[j] = '\0';
    return out;
}

char *format_playtime(int64_t seconds)
{
    if (seconds <= 0) return xstrdup("未游玩");
    int64_t h = seconds / 3600;
    int64_t m = (seconds % 3600) / 60;
    if (h > 0) return xasprintf("%lld 小时 %lld 分", (long long)h, (long long)m);
    if (m > 0) return xasprintf("%lld 分钟", (long long)m);
    return xasprintf("%lld 秒", (long long)seconds);
}

char *format_date(int64_t t)
{
    if (t <= 0) return xstrdup("");
    time_t tt = (time_t)t;
    struct tm tmv;
    localtime_r(&tt, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return xstrdup(buf);
}

const char *taiji_config_dir(void)
{
    static char dir[PATH_MAX];
    if (dir[0]) return dir;
    const char *override = getenv("TAIJI_CONFIG_DIR");
    if (override && *override) {
        snprintf(dir, sizeof(dir), "%s", override);
        mkdir_p(dir);
        return dir;
    }
#ifdef __APPLE__
    const char *home = getenv("HOME");
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/Taiji", home ? home : ".");
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(dir, sizeof(dir), "%s/taiji", xdg);
    else {
        const char *home = getenv("HOME");
        snprintf(dir, sizeof(dir), "%s/.config/taiji", home ? home : ".");
    }
#endif
    mkdir_p(dir);
    return dir;
}

char *find_asset_root(void)
{
    const char *env = getenv("TAIJI_ASSETS");
    if (env && dir_exists(env)) return xstrdup(env);

    char exe[PATH_MAX];
#if defined(__APPLE__)
    uint32_t n = (uint32_t)sizeof(exe);
#endif
#if defined(__linux__)
    ssize_t got = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (got > 0) {
        exe[got] = '\0';
        char *dir = path_dirname_dup(exe);
        char *cand = path_join(dir, "assets");
        if (dir_exists(cand)) { free(dir); return cand; }
        free(cand);
        char *share = path_join(dir, "../share/taiji/assets");
        free(dir);
        if (dir_exists(share)) return share;
        free(share);
    }
#elif defined(__APPLE__)
    /* _NSGetExecutablePath is in <mach-o/dyld.h>; fall back to argv is not available here. */
    extern int _NSGetExecutablePath(char *buf, uint32_t *size);
    if (_NSGetExecutablePath(exe, &n) == 0) {
        char *dir = path_dirname_dup(exe);
        char *cand = path_join(dir, "assets");
        if (dir_exists(cand)) { free(dir); return cand; }
        free(cand);
        char *share = path_join(dir, "../share/taiji/assets");
        free(dir);
        if (dir_exists(share)) return share;
        free(share);
    }
#endif
    if (dir_exists("assets")) return xstrdup("assets");
    if (dir_exists(TAIJI_BUILD_ASSET_DIR)) return xstrdup(TAIJI_BUILD_ASSET_DIR);
    return xstrdup(TAIJI_BUILD_ASSET_DIR);
}

const char *taiji_asset_root(void)
{
    static char *root = NULL;
    if (!root) root = find_asset_root();
    return root;
}

char *asset_path(const char *rel)
{
    return path_join(taiji_asset_root(), rel);
}

uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; s && *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

uint32_t color_blend(uint32_t a, uint32_t b, int t)
{
    if (t <= 0) return a;
    if (t >= 255) return b;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + (br - ar) * t / 255;
    int g = ag + (bg - ag) * t / 255;
    int bl = ab + (bb - ab) * t / 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

uint32_t color_alpha(uint32_t color, uint8_t alpha)
{
    return (color & 0x00FFFFFFu) | ((uint32_t)alpha << 24);
}

static bool open_command_exists(const char *name)
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

static bool read_first_line(const char *cmd, char *out, size_t n)
{
    out[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    pclose(f);
    if (!ok) return false;
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] != '\0';
}

bool fs_has_default_app(const char *path)
{
    if (!path || !*path) return false;
    if (!open_command_exists("xdg-mime")) return false;
    char *q = shell_quote(path);
    char cmd[PATH_MAX + 64];
    char mime[128];
    snprintf(cmd, sizeof(cmd), "xdg-mime query filetype %s 2>/dev/null", q);
    free(q);
    if (!read_first_line(cmd, mime, sizeof(mime))) return false;

    char *qm = shell_quote(mime);
    char app[256];
    snprintf(cmd, sizeof(cmd), "xdg-mime query default %s 2>/dev/null", qm);
    free(qm);
    if (!read_first_line(cmd, app, sizeof(app))) return false;

    const char *home = getenv("HOME");
    char p[PATH_MAX];
    if (home && *home) {
        snprintf(p, sizeof(p), "%s/.local/share/applications/%s", home, app);
        if (access(p, R_OK) == 0) return true;
        snprintf(p, sizeof(p), "%s/.local/share/flatpak/exports/share/applications/%s",
                 home, app);
        if (access(p, R_OK) == 0) return true;
    }
    snprintf(p, sizeof(p), "/usr/share/applications/%s", app);
    if (access(p, R_OK) == 0) return true;
    snprintf(p, sizeof(p), "/usr/local/share/applications/%s", app);
    if (access(p, R_OK) == 0) return true;
    snprintf(p, sizeof(p), "/var/lib/flatpak/exports/share/applications/%s", app);
    if (access(p, R_OK) == 0) return true;
    snprintf(p, sizeof(p), "/var/lib/snapd/desktop/applications/%s", app);
    if (access(p, R_OK) == 0) return true;
    return false;
}

bool fs_open_default(const char *path)
{
    if (!path || !*path) return false;
    const char *prog = NULL;
    if (open_command_exists("xdg-open")) prog = "xdg-open";
    else if (open_command_exists("gio")) prog = "gio";
    else return false;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        if (strcmp(prog, "gio") == 0)
            execlp(prog, prog, "open", path, (char *)NULL);
        else
            execlp(prog, prog, path, (char *)NULL);
        _exit(127);
    }
    return true;
}
