// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_OOBE_H
#define TAIJI_OOBE_H

#include "app.h"

void oobe_input(App *app, InputAction action);
void oobe_finish(App *app);
void oobe_start(App *app, bool prefill);
void app_oobe_rom_done(App *app);

#endif
