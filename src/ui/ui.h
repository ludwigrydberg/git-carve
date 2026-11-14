/* SPDX-License-Identifier: MIT */
#ifndef CARVE_UI_H
#define CARVE_UI_H

struct repo;

/*
 * Run the TUI on range, or on the series for NULL, with its commits in the
 * Series pane and the work tree. Returns 0 on a normal exit.
 */
int ui_main(struct repo *r, const char *range);
/* git carve keys, printing the key bindings in effect. */
int ui_print_keys(struct repo *r);

#endif
