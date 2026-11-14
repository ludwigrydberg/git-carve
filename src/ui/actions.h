/* SPDX-License-Identifier: MIT */
#ifndef CARVE_UI_ACTIONS_H
#define CARVE_UI_ACTIONS_H

#include "ui_int.h"
#include "../absorb.h"

/*
 * Each act_ function is what a key does. keys.c binds them by name, and
 * the comment at each definition names its key.
 */
void act_filter(void);
void sv_select(long row);
void series_goto(void);
void act_file_toggle(bool staged_section);
void act_space(void);
void act_mark(void);
void act_select_hunk(void);
void act_open_file(void);
void act_commit(bool amend);
void act_split(void);
void act_abort(void);
/*
 * The commits that last touched the lines of the change f or t is about to
 * move, from blame_change on the diff it takes them from. Returns -1 with
 * a message in ui.r->err.
 */
int blame_suggest(struct blame_count **v, size_t *n, int *total);
void act_fixup(void);
void act_fixup_commit(bool amend);
void act_squash(void);
void act_series_abort(void);
void act_goto(void);
/* n (dir 1) and N (dir -1): the next hit, into the next file too. */
void search_next(int dir);
void act_search(void);
void act_search_list(void);
/* Forget the hits, for a new search or a change to the files. */
void hits_clear(void);
void act_undo(void);
void act_discard(void);
void act_resolve(bool theirs);
void act_absorb(void);
void act_mark_pair(void);
void act_remove(void);
void act_load_diff(void);
void act_ignore(void);
void act_commit_out(void);
void act_commit_drop(void);
void act_commit_move_to(void);
void act_reword(void);
void act_commit_edit(void);
void act_finish(void);
void act_next_commit(void);
void act_prev_commit(void);
void act_show_commit(void);
struct file_status *cur_fs(void);

void act_message_edit(void);

#endif
