/* SPDX-License-Identifier: MIT */
/*
 * absorb moves every unstaged change block into the commit of the series
 * that last touched its lines, found with git blame -M (and -C with
 * carve.absorbCopies), using fixup-to's machinery. Blocks without one
 * clear target stay.
 */
#ifndef CARVE_ABSORB_H
#define CARVE_ABSORB_H

#include "git.h"

struct fdiff;

struct absorb_hunk {
	char *path;
	char *spec; /* its lines, as a selection (o5-7,n5-8) */
	int line; /* where it is, its first new-side line, else old-side line */
	int new_start, new_count; /* its work-tree lines (the -U0 hunk) */
	char *target;		  /* full commit id, or NULL */
	char *why;		  /* why it stays, when no target */
};

/*
 * Plan the absorb over range, @{upstream}..HEAD for NULL. Nothing may be
 * staged. Hunks come in path and line order.
 */
int absorb_plan(struct repo *r, const char *range, struct absorb_hunk **out,
		size_t *n);
void absorb_plan_free(struct absorb_hunk *v, size_t n);
/*
 * One line per hunk, "path:line -> abc1234 subject" or "path:line stays:
 * why".
 */
void absorb_describe(struct repo *r, const struct absorb_hunk *v, size_t n,
		     struct strbuf *out);

/*
 * Do the plan absorb_plan made just before, with nothing changed since,
 * as every target's fixup in one rebase. Returns 0 when done, -1 on
 * errors, and 1 when it stopped on a conflict. Continue or abort that, and
 * absorb again for the rest. *moved counts the hunks moved.
 */
int absorb_apply_plan(struct repo *r, const char *range,
		      const struct absorb_hunk *plan, size_t n, int *moved);

/* A commit and how many of the blamed lines it last touched. */
struct blame_count {
	char *id;
	int n;
};

/*
 * The commits that last touched the lines of HEAD that a change of fd, a
 * diff against HEAD, is about. Those are the removed lines among the
 * selected ones (all when none is selected), for an added line the removed
 * ones of its block, and else the lines around the added ones, all from
 * one git blame -M of HEAD. Most lines first, and *nlines gets the number
 * of lines blamed. This is b in the f and t pickers.
 */
int blame_change(struct repo *r, const struct fdiff *fd,
		 struct blame_count **out, size_t *n, int *nlines);
void blame_counts_free(struct blame_count *v, size_t n);

#endif
