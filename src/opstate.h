/* SPDX-License-Identifier: MIT */
#ifndef CARVE_OPSTATE_H
#define CARVE_OPSTATE_H

#include "git.h"

/*
 * Per worktree (refs/worktree/), since a split in one worktree is nothing
 * to the others. Version 1.0.0 and older used the _OLD names, shared by
 * all worktrees of a repository.
 */
#define SPLIT_REF "refs/worktree/carve/orig"
#define SPLIT_REF_OLD "refs/carve/orig"
#define WT_REF "refs/worktree/carve/worktree"
#define WT_REF_OLD "refs/carve/worktree"

/*
 * An operation of this worktree begun by version 1.0.0 or older moves its
 * refs from the old shared names to the per-worktree ones.
 */
void carve_refs_migrate(struct repo *r);

/* The state of the operation in progress, as files in $GIT_DIR/carve. */
int state_read(struct repo *r, const char *name, struct strbuf *out);
int state_write(struct repo *r, const char *name, const char *data, size_t len);
/* An operation of git carve is in progress (its state files exist). */
bool op_in_progress(struct repo *r);
/* The one refusal for that ({finish}, {abort}: the keys or commands). */
#define OP_BUSY_MSG                                                        \
	"another operation is in progress; {finish} finishes it, {abort} " \
	"gives it up"
/* State file name as one string (trimmed), NULL when there is none. */
char *state_str(struct repo *r, const char *name);
/*
 * Write value as the state file name. A NULL value is an error, and
 * nothing is written.
 */
int state_str_set(struct repo *r, const char *name, const char *value);
/* Remove all state files and the directory. */
void state_remove(struct repo *r);
/*
 * A rollback to tip failed, with git's reason in r->err. The state is
 * left in place. With retry the caller knows that abort repeats exactly
 * these steps, and the error says to use it, if the state is still
 * there. Otherwise it names the git command that puts the branch back.
 * Returns -1.
 */
int rollback_failed(struct repo *r, const char *tip, bool retry);

/*
 * The index and work tree are saved in WT_REF before a rebase cleans them,
 * and put back exactly afterwards. The files come back as they were and
 * the index as HEAD, or as it was with staged. Nothing is merged.
 * save_worktree returns 1 when there was nothing to save.
 */
int save_worktree(struct repo *r);
int restore_worktree(struct repo *r, bool staged);

#endif
