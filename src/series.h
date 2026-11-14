/* SPDX-License-Identifier: MIT */
#ifndef CARVE_SERIES_H
#define CARVE_SERIES_H

#include "diff.h"
#include "patch.h"

/*
 * Operations on older commits of a series. They all run a non-interactive
 * "git rebase -i" and keep their state in $GIT_DIR/carve. A stop at a
 * conflict can then be continued or aborted later, from the CLI or the
 * TUI.
 */

enum series_op { OP_NONE, OP_FIXUP, OP_DEFER, OP_MOVE, OP_SQUASH };

struct series_state {
	enum series_op op;
	char *target; /* commit being changed */
	char *subject;
	bool rebasing;	     /* a rebase is in progress */
	int conflicts;	     /* unmerged paths */
	bool foreign_rebase; /* a rebase we did not start */
};

int series_state_load(struct repo *r, struct series_state *st);
void series_state_release(struct series_state *st);
/* "FIXUP-TO a1b2c3d "subject": 2 conflicted files" and the like. */
void series_status_line(const struct series_state *st, struct strbuf *out);

struct fixup_opts {
	bool amend;	     /* an amend! with a new message */
	const char *message; /* the new message of amend!, NULL for the editor */
	const char *file;    /* or that message read from a file */
	bool quiet;	     /* git commit -q */
	/*
	 * the caller staged the change on a clean index, and it is
	 * unstaged again when no commit is made
	 */
	bool was_clean;
};

/*
 * A fixup commit of commit, one of the series, for a later squash. What is
 * staged is committed as "fixup! <subject>", or with amend as "amend!
 * <subject>" with the new message, where staged changes are optional.
 */
int fixup_commit(struct repo *r, const char *commit,
		 const struct fixup_opts *o);

/* A commit of a squash's series. */
struct squash_commit {
	char *id;
	char *subject;
	char *body; /* amend!: the new message */
	int nparents;
	char kind;   /* 'f' fixup!, 'a' amend!, 's' squash!, 0 */
	long target; /* the index of its commit, or -1 for none */
};

/* What squash would do, the commits oldest first and the rebase's todo. */
struct squash_plan {
	struct squash_commit *c;
	size_t n;
	size_t first; /* the oldest commit changed */
	int nfix;     /* fixups squashed */
	int ntargets; /* into that many commits */
	struct strbuf todo;
	struct strbuf left; /* fixups without their commit, which stay */
	char *tip;
};

/*
 * Squash the fixup!, amend! and squash! commits of the series into their
 * commits, as git rebase --autosquash does, amend! too on any git. The
 * series is base..HEAD, by default @{upstream}..HEAD, or the last 200
 * commits without an upstream. squash_plan only looks. squash_run rewrites
 * and returns 0 when done, 1 when it stopped on a conflict and -1 when
 * nothing changed. The series' tree stays the same.
 */
int squash_plan(struct repo *r, const char *base, struct squash_plan *p);
int squash_run(struct repo *r, struct squash_plan *p);
void squash_plan_release(struct squash_plan *p);

/*
 * fixup-to commits what is staged as "fixup! <subject>" and autosquashes
 * it into commit. was_clean says the caller staged exactly the selection
 * on a clean index, and a failure or abort then unstages it again.
 * Otherwise it stays staged. Returns 0 when done, or 1 when the rebase
 * stopped at a conflict, with a message in r->err.
 */
int fixup_to(struct repo *r, const char *commit, bool was_clean);
/*
 * The tree of commit with the staged change (as fixup-to builds it to put
 * right after commit), or NULL when the change does not apply there.
 */
char *change_tree_onto(struct repo *r, const char *commit);
/*
 * fixup-to into n commits in one rebase. trees[i] is targets[i] with its
 * part of the change (change_tree_onto), targets[0] is the oldest, and the
 * index holds the whole change. Returns as fixup_to does.
 */
int fixup_trees(struct repo *r, char *const *targets, char *const *trees,
		size_t n, bool was_clean);

/*
 * defer moves the lines spec selects in commit's diff of path out of it,
 * into a new commit right after it, or after the commit after. msg may be
 * NULL for the editor. Returns 0 when done, or 1 when it stopped at a
 * conflict.
 */
int defer_lines(struct repo *r, const char *commit, const char *path,
		const char *spec, const char *after, const char *msg);

/* Continue / abort the fixup-to, defer or squash in progress (op_*). */
int series_continue(struct repo *r);
int series_abort(struct repo *r);

#endif
