// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_EXTERNAL_OSK_H
#define TAIJI_EXTERNAL_OSK_H

#include "app.h"

bool external_osk_available(void);
bool external_osk_show(App *app);
void external_osk_hide(App *app);
void external_osk_poll(App *app);
void external_osk_close(App *app);

#endif
