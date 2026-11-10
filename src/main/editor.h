// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_EDITOR_H
#define TAIJI_EDITOR_H

#include "app.h"

#include <stdbool.h>

bool editor_is_text_path(const char *path);
bool editor_open(App *app, const char *path);
void editor_close(App *app);
bool editor_save(App *app);
void editor_set_line(App *app, int idx, const char *text);
void editor_append_line(App *app);
void editor_insert_after(App *app, int idx);
void editor_edit_line(App *app, int idx);
void editor_input(App *app, InputAction action, const char *text);
void editor_begin_edit(App *app);
void editor_stop_edit(App *app);
void editor_insert_text(App *app, const char *text);
void editor_backspace(App *app);
void editor_enter(App *app);
void editor_clear_line(App *app);
void editor_leave(App *app);
const char *editor_format_label(const char *path);

#endif
