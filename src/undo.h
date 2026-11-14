/* SPDX-License-Identifier: MIT */
/*
 * Undo. Every command that changes the index or HEAD runs between
 * undo_begin() and undo_end(), and a change becomes one entry.
 * refs/worktree/carve/undo/<seq> points to a commit whose tree is the
 * index before the change and whose parent is HEAD before it, which makes
 * git gc keep what undo needs. The message records the state after the
 * change, which must still hold for undo.
 *
 * Operations that span several commands (split, or fixup-to and defer
 * stopped at a conflict) keep their starting state in
 * refs/worktree/carve/undo-pending and become one entry when they end.
 * Entries made in between are folded into it. Undo touches the work tree
 * only for commands that change it (discard). Their files are saved in the
 * entry, as a second parent, and put back.
 */
#ifndef CARVE_UNDO_H
#define CARVE_UNDO_H

#include <stdbool.h>

#include "git.h"

/*
 * The operations undo knows, by the names of the CLI's commands, which the
 * TUI's actions share. An operation is not recorded, recorded, or, when it
 * changes the work tree, recorded with the files it changes saved first.
 */
enum undo_kind { UNDO_NONE, UNDO_INDEX, UNDO_WORKTREE };
enum undo_kind undo_kind(const char *op);

#define UNDO_REFS "refs/worktree/carve/undo/"
#define UNDO_PENDING "refs/worktree/carve/undo-pending"

/*
 * A work-tree file saved for undo, as blob and mode, or "-" when it is
 * absent.
 */
struct undo_file {
	char *path;
	char *blob;
	char *mode; /* "100644", "100755", "120000" or "-" */
};

/*
 * HEAD and index as seen by undo ("-" for an unborn HEAD), and for
 * commands that change the work tree the files they may change.
 */
struct undo_state {
	char *head;
	char *index; /* tree id, NULL when it cannot be written (a conflict) */
	char *stamp; /* the index file's checksum then, if known */
	struct undo_file *files;
	size_t nfiles;
	/*
	 * For a resolved conflict (undo_conflict_begin), the index entries
	 * of its path as "<mode> <blob> <stage>" separated by ",", or "-"
	 * for none
	 */
	char *stages;
	/*
	 * Set by the caller before undo_begin when the command is a line
	 * stage or unstage and nothing else, with no intent-to-add and no
	 * move. Its entry may then be the patch it applies instead of the
	 * index before it.
	 */
	bool as_patch;
};

struct undo_entry {
	char *ref;
	int seq;
	char *op;   /* "stage", "commit", "split", ... */
	char *what; /* arguments, for the list */
	struct undo_state before, after;
	int pending_seq; /* for pending, the last entry before it began */
	long time;	 /* committer time, seconds */
	/* for a line stage or unstage, the patch it applied */
	char *patch;
	bool patch_reverse, patch_zero;
};

/*
 * Add the work-tree files paths (relative to the top) to st. With write
 * their content goes into the object database, for a "before" state.
 */
int undo_capture_files(struct repo *r, struct undo_state *st,
		       const char *const *paths, size_t n, bool write);
void undo_state_release(struct undo_state *st);
bool undo_state_equal(const struct undo_state *a, const struct undo_state *b);

/* Build and parse an entry's commit message. */
void undo_format(const char *op, const char *what,
		 const struct undo_state *before,
		 const struct undo_state *after, int pending_seq,
		 struct strbuf *out);
int undo_parse(const char *commit_obj, size_t len, struct undo_entry *e);
void undo_entry_release(struct undo_entry *e);

/*
 * Around a command. begin captures the state, which is harmless when the
 * command then fails or changes nothing, and end records what changed.
 * Errors in end are reported as warnings, never as the command's failure.
 */
int undo_begin(struct repo *r, struct undo_state *before, const char *op);
/*
 * Taking a side of a conflict in path. The index then has unmerged
 * entries, which no tree can hold, and the entry keeps the file and the
 * path's index entries from before and after.
 */
int undo_conflict_begin(struct repo *r, struct undo_state *before,
			const char *path);
void undo_conflict_end(struct repo *r, struct undo_state *before,
		       const char *path);
void undo_end(struct repo *r, struct undo_state *before, const char *op,
	      const char *what);

/*
 * With r->undo_defer (the TUI), the entries of commands that change only
 * the index (stage, unstage) are kept in r, and undo_flush writes them
 * together. That happens when the keyboard is idle, before any other
 * command is recorded, before undo itself, and on exit. Each is still one
 * entry.
 */
bool undo_queued(const struct repo *r);
int undo_flush(struct repo *r);

/* Entries, oldest first. */
int undo_list(struct repo *r, struct undo_entry **out, size_t *n);
void undo_list_free(struct undo_entry *e, size_t n);

/*
 * Revert the newest entry. Refuses when HEAD or the index changed since
 * it was recorded, unless force. *done (if given) describes what was
 * undone.
 */
int undo_last(struct repo *r, bool force, struct strbuf *done);

/*
 * For git carve gc, fold the steps of an operation that ended without git
 * carve noticing, and drop the entries beyond carve.undoLimit. Without act
 * it only says what it would do, in out.
 */
int undo_gc(struct repo *r, bool act, struct strbuf *out);

/* What the TUI says when an action could not be recorded. */
#define UNDO_FAILED_MSG "could not record for undo: %s"

#endif
