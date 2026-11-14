/* SPDX-License-Identifier: MIT */
#ifndef CARVE_UI_DRAW_H
#define CARVE_UI_DRAW_H

#include "ui_int.h"

attr_t role_attr(int role);
chtype line_ch(chtype acs, char ascii);
bool msg_box_key(int c);
int put_text_from(int y, int x, int maxw, const char *s, size_t len, int skip);
int puts_at(int y, int x, int maxw, const char *s);
/* Blank w columns from (y, x). */
void fill_line(int y, int x, int w);
void puts_keys(int y, int x, int w, const char *s);
/* Whether the Series pane is there (a repository with commits). */
bool series_mode(void);
bool too_small(void);
/* The whole screen, or what fits a terminal too small for it. */
void draw(void);
/*
 * The screen behind an overlay. It is drawn but not sent to the terminal
 * until the overlay is on it too, or holding a key would show it without
 * one.
 */
void draw_under(void);
/* Colours, unless the terminal has none or NO_COLOR is set. */
void init_colors(void);

#endif
