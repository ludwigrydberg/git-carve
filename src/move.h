/* SPDX-License-Identifier: MIT */
#ifndef CARVE_MOVE_H
#define CARVE_MOVE_H

#include "git.h"

/*
 * Changes to any commit of HEAD's history (the Series pane's commits),
 * without stopping there by hand. One "git rebase -i" runs with a break
 * after every commit the change touches. At each break the step is done
 * (lines taken out or put in, a new commit, a new message) and the rebase
 * goes on. A conflict stops it as it stops fixup-to, to be resolved,
 * staged and continued, or aborted. The work tree's own changes are saved
 * and put back. The result is checked, and when the tip's tree is not what
 * the change makes of it, everything is rolled back.
 */
enum move_how {
	MOVE_OUT,    /* out of the commit, into the work tree (unstaged) */
	MOVE_DROP,   /* out of the commit and of the work tree */
	MOVE_INTO,   /* into commit dest, older or newer */
	MOVE_NEW,    /* into a new commit right after dest */
	MOVE_REWORD, /* the commit's message, or the editor when there is none */
	MOVE_EDIT,   /* path in the commit becomes content (commit_region) */
};

struct commit_opts;

struct move_req {
	enum move_how how;
	const char *commit;
	const char *path; /* not for MOVE_REWORD */
	const char *sel;  /* its lines in commit's diff, or NULL for all */
	const char *dest; /* MOVE_INTO, MOVE_NEW */
	const struct commit_opts *co; /* MOVE_NEW, MOVE_REWORD */
	const char *content; /* the file as it is to be, for MOVE_EDIT */
	size_t len;
};

/*
 * Returns 0 when done (*msg says what), 1 when stopped on a conflict
 * (r->err says how to go on), -1 on errors (nothing changed).
 */
int move_lines(struct repo *r, const struct move_req *m, struct strbuf *msg);
/*
 * For MOVE_EDIT, the region of path in commit that the lines sel of its
 * diff cover (context included), its text, and the blob it is in. The file
 * as it is to be is splice_lines(blob, rg->start, rg->count, ...).
 */
struct region;
int commit_region(struct repo *r, const char *commit, const char *path,
		  const char *sel, struct region *rg, struct strbuf *blob);
/*
 * Continue the move in progress after a conflict was resolved, or give it
 * up.
 */
int move_continue(struct repo *r);
int move_abort(struct repo *r);
bool move_in_progress(struct repo *r);

#endif
