/* SPDX-License-Identifier: MIT */
/*
 * Renames as entries of their own. Staged renames come from git diff-index
 * -M against HEAD. Renames still in the work tree, a file deleted and a
 * new one with similar content, come from a temporary copy of the index,
 * and the real one is never touched.
 */
#ifndef CARVE_RENAME_H
#define CARVE_RENAME_H

#include <stdbool.h>

#include "git.h"
#include "diff.h"
#include "status.h"

struct rename {
	char *from, *to;
	bool staged; /* HEAD to index, else index to work tree */
	int score;   /* similarity, percent */
};

/* Candidates beyond this (deleted or new files) skip work-tree detection. */
#define RENAME_MAX_CANDIDATES 1000

/* The renames among the changes in fs (a git status listing). */
int find_renames(struct repo *r, const struct file_status *fs, size_t n,
		 struct rename **out, size_t *nout);
void free_renames(struct rename *v, size_t n);
/*
 * Fold renames into a status listing. The new path gets 'R' in x (staged)
 * or y (work tree) and the old path in from_x or from_y. The old path
 * loses that side, and its entry when nothing is left. Returns the new n.
 */
size_t apply_renames(struct file_status *fs, size_t n, const struct rename *v,
		     size_t nv);

/* Parse "R<score>\0from\0to\0..." (diff --name-status -z), for tests. */
size_t parse_renames(const char *buf, size_t len, bool staged,
		     struct rename **out);

/*
 * Stage only the move of a work-tree rename. The index loses from and gets
 * to with from's content and mode. The edits made after the move stay
 * unstaged, as ordinary line changes of to.
 */
int stage_move(struct repo *r, const char *from, const char *to);
/* Unstage a staged rename, both halves (git reset for both paths). */
int unstage_move(struct repo *r, const char *from, const char *to);

/*
 * The content changes of a work-tree rename, as the unstaged diff of to
 * will be once the move is staged (computed in a temporary index).
 */
int rename_diff(struct repo *r, const char *from, const char *to,
		const struct diff_opts *opts, struct fdiff **out);

/*
 * The content changes of a staged rename (HEAD:from to index:to), as a
 * plain diff of to whose lines can be unstaged like any others.
 */
int staged_rename_diff(struct repo *r, const char *from, const char *to,
		       const struct diff_opts *opts, struct fdiff **out);

/* The old path if path is the new name of a staged rename, else NULL. */
char *staged_rename_source(struct repo *r, const char *path);
/*
 * The old path if the new (untracked) file path is a rename of a file
 * deleted in the work tree, else NULL.
 */
char *worktree_rename_source(struct repo *r, const char *path);
/*
 * The file in HEAD a new file is an exact copy of, else NULL. The new file
 * is read from the index when cached is set, else from the work tree, and
 * its blob comes from the diff fd if given. A copy is still a new file and
 * this only gives the note.
 */
char *copy_source(struct repo *r, const char *path, bool cached,
		  const struct fdiff *fd);

#endif
