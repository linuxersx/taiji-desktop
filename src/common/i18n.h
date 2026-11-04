// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_I18N_H
#define TAIJI_I18N_H

/* 0 = 中文，1 = English */
void i18n_set_language(int lang);
int i18n_language(void);
const char *i18n_language_name(int lang);
const char *tr(const char *text);

#endif
