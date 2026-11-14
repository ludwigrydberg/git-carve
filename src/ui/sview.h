/* SPDX-License-Identifier: MIT */
#ifndef CARVE_UI_SVIEW_H
#define CARVE_UI_SVIEW_H

#include "ui_int.h"

/* sview.c, the Series pane */

/*
 * The series of HEAD down to the base of range (<base>.. ), or else to
 * HEAD's upstream or what the remotes have. Only its first rows load.
 */
int sv_open(struct repo *r, const char *range);
void sv_release(void);
/* The rows, the work tree's row first. */
size_t sv_rows(void);
/* The commit of row, NULL for the work tree. */
const char *sv_commit(size_t row);
size_t sv_row_of(const char *id);
/* With the cursor on the last row loaded, load more of the history. */
void sv_grow(size_t row);
void sv_load_range(void);
void sv_label(size_t k, char *buf, size_t len);
/* Whether the commit shown is HEAD. */
bool head_shown(void);
/*
 * Open the series again when HEAD moved (or full, also when the upstream
 * changed). A step forward only adds rows.
 */
void sv_refresh(bool full);
/* Until a row is chosen, the cursor is on the work tree if dirty, else HEAD. */
void sv_follow(bool dirty);

#endif
