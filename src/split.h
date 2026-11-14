/* SPDX-License-Identifier: MIT */
#ifndef CARVE_SPLIT_H
#define CARVE_SPLIT_H

#include "git.h"
#include "status.h"

struct commit_opts {
	const char *message; /* -m */
	const char *file;    /* -F */
	bool amend;
	bool quiet;   /* pass -q to git commit */
	bool signoff; /* add Signed-off-by unless already there */
	/*
	 * With the editor, why this commit, as comment lines below the
	 * message instead of git's status. In a rebase or a carve git's
	 * status describes git's steps, not yours.
	 */
	const char *note;
	/*
	 * Leave git's status out of the editor. With thousands of changed
	 * files it is a huge buffer and seconds of git, and the TUI shows
	 * the state anyway.
	 */
	bool no_status;
};

/*
 * Commit what is staged. Without a message the editor opens, and the
 * terminal must be available. During a split the editor starts from a
 * template, the original message for the first commit and a commented copy
 * for later ones, with the Change-Id handled as carve.keepChangeId says.
 */
int carve_commit(struct repo *r, const struct commit_opts *o);
/*
 * What the editor of carve_commit would start with. During a split that is
 * the template of this part, else the subsystem prefix, and with signoff
 * the committer's Signed-off-by. The TUI's message pane starts from it.
 */
int commit_template(struct repo *r, bool signoff, struct strbuf *out);
/*
 * Commit what is staged with the message being written (DRAFT_FILE in
 * message.h), comments left out, as carve_commit would with -m. The draft
 * goes once the commit is made.
 */
int commit_draft(struct repo *r, const struct commit_opts *o);
/* The scissors line and what it means, as comment lines c */

struct split_state {
	bool active;
	bool older; /* splitting a commit below HEAD (rebase) */
	char *orig; /* commit being split */
	char *subject;
	int commits_made;
	int added, removed; /* orig vs. HEAD */
	bool done;	    /* tree matches and nothing is left */
	bool in_rebase;	    /* HEAD where a rebase of yours stopped */
};

int split_state_load(struct repo *r, struct split_state *st);
/*
 * The same, clean telling whether the index and tracked files have no
 * changes (a status the caller read), -1 to ask git.
 */
int split_state_load_clean(struct repo *r, struct split_state *st, int clean);
void split_state_release(struct split_state *st);
/* "SPLIT a1b2c3d "subject" — 2 commits made, remaining diff +14/-3" */
void split_status_line(const struct split_state *st, struct strbuf *out);

/* Start splitting commit, or HEAD for NULL. */
int split_start(struct repo *r, const char *commit);
/*
 * The status right after split_start of HEAD, sorted as load_status
 * sorts it. The tree was clean and the commit's changes are now
 * unstaged, so that is the commit's diff, and no git status has to hash
 * every file the reset left without stat data. Files the commit added
 * are untracked again and come with the scan. -1 when it cannot tell.
 */
int split_start_status(struct repo *r, struct file_status **out, size_t *n);
/*
 * Finish the split, once nothing is left to stage and the tree equals the
 * original commit's tree. *out, if given, then lists the new commits
 * oldest first, one full id per line.
 */
int split_finish(struct repo *r, struct strbuf *out);
/*
 * Restore the original commit, index and work tree exactly. Returns 1
 * when, forced, it only forgot a split whose rebase (one of yours) has
 * moved on, and changed nothing else.
 */
int split_abort(struct repo *r, bool force);

/*
 * While splitting HEAD and before the first commit, move what is staged
 * into the commit below the split, for a change of the split commit that
 * belongs there. The split must still add up to the original tree to
 * finish.
 */
int split_amend_base(struct repo *r);
/*
 * Lines taken out of the work tree during a split (discarded) stop being
 * part of what the split adds up to. split_take_begin() checks before a
 * change is taken out of path that the file is as the split commit has it,
 * not edited during the split. split_take_end() then takes the change out
 * of the split's target tree. Without a split both do nothing. Finishing
 * accepts the original tree or the target tree.
 */
struct split_take {
	char *path;
	char *before; /* work-tree blob before, NULL if absent */
	char *mode;
	char *tree; /* the target tree before */
};

int split_take_begin(struct repo *r, const char *path, struct split_take *t);
int split_take_end(struct repo *r, struct split_take *t);
void split_take_release(struct split_take *t);

/* Why lines of a commit cannot be moved while splitting. */
#define SPLIT_BUSY_MOVE_MSG \
	"not during a split: its commits must add up to the original"

#endif
