/* SPDX-License-Identifier: MIT */
#ifndef CARVE_PATCH_H
#define CARVE_PATCH_H

#include "diff.h"

enum patch_dir {
	PATCH_STAGE,   /* fd is an unstaged diff, applied forward */
	PATCH_UNSTAGE, /* fd is a staged diff, applied with --reverse */
};

/*
 * Build a patch for the selected lines of fd (dline.sel), following the
 * partial patch rule. An unselected '+' line is dropped when staging and
 * becomes context when unstaging. An unselected '-' line becomes context
 * when staging and is dropped when unstaging. Hunks without selected
 * changes are left out, and hunk headers are computed again. A context
 * line whose end of line differs between the side being patched and the
 * result (a "\ No newline" line that stops being last) is written as a '-'
 * and '+' pair. Returns the number of hunks written, 0 when nothing is
 * selected, or -1 with a message in *err.
 */
int build_patch(const struct fdiff *fd, enum patch_dir dir, struct strbuf *out,
		struct strbuf *err);

/* git apply --cached [--reverse] --recount --whitespace=nowarn - */
/*
 * git apply of patch on stdin, with whitespace kept as it is and
 * --recount. flags say where it goes (the index, the index and the work
 * tree, or else the work tree), and whether it is reversed or -U0.
 */
#define APPLY_CACHED 1u
#define APPLY_INDEX 2u
#define APPLY_REVERSE 4u
#define APPLY_ZERO 8u
int git_apply(struct repo *r, const struct strbuf *patch, unsigned flags);
int apply_patch(struct repo *r, const struct strbuf *patch, bool reverse);
/*
 * apply_patch for a patch without context (-U0), which git apply needs
 * --unidiff-zero for.
 */
int apply_patch_zero(struct repo *r, const struct strbuf *patch, bool reverse,
		     bool zero);

/* Stage (or unstage) the selected lines of fd, a diff of the right kind. */
int apply_selection(struct repo *r, const struct fdiff *fd, enum patch_dir dir);

/*
 * Load the diff of path, select spec (see sel_apply) and stage/unstage it.
 * Untracked files are marked intent-to-add first.
 */
int stage_lines(struct repo *r, const char *path, const char *spec,
		enum patch_dir dir);

int stage_file(struct repo *r, const char *path);
int unstage_file(struct repo *r, const char *path);
/* The same for many paths, in one git process. */
int stage_files(struct repo *r, const char *const *paths, size_t n);
/* Many tracked files by exact path, through git update-index. */
int stage_tracked_paths(struct repo *r, const char *const *paths, size_t n);
/*
 * Unstage everything. One pathspec does it, which took 0.2 s where the
 * 15,000 paths of a kernel release took 2.5 s to match. Without HEAD it is
 * git rm --cached.
 */
int unstage_all(struct repo *r);
int unstage_files(struct repo *r, const char *const *paths, size_t n);
int intent_to_add(struct repo *r, const char *path);

/*
 * Discard the selected lines of path's unstaged diff from the work tree
 * file, or all its unstaged changes by a checkout from the index. An
 * untracked or intent-to-add file is deleted. Undo saves the file first.
 */
int discard_lines(struct repo *r, const char *path, const char *spec);
/* The same for the selected lines (dline.sel) of an unstaged diff. */
int discard_selection(struct repo *r, const struct fdiff *fd);
int discard_file(struct repo *r, const char *path);
/*
 * git rm, deleting path from the work tree and staging its deletion, local
 * changes included. Undo saves the file first. A path git does not track
 * is refused.
 */
int remove_file(struct repo *r, const char *path);

/* A selection without added or removed lines. */
#define NO_CHANGED_LINES_MSG "selection %s contains no changed lines"

#endif
