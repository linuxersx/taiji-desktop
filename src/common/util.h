// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_UTIL_H
#define TAIJI_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...);

char *str_trim(char *s);
bool str_ieq(const char *a, const char *b);
bool str_contains_icase(const char *haystack, const char *needle);
void str_lower(char *s);

char *path_join(const char *a, const char *b);
const char *path_basename(const char *path);
const char *path_extension(const char *path);
char *path_dirname_dup(const char *path);
char *path_absolute(const char *path);
char *expand_home(const char *path);
bool file_exists(const char *path);
bool dir_exists(const char *path);
bool mkdir_p(const char *path);
char *read_file(const char *path, size_t *out_len);
bool write_file_atomic(const char *path, const char *data, size_t len);

/* 用桌面默认程序打开文件（xdg-open / gio open）。 */
bool fs_open_default(const char *path);
bool fs_has_default_app(const char *path);

char **list_dir(const char *path, int *out_count);
void free_strv(char **v, int count);

char *normalize_title(const char *name);       /* lower, strip tags, alnum-only words */
char *pretty_rom_name(const char *filename);   /* display name from filename */
char *escape_field(const char *s);
char *unescape_field(const char *s);
char *html_decode(const char *s);
char *shell_quote(const char *s);
char *format_playtime(int64_t seconds);
char *format_date(int64_t unix_time);

const char *taiji_config_dir(void);
const char *taiji_asset_root(void);
char *asset_path(const char *rel);
char *find_asset_root(void);

uint32_t fnv1a(const char *s);
uint32_t color_blend(uint32_t a, uint32_t b, int t); /* 0..255 */
uint32_t color_alpha(uint32_t color, uint8_t alpha);

#endif
