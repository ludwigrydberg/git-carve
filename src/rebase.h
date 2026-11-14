/* SPDX-License-Identifier: MIT */
#ifndef CARVE_REBASE_H
#define CARVE_REBASE_H

#include "git.h"

/*
 * Check that commit is a non-merge ancestor of HEAD with no merges above
 * it, and that no operation is in progress. *full gets the full id.
 */
int check_series_commit(struct repo *r, const char *commit, char **full);

/*
 * git rebase -i onto base, or --root for NULL, with seq as the sequence
 * editor and quiet message editors. A NULL seq leaves git's todo as it is.
 * Returns git's exit status, with its stderr in err.
 */
int rebase_start(struct repo *r, const char *base, const char *seq,
		 bool autosquash, struct strbuf *err);
/*
 * Start a rebase that stops at commit (edit), optionally with a break
 * right after break_after.
 */
int rebase_edit_at(struct repo *r, const char *commit, const char *break_after);

bool rebase_in_progress(struct repo *r);

/*
 * HEAD of the rebase in progress was rewritten from was to now, the same
 * commit with new parents. An edit stop's rebase-merge/amend then follows
 * it, and git rebase --continue amends staged changes into it as it would
 * have into was. Nothing changes when amend names another commit.
 */
int rebase_amend_follow(struct repo *r, const char *was, const char *now);

/* The commit the rebase in progress replays onto, or NULL. */
char *rebase_onto(struct repo *r);

/*
 * A rebase you started is in progress, not one of git carve's (a split of
 * an older commit, fixup-to, defer).
 */
bool foreign_rebase(struct repo *r);

/* The refusal for when a rebase that is not yours is in progress. */
#define REBASE_BUSY_MSG "a rebase is in progress; finish or abort it first"
/* Why an operation that needs a rebase of its own waits for yours. */
#define YOUR_REBASE_MSG                                                 \
	"a rebase of yours is in progress: where it stops, split HEAD " \
	"(split) or move lines between the commits it replayed "        \
	"(move); the rest once the rebase is done"

/*
 * Where the rebase in progress is, as step of total. False when that is
 * unknown.
 */
bool rebase_step(struct repo *r, int *step, int *total);

/*
 * git rebase --continue (message editor suppressed). Returns 0 when the
 * rebase finished, 1 when it stopped again, -1 on error.
 */
int rebase_continue(struct repo *r);

int rebase_abort(struct repo *r);

int count_conflicts(struct repo *r);

/*
 * After a replay the commits of range, oldest first, must have the trees
 * of old_ids, one id per line. Returns -1 with the first difference. The
 * replay is done by then, and when git cannot tell (old_ids is NULL or git
 * fails) it only warns and returns 0.
 */
int check_replayed(struct repo *r, const char *old_ids, const char *range);

/*
 * The series is the commits of HEAD that are not upstream yet. It ends at
 * HEAD's upstream, else at what the remote-tracking branches have. Pushes
 * the revisions for git log or rev-list onto a ("HEAD" and "^<upstream>",
 * or "HEAD --not --remotes") and returns 0. With neither an upstream nor a
 * remote-tracking branch it pushes "-n SERIES_MAX HEAD" and returns 1.
 * name, if not NULL, gets what to call the end, such as "origin/main" or
 * "pushed".
 */
#define SERIES_MAX 200
int series_revs(struct repo *r, struct argv *a, char **name);

/*
 * How many first-parent commits of tip are not in base, or in no
 * remote-tracking branch when base is NULL. -1 when git cannot tell.
 */
long series_count(struct repo *r, const char *tip, const char *base);

/* rev-list --reverse range into out. */
int list_commits(struct repo *r, const char *range, struct strbuf *out);

#endif
