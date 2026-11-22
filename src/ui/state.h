/* SPDX-License-Identifier: MIT */
#ifndef CARVE_UI_STATE_H
#define CARVE_UI_STATE_H

#include "ui_int.h"

void dp_clear(struct dpane *dp);
/* The unstaged (or cached) diff of path into the pane, up to the limit. */
void dp_load(struct dpane *dp, const char *path, bool cached);
/*
 * A file's diff for the view of all files, from the diff cache only, and
 * no git. False when the cache does not have it.
 */
bool dp_preview(struct dpane *dp, const char *path, bool cached);
/* A row with a note, such as why there is no diff. */
void dp_push_note(struct dpane *dp, const char *note);
/* The rows of an unmerged file, each conflict with its sides. */
size_t conflict_view(const char *path, struct crow **out);
/*
 * The number conflict i of the nc left in path had when the file was
 * first shown, and in *total how many there were then.
 */
size_t conflict_number(const char *path, size_t nc, size_t i, size_t *total);
/* Conflict i of path was taken ours or theirs. Its number stays used. */
void conflict_resolved(const char *path, size_t nc, size_t i);
void cnums_free(void);
const char *commit_from(const char *path);
void crows_free(struct crow *v, size_t n);
const char *sec_name(enum sec s);
/* The path of item k of the Files list, a file of the commit shown too. */
const char *item_path(size_t k);
bool item_is_dir(size_t k);
bool item_is_fold(size_t k);
bool item_stop(size_t k);
bool item_is_section(size_t k);
size_t section_files(enum sec sec, bool untracked, size_t **out);
void fold_untracked(int how);
bool untracked_match(const char *pattern);
bool untracked_shown(void);
void set_filter(const char *pattern);
size_t filter_count(void);
/* The file under the Files cursor becomes the one the diff panes show. */
void select_item(void);
void load_head(void);
/*
 * "%h %s" of rev with the subject quoted, without git while the title
 * knows the abbreviation. NULL if rev is no commit.
 */
char *commit_line(const char *rev, bool quote);
/* ui.tracked holds for the index file as it is now (or cannot tell). */
bool status_still(void);
/* The index file was written by us without changing what status said. */
void status_restamp(void);
/* undo_flush, which writes only the index's tree cache */
int flush_undo(void);
/* Anything staged (no git while the status read holds). */
bool staged_any(void);
/* Status, HEAD, the operation in progress and the diffs, read again. */
void reload(void);
/* reload, also reading again what no command of ours changes (F5) */
void reload_full(void);
/*
 * After a command that changed only the index entry of path (a line or
 * file staged): read that path's status and diffs again, not everything.
 */
void reload_path(const char *path);
void scan_done(void);
void start_scan(void);
void sv_show(void);
int tracked_clean_now(void);
void diffs_stale(void);
/* Load the diffs of the selected file, if they are stale. */
void diffs_load(void);

/* The commit message being written, from its file (no git). */
void load_draft(void);
/* What the message pane shows, the message of the commit shown or the draft. */
const struct strbuf *pane_message(void);

#endif
