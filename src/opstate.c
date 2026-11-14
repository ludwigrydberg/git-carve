// SPDX-License-Identifier: MIT
/*
 * The state of an operation in progress, as files in $GIT_DIR/carve, the
 * per-worktree refs, and the work tree saved while a rebase runs.
 */
#include "opstate.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Snapshot the index and work tree with git stash create, which makes a
 * commit whose tree is the work tree and whose second parent is the index,
 * and keep it in a ref. The caller then cleans the work tree for the
 * rebase.
 */
int save_worktree(struct repo *r)
{
	struct strbuf out = STRBUF_INIT;
	int rc = 0;

	if (git_out(r, &out, "stash", "create", "git carve", NULL)) {
		sb_release(&out);
		return repo_err(r, "cannot save the work tree state (git "
				   "stash create failed)");
	}
	sb_rtrim(&out);
	/*
	 * nothing changed (a squash on a clean tree): nothing to put back;
	 * an old snapshot (a crash) must not come back then
	 */
	if (!out.len) {
		char *old = repo_rev(r, WT_REF);

		if (old && git_quiet(r, "update-ref", "-d", WT_REF, NULL))
			rc = -1;
		free(old);
		sb_release(&out);
		return rc ? rc : 1;
	}
	if (git_quiet(r, "update-ref", WT_REF, out.buf, NULL))
		rc = -1;
	sb_release(&out);
	return rc;
}

/*
 * Put the work tree files back exactly as they were, and the index to
 * HEAD, or with staged to the index as it was. Nothing is merged, and the
 * rebase never rewrites the user's files.
 */
int restore_worktree(struct repo *r, bool staged)
{
	struct strbuf idx = STRBUF_INIT;
	char *s = repo_rev(r, WT_REF);
	int rc = 0;

	if (!s)
		return 0;
	if (git_quiet(r, "restore", "--source", s, "--worktree", "--", ".",
		      NULL))
		rc = -1;
	sb_addf(&idx, "%s^2^{tree}", s);
	if (!rc && staged)
		rc = git_quiet(r, "read-tree", idx.buf, NULL) ? -1 : 0;
	else if (!rc)
		rc = git_quiet(r, "reset", "-q", NULL) ? -1 : 0;
	if (!rc && git_quiet(r, "update-ref", "-d", WT_REF, NULL)) {
		/* the files are back, only the ref that saved them stays */
		repo_warn(r,
			  "warning: your files are back, but %s could not be "
			  "removed (%s); git update-ref -d %s removes it",
			  WT_REF, sb_str(&r->err), WT_REF);
		sb_reset(&r->err);
	} else if (rc) {
		repo_warn(r,
			  "warning: your work tree state is saved in %s "
			  "(%.7s); restore it with git restore --source=%s "
			  "--worktree -- .",
			  WT_REF, s, WT_REF);
	}
	free(s);
	sb_release(&idx);
	return rc;
}

int state_read(struct repo *r, const char *name, struct strbuf *out)
{
	char *p = carve_path(r, name);
	int rc = read_file(p, out);

	free(p);
	return rc;
}

/*
 * The file is written next to its place and renamed there, so a write
 * that fails or is cut short leaves the old content as it was. Abort and
 * continue read these files after a crash.
 */
int state_write(struct repo *r, const char *name, const char *data, size_t len)
{
	struct strbuf tmp = STRBUF_INIT;
	char *p;
	int rc;

	if (mkdir(r->carve_dir, 0777) && errno != EEXIST)
		return repo_err(r, "cannot create %s: %s", r->carve_dir,
				strerror(errno));
	p = carve_path(r, name);
	sb_addf(&tmp, "%s.tmp", p);
	rc = write_file(tmp.buf, data, len) || rename(tmp.buf, p);
	if (rc) {
		repo_err(r, "cannot write %s: %s", p, strerror(errno));
		unlink(tmp.buf);
	}
	free(p);
	sb_release(&tmp);
	return rc ? -1 : 0;
}

int rollback_failed(struct repo *r, const char *tip, bool retry)
{
	char *why = xstrdup(r->err.len ? sb_str(&r->err) : "git failed");

	sb_reset(&r->err);
	if (retry && tip && op_in_progress(r))
		sb_addf(&r->err,
			"could not roll back to %.7s (%s); {abort} tries again",
			tip, why);
	else if (tip)
		sb_addf(&r->err,
			"could not roll back (%s); git reset --hard %s puts "
			"the branch back",
			why, tip);
	else
		sb_addf(&r->err, "could not roll back (%s)", why);
	free(why);
	return -1;
}

void state_remove(struct repo *r)
{
	static const char *const files[] = {
		"msg",
		"base",
		"mode",
		"tip",
		"COMMIT_MSG",
		"COMMIT_TEMPLATE",
		"abort-index",
		"op",
		"target",
		"fixup",
		"cached",
		"descendants",
		"phase",
		"patch",
		"after",
		"newmsg",
		"conflicted",
		"expect-index",
		"newcommit",
		"fixup-mode",
		"expect-tree",
		"rebase",
		"rollback",
		/* a move's */
		"how",
		"steps",
		"step",
		"pending",
		"remove",
		"add",
		"expect",
		"saved",
		"newnote",
		"move-todo",
		"move-index",
		"move-edit-index",
		"move-head-index",
		"target-index",
		/* a squash's */
		"squash-todo",
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(files); i++) {
		char *p = carve_path(r, files[i]);
		struct strbuf tmp = STRBUF_INIT;

		/*
		 * and what a write cut short left, ours or the lock of git
		 * writing one of our temporary indexes
		 */
		unlink(p);
		sb_addf(&tmp, "%s.tmp", p);
		unlink(tmp.buf);
		sb_reset(&tmp);
		sb_addf(&tmp, "%s.lock", p);
		unlink(tmp.buf);
		free(p);
		sb_release(&tmp);
	}
	/* the amend! messages of a squash, squash-msg-1, -2, ... */
	for (i = 1;; i++) {
		char name[32], *p;
		int gone;

		snprintf(name, sizeof(name), "squash-msg-%zu", i);
		p = carve_path(r, name);
		gone = unlink(p);
		free(p);
		if (gone)
			break;
	}
	rmdir(r->carve_dir);
}

void carve_refs_migrate(struct repo *r)
{
	static const char *const pairs[][2] = {
		{ SPLIT_REF_OLD, SPLIT_REF },
		{ WT_REF_OLD, WT_REF },
	};
	size_t i;

	/* only the worktree whose operation it is (its state files) */
	if (!git_path_exists(r, "carve/msg") && !git_path_exists(r, "carve/op"))
		return;
	for (i = 0; i < 2; i++) {
		char *old = repo_rev(r, pairs[i][0]);

		if (old && repo_rev_status(r, pairs[i][1]) &&
		    !git_quiet(r, "update-ref", pairs[i][1], old, NULL))
			git_quiet(r, "update-ref", "-d", pairs[i][0], old,
				  NULL);
		free(old);
	}
	sb_reset(&r->err);
}

bool op_in_progress(struct repo *r)
{
	return git_path_exists(r, "carve/op") ||
	       git_path_exists(r, "carve/msg");
}

char *state_str(struct repo *r, const char *name)
{
	struct strbuf sb = STRBUF_INIT;

	if (state_read(r, name, &sb)) {
		sb_release(&sb);
		return NULL;
	}
	sb_rtrim(&sb);
	return sb_detach(&sb);
}

int state_str_set(struct repo *r, const char *name, const char *value)
{
	if (!value)
		return repo_err(r, "cannot record %s", name);
	return state_write(r, name, value, strlen(value));
}
