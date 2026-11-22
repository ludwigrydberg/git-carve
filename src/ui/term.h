/* SPDX-License-Identifier: MIT */
#ifndef CARVE_UI_TERM_H
#define CARVE_UI_TERM_H

#include "ui_int.h"

void draw_frame(int y0, int x0, int h, int w);
bool prompt(const char *label, char *buf, size_t sz);
bool prompt_live(const char *label, char *buf, size_t sz,
		 void (*changed)(const char *text));
/* A question at the bottom line. Only y says yes. */
bool confirm(const char *question);
void show_warnings(void);
/* Ctrl-Z handled by us (leaving curses first), or by the default. */
void set_tstp(bool ours);
/* Leave curses for a child that needs the terminal, and come back. */
void suspend_curses(void);
void resume_curses(void);
char *pick_commit_sel(const char *title, const char *rev, const char *sel,
		      bool blame);
void view_lines_paint(const char *title, char *const *lines, size_t n,
		      line_paint_fn paint);
/*
 * A choice among lines, each painted by paint or plain for NULL. Returns
 * its index, or -1.
 */
long pick_list(const char *title, char *const *lines, size_t n, size_t cur,
	       line_paint_fn paint);
size_t paint_show(char *const *lines, size_t n, size_t i, struct span *sp,
		  size_t max);
size_t paint_hit(char *const *lines, size_t n, size_t i, struct span *sp,
		 size_t max);
size_t paint_absorb(char *const *lines, size_t n, size_t i, struct span *sp,
		    size_t max);
bool confirm_list(const char *title, char *const *lines, size_t n,
		  const char *verb, line_paint_fn paint);
enum action help_pick(void);

#endif
