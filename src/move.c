// SPDX-License-Identifier: MIT
/* Changes to any commit of the series, through one rebase (move.h). */
#include "move.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diff.h"
#include "index.h"
#include "patch.h"
#include "series.h"
#include "split.h"
#include "message.h"
#include "status.h"
#include "opstate.h"
#include "rebase.h"
#include "cdiff.h"

#define WT_KEPT_MSG "the lines stay in your work tree (your edits touch them)"
#define WHOLE_COMMIT_MSG \
	"that is all the commit changes: nothing would be left of it"
#define TAKEN_OUT_MSG "taken out of %.7s into the work tree"

/*
 * The state in $GIT_DIR/carve while a move runs.
 *
 *   op        "move"
 *   tip       HEAD before it
 *   how       out, drop, into, new or reword
 *   path      the file, not for reword
 *   steps     one line per break of the todo, "amend-remove", "amend-add",
 *             "new" or "reword", in the order the rebase gets there
 *   step      how many of them are done
 *   pending   a step whose patch conflicted, finished once resolved
 *   remove    the patch that takes the lines out, applied reversed
 *   add       the patch that puts them in, applied as it is
 *   newmsg    the message of a new commit or a reword, "" for the editor
 *   newnote   the lines and where from, for the editor of a new commit
 *   expect    the tip's tree the move must end with, "" when not known
 *   conflicted  set once a conflict was resolved by hand, which is then
 *             not checked
 *   saved     "1" when the work tree was saved (WT_REF)
 */

bool move_in_progress(struct repo *r)
{
	struct strbuf op = STRBUF_INIT;
	bool res;

	res = !state_read(r, "op", &op) && !strncmp(sb_str(&op), "move", 4);

	sb_release(&op);
	return res;
}

/*
 * git apply of the state file name to the index and work tree, 3-way where
 * the lines' surroundings changed. Returns 1 when it left conflicts to
 * resolve.
 */
static int apply_state(struct repo *r, const char *name, bool reverse)
{
	struct strbuf patch = STRBUF_INIT, e = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	struct run_opts o = { 0 };
	int rc;

	if (state_read(r, name, &patch))
		return repo_err(r, "the move lost its patch (%s)", name);
	/*
	 * Stat data that only looks changed, as on another file system or
	 * in a copy, would make git apply --index refuse. Refresh it
	 * first.
	 */
	git_status_of(r, "update-index", "-q", "--refresh", NULL);
	o.in = patch.buf;
	o.inlen = patch.len;
	argv_pushl(&a, GIT_APPLY_PLAIN, "apply", "--index", "--3way", NULL);
	if (reverse)
		argv_push(&a, "--reverse");
	argv_pushl(&a, "--whitespace=nowarn", "-", NULL);
	rc = run_git(&a, &o, NULL, &e);
	if (rc && count_conflicts(r) > 0)
		rc = 1;
	else if (rc && (strstr(sb_str(&e), "does not apply") ||
			strstr(sb_str(&e), "lacks the necessary blob")))
		/* no 3-way either, the lines' context is not there */
		rc = repo_err(r,
			      "the lines do not apply to the commit there: "
			      "the lines around them differ (a commit between "
			      "changes them); nothing changed");
	else if (rc)
		rc = git_fail(r, "git apply", &e);
	argv_clear(&a);
	sb_release(&patch);
	sb_release(&e);
	return rc;
}

/* Commit a step's result, as an amend, a new commit or a new message. */
static int commit_step(struct repo *r, const char *step)
{
	struct commit_opts co = { 0 };
	char *note = NULL;
	char *msg;
	int rc;

	if (!strncmp(step, "amend", 5))
		return git_env(r, git_quiet_editor, NULL, "commit", "-q",
			       "--amend", "--no-edit", "--allow-empty", NULL) ?
			       -1 :
			       0;
	msg = state_str(r, "newmsg");
	co.message = msg && *msg ? msg : NULL;
	if (!strcmp(step, "new")) {
		note = state_str(r, "newnote");
		co.note = note && *note ? note : NULL;
	}
	co.quiet = true;
	co.signoff = config_signoff(r);
	co.amend = !strcmp(step, "reword");
	rc = carve_commit(r, &co);
	free(msg);
	free(note);
	return rc;
}

/*
 * The step at the break the rebase stands at. Returns 1 when it stopped
 * there.
 */
static int do_step(struct repo *r, const char *step)
{
	int rc = 0;

	if (!strcmp(step, "amend-remove"))
		rc = apply_state(r, "remove", true);
	else if (!strcmp(step, "amend-add") || !strcmp(step, "new"))
		rc = apply_state(r, "add", false);
	if (rc == 1) {
		state_str_set(r, "pending", step);
		state_str_set(r, "conflicted", "1");
		repo_err(r,
			 "the lines conflict with the commit there: resolve, "
			 "stage, then {finish} to continue or {abort} to "
			 "give up");
		return 1;
	}
	if (rc)
		return -1;
	if (!strcmp(step, "amend-remove")) {
		/* a commit left empty is no move but a drop */
		char *t = git_line(r, "write-tree", NULL);
		char *p = repo_rev(r, "HEAD^^{tree}");
		bool all = t && p && !strcmp(t, p);

		free(t);
		free(p);
		if (all)
			return repo_err(r,
					"those are all the commit's changes: "
					"nothing would be left of it");
	}
	return commit_step(r, step);
}

/* The last command the rebase did was a "break" (one of ours). */
static bool at_break(struct repo *r)
{
	char *p = repo_git_path_dup(r, "rebase-merge/done");
	struct strbuf done = STRBUF_INIT;
	bool res = false;

	if (p && !read_file(p, &done)) {
		const char *l;

		sb_rtrim(&done);
		l = strrchr(sb_str(&done), '\n');
		l = l ? l + 1 : sb_str(&done);
		res = !strcmp(l, "break") || !strcmp(l, "b");
	}
	free(p);
	sb_release(&done);
	return res;
}

/* The tip's tree the move should make, into *out ("" : not known). */
static void expected_tree(struct repo *r, const char *tip, int change,
			  struct strbuf *out)
{
	struct strbuf envs = STRBUF_INIT, patch = STRBUF_INIT, e = STRBUF_INIT;
	char *idx = carve_path(r, "move-index");
	const char *env[] = { NULL, NULL };
	struct argv a = ARGV_INIT;
	struct run_opts o = { 0 };

	sb_reset(out);
	unlink(idx);
	sb_addf(&envs, "GIT_INDEX_FILE=%s", idx);
	env[0] = envs.buf;
	if (git_env(r, env, NULL, "read-tree", tip, NULL))
		goto out;
	if (change) {
		/* the lines gone from the tip (1), or changed in it (2) */
		if (state_read(r, change == 1 ? "remove" : "add", &patch))
			goto out;
		o.env = env;
		o.in = patch.buf;
		o.inlen = patch.len;
		argv_pushl(&a, "apply", "--cached", NULL);
		if (change == 1)
			argv_push(&a, "--reverse");
		argv_pushl(&a, "--whitespace=nowarn", "-", NULL);
		if (run_git(&a, &o, NULL, &e))
			goto out;
	}
	git_env(r, env, out, "write-tree", NULL);
	sb_rtrim(out);
out:
	sb_reset(&r->err);
	unlink(idx);
	free(idx);
	argv_clear(&a);
	sb_release(&envs);
	sb_release(&patch);
	sb_release(&e);
}

/* Back to where the move started, the work tree as it was. */
/*
 * Back to the tip with your files as they were. It stops at the first
 * step that fails and keeps the state, and move_abort runs these steps
 * again. When it works, the error that made it roll back stays.
 */
static int roll_back(struct repo *r)
{
	char *tip = state_str(r, "tip"), *saved = state_str(r, "saved");
	char *err = xstrdup(sb_str(&r->err));
	int rc = 0;

	if ((rebase_in_progress(r) && rebase_abort(r)) ||
	    (tip && git_quiet(r, "reset", "-q", "--hard", tip, NULL)) ||
	    (saved && !strcmp(saved, "1") && restore_worktree(r, true))) {
		rc = rollback_failed(r, tip, true);
	} else {
		state_remove(r);
		sb_reset(&r->err);
		sb_adds(&r->err, err);
	}
	free(err);
	free(tip);
	free(saved);
	return rc;
}

/* The rebase is done. Check the result, then put the work tree back. */
static int finish(struct repo *r)
{
	char *how = state_str(r, "how"), *expect = state_str(r, "expect"),
	     *conflicted = state_str(r, "conflicted"),
	     *saved = state_str(r, "saved"), *tree = NULL;
	struct strbuf patch = STRBUF_INIT;
	int rc = -1;

	tree = repo_rev(r, "HEAD^{tree}");
	if (expect && *expect && !conflicted && tree && strcmp(tree, expect)) {
		if (!roll_back(r))
			repo_err(r, "the result is not what the move should "
				    "make (the tip's tree differs); rolled "
				    "back");
		goto out;
	}
	if (saved && !strcmp(saved, "1")) {
		/*
		 * Your files exactly as they were, and the index as the
		 * new HEAD plus what you had staged. The old index has the
		 * old commits' lines.
		 */
		struct strbuf staged = STRBUF_INIT, spec = STRBUF_INIT;
		char *tip = state_str(r, "tip");
		struct argv a = ARGV_INIT;

		sb_addf(&spec, "%s^2", WT_REF);
		argv_push(&a, "--literal-pathspecs");
		push_diff_args(&a);
		argv_pushl(&a, "--binary", "--full-index", tip ? tip : "HEAD",
			   spec.buf, NULL);
		run_git(&a, NULL, &staged, NULL);
		argv_clear(&a);
		if (restore_worktree(r, false)) {
			repo_warn(r, "could not put the work tree back; it is "
				     "saved in " WT_REF);
		} else if (staged.len) {
			struct run_opts o = { .in = staged.buf,
					      .inlen = staged.len };

			argv_pushl(&a, "apply", "--cached", "--3way",
				   "--whitespace=nowarn", "-", NULL);
			if (run_git(&a, &o, NULL, NULL))
				repo_warn(r, "what you had staged is unstaged "
					     "now (it did not fit the new "
					     "commits)");
			argv_clear(&a);
		}
		free(tip);
		sb_release(&staged);
		sb_release(&spec);
	} else if (how && !strcmp(how, "out")) {
		/* the lines back in the work tree, unstaged */
		char *tip = state_str(r, "tip");

		if (tip && git_quiet(r, "restore", "--source", tip,
				     "--worktree", "--", ".", NULL))
			repo_warn(r,
				  "the lines are in %.7s; the work tree could "
				  "not get them back",
				  tip);
		free(tip);
	}
	if (how && !strcmp(how, "edit") && saved && !strcmp(saved, "1") &&
	    !state_read(r, "add", &patch)) {
		struct run_opts o = { .in = patch.buf, .inlen = patch.len };
		/* your edits are kept, and the lines change in the files too */
		struct argv a = ARGV_INIT;

		argv_pushl(&a, "apply", "--whitespace=nowarn", "-", NULL);
		if (run_git(&a, &o, NULL, NULL))
			repo_warn(r,
				  "your work tree has the lines as they were "
				  "(your edits touch them)");
		argv_clear(&a);
		sb_reset(&patch);
	}
	if (how && !strcmp(how, "drop") && saved && !strcmp(saved, "1") &&
	    !state_read(r, "remove", &patch)) {
		struct run_opts o = { .in = patch.buf, .inlen = patch.len };
		/* your edits are kept, and the lines leave the files too */
		struct argv a = ARGV_INIT;

		argv_pushl(&a, "apply", "--reverse", "--whitespace=nowarn", "-",
			   NULL);
		if (run_git(&a, &o, NULL, NULL))
			repo_warn(r, WT_KEPT_MSG);
		argv_clear(&a);
	}
	state_remove(r);
	rc = 0;
out:
	free(how);
	free(expect);
	free(conflicted);
	free(saved);
	free(tree);
	sb_release(&patch);
	return rc;
}

/* Run the rebase on, doing a step at every break, until it is done or stops. */
static int run(struct repo *r)
{
	for (;;) {
		struct strbuf steps = STRBUF_INIT;
		char *n = NULL, *step = NULL;
		const char *p;
		long k, i;
		int rc;

		if (!rebase_in_progress(r))
			return finish(r);
		if (count_conflicts(r) > 0) {
			state_str_set(r, "conflicted", "1");
			repo_err(r,
				 "a later commit conflicts with the change: "
				 "resolve, stage, then {finish} to continue or "
				 "{abort} to give up");
			return 1;
		}
		if (!at_break(r)) {
			repo_err(r, "the rebase stopped (git status says why): "
				    "{finish} to continue, {abort} to give up");
			return 1;
		}
		state_read(r, "steps", &steps);
		n = state_str(r, "step");
		k = number_or(n, 0);
		free(n);
		/* the k-th line of steps */
		p = sb_str(&steps);
		for (i = 0; i < k && p; i++) {
			p = strchr(p, '\n');
			p = p ? p + 1 : NULL;
		}
		if (p && *p)
			step = xstrndup(p, strcspn(p, "\n"));
		sb_release(&steps);
		if (!step) {
			/* a break of the user's is not ours to do */
			rc = rebase_continue(r);
		} else {
			char num[32];

			snprintf(num, sizeof(num), "%ld", k + 1);
			state_str_set(r, "step", num);
			rc = do_step(r, step);
			free(step);
			if (rc)
				return rc;
			rc = rebase_continue(r);
		}
		if (rc < 0)
			return -1;
	}
}

/*
 * The todo picks every commit from base on, with a break after each commit
 * a step happens at, in order.
 */
static int build_todo(struct repo *r, const char *base, const char *const *at,
		      const char *const *what, size_t n, struct strbuf *todo,
		      struct strbuf *steps)
{
	struct strbuf ids = STRBUF_INIT, range = STRBUF_INIT;
	const char *pos, *end, *l;
	size_t len, i;
	int rc = -1;

	if (base)
		sb_addf(&range, "%s..HEAD", base);
	else
		sb_adds(&range, "HEAD");
	if (list_commits(r, range.buf, &ids))
		goto out;
	pos = sb_str(&ids);
	end = pos + ids.len;
	while ((l = next_line(&pos, end, &len))) {
		sb_addf(todo, "pick %.*s\n", (int)len, l);
		for (i = 0; i < n; i++) {
			if (strlen(at[i]) != len || strncmp(at[i], l, len))
				continue;
			sb_adds(todo, "break\n");
			sb_addf(steps, "%s\n", what[i]);
		}
	}
	rc = 0;
out:
	sb_release(&ids);
	sb_release(&range);
	return rc;
}

/* Start the rebase with our todo (it stops at the first break). */
static int start_rebase(struct repo *r, const char *base, const char *todo)
{
	struct strbuf ed = STRBUF_INIT, e = STRBUF_INIT;
	char *file = carve_path(r, "move-todo");
	int rc;

	if (!todo || state_write(r, "move-todo", todo, strlen(todo))) {
		free(file);
		return -1;
	}
	/* git runs it as sh -c '<editor> "$@"': our todo over its own */
	sb_addf(&ed, "cp '%s'", file);
	rc = rebase_start(r, base, ed.buf, false, &e);
	if (rc && !rebase_in_progress(r))
		rc = git_fail(r, "git rebase", &e);
	else
		rc = 0;
	sb_release(&ed);
	sb_release(&e);
	free(file);
	return rc;
}

/* Where commit is in HEAD's history, 0 for HEAD, 1 for HEAD^ and so on. */
static long depth_of(struct repo *r, const char *commit)
{
	struct strbuf range = STRBUF_INIT, ids = STRBUF_INIT;
	const char *p;
	long d = -1;

	/* the series checked already lists them, without git */
	sb_addf(&range, "%s..HEAD", commit);
	if (!list_commits(r, range.buf, &ids))
		for (d = 0, p = sb_str(&ids); (p = strchr(p, '\n')); p++)
			d++;
	sb_release(&range);
	sb_release(&ids);
	return d;
}

int commit_region(struct repo *r, const char *commit, const char *path,
		  const char *sel, struct region *rg, struct strbuf *blob)
{
	struct strbuf err = STRBUF_INIT, spec = STRBUF_INIT;
	struct fdiff *fd = NULL;
	const char *why;
	int rc = -1, n;

	if (load_commit_diff(r, commit, path, NULL, &fd))
		goto out;
	if (!fd) {
		repo_err(r, "%.7s does not change %s", commit, path);
		goto out;
	}
	if (!fdiff_line_ops(fd, &why)) {
		repo_err(r, "%s: %s", path, why);
		goto out;
	}
	n = sel_apply(fd, sel, &err);
	if (n < 0 || (!n && !err.len)) {
		repo_err(r, "%s", err.len ? err.buf : "nothing selected");
		goto out;
	}
	sb_addf(&spec, "%s:%s", commit, path);
	if (git_out(r, blob, "cat-file", "blob", spec.buf, NULL))
		goto out;
	/* the lines as the commit has them, the "staged" side of its diff */
	if (staged_region(fd, true, sb_str(blob), blob->len, rg, &err)) {
		repo_err(r, "%s", err.buf);
		goto out;
	}
	rc = 0;
out:
	if (fd) {
		fdiff_free(fd);
		free(fd);
	}
	sb_release(&err);
	sb_release(&spec);
	return rc;
}

/* The patch that makes path in commit into content (full blob ids). */
static int edit_patch(struct repo *r, const char *commit, const char *path,
		      const char *content, size_t len, struct strbuf *patch)
{
	struct strbuf blob = STRBUF_INIT, ls = STRBUF_INIT, envs = STRBUF_INIT,
		      tree = STRBUF_INIT, info = STRBUF_INIT, e = STRBUF_INIT;
	char *idx = carve_path(r, "move-edit-index"), mode[16] = "100644";
	const char *env[] = { NULL, NULL };
	struct argv a = ARGV_INIT;
	int rc = -1;

	if (git_in(r, content, len, &blob, "hash-object", "-w", "--no-filters",
		   "--stdin", NULL))
		goto out;
	sb_rtrim(&blob);
	if (!git_out(r, &ls, "ls-tree", commit, "--", path, NULL) && ls.len)
		sscanf(ls.buf, "%15s", mode);
	if (mkdir(r->carve_dir, 0777) && errno != EEXIST) {
		repo_err(r, "cannot create %s", r->carve_dir);
		goto out;
	}
	unlink(idx);
	sb_addf(&envs, "GIT_INDEX_FILE=%s", idx);
	env[0] = envs.buf;
	sb_addf(&info, "%s,%s,%s", mode, blob.buf, path);
	if (git_env(r, env, NULL, "read-tree", commit, NULL) ||
	    git_env(r, env, NULL, "update-index", "--cacheinfo", info.buf,
		    NULL) ||
	    git_env(r, env, &tree, "write-tree", NULL))
		goto out;
	sb_rtrim(&tree);
	argv_push(&a, "--literal-pathspecs");
	push_diff_args(&a);
	argv_pushl(&a, "--binary", "--full-index", commit, tree.buf, "--", path,
		   NULL);
	if (run_git(&a, NULL, patch, &e)) {
		git_fail(r, "git diff", &e);
		goto out;
	}
	if (!patch->len) {
		repo_err(r, "nothing changed");
		goto out;
	}
	rc = 0;
out:
	unlink(idx);
	free(idx);
	argv_clear(&a);
	sb_release(&blob);
	sb_release(&ls);
	sb_release(&envs);
	sb_release(&tree);
	sb_release(&info);
	sb_release(&e);
	return rc;
}

/*
 * HEAD itself needs no rebase for lines taken out or dropped or for a new
 * message, which also covers where a rebase of yours stopped. The commit
 * is made from a temporary index, since yours may hold other staged
 * changes, and then the lines leave your index too. The work tree keeps
 * them (out) or loses them.
 */
static int move_head(struct repo *r, const struct move_req *m, const char *head,
		     struct strbuf *msg)
{
	struct strbuf rem = STRBUF_INIT, envs = STRBUF_INIT, e = STRBUF_INIT;
	const char *env[] = { NULL, GIT_EDITOR_QUIET, NULL };
	char *idx = carve_path(r, "move-head-index");
	struct argv a = ARGV_INIT;
	struct run_opts o = { 0 };
	char name[8];
	int rc = -1;

	if (m->how == MOVE_REWORD) {
		argv_pushl(&a, "commit", "-q", "--amend", "--only", NULL);
		if (m->co && m->co->message)
			argv_pushl(&a, "-m", m->co->message, NULL);
		o.tty = r->tty && !(m->co && m->co->message);
		rc = run_git(&a, &o, NULL, o.tty ? NULL : &e);
		if (rc)
			git_fail(r, "git commit", &e);
		else if (msg)
			sb_addf(msg, "%.7s has its new message", head);
		goto out;
	}
	snprintf(name, sizeof(name), "%.7s", head);
	if (commit_patch(r, head, name, m->path, m->sel, PATCH_UNSTAGE, &rem,
			 NULL))
		goto out;
	if (mkdir(r->carve_dir, 0777) && errno != EEXIST) {
		repo_err(r, "cannot create %s", r->carve_dir);
		goto out;
	}
	unlink(idx);
	sb_addf(&envs, "GIT_INDEX_FILE=%s", idx);
	env[0] = envs.buf;
	o.env = env;
	o.in = rem.buf;
	o.inlen = rem.len;
	argv_pushl(&a, "apply", "--cached", "--reverse", "--whitespace=nowarn",
		   "-", NULL);
	if (git_env(r, env, NULL, "read-tree", "HEAD", NULL) ||
	    run_git(&a, &o, NULL, &e)) {
		git_fail(r, "git apply", &e);
		goto out;
	}
	argv_clear(&a);
	{
		char *ptree = repo_rev(r, "HEAD^^{tree}");
		/*
		 * the temporary index's tree, when not all of HEAD's
		 * changes are gone
		 */
		struct strbuf t = STRBUF_INIT;
		bool all;

		sb_reset(&r->err);
		git_env(r, env, &t, "write-tree", NULL);
		sb_rtrim(&t);
		all = ptree && !strcmp(sb_str(&t), ptree);
		sb_release(&t);
		free(ptree);
		if (all) {
			repo_err(r,
				 "that is all HEAD changes: nothing would be "
				 "left of it");
			goto out;
		}
	}
	if (git_env(r, env, NULL, "commit", "-q", "--amend", "--no-edit",
		    "--no-verify", "--allow-empty", NULL))
		goto out;
	/* your index without the lines too, and the work tree as you choose */
	argv_pushl(&a, "apply", "--cached", "--reverse", "--whitespace=nowarn",
		   "-", NULL);
	o.env = NULL;
	if (run_git(&a, &o, NULL, NULL))
		repo_warn(r, "your index still has the lines (your staged "
			     "changes touch them)");
	argv_clear(&a);
	if (m->how == MOVE_DROP) {
		argv_pushl(&a, "apply", "--reverse", "--whitespace=nowarn", "-",
			   NULL);
		if (run_git(&a, &o, NULL, NULL))
			repo_warn(r, WT_KEPT_MSG);
	}
	if (msg)
		sb_addf(msg,
			m->how == MOVE_DROP ? "dropped from %.7s" :
					      TAKEN_OUT_MSG,
			head);
	rc = 0;
out:
	unlink(idx);
	free(idx);
	argv_clear(&a);
	sb_release(&rem);
	sb_release(&envs);
	sb_release(&e);
	return rc;
}

/*
 * A move in a rebase of yours, where it stopped. git cannot start a rebase
 * inside it, and the move runs in a worktree of its own, checked out at
 * HEAD with no rebase there. HEAD here then points at the result. Moving
 * lines between commits or into a new one keeps the tip's tree. That is
 * checked, and your rebase goes on from exactly the tree it stopped at.
 */
static int move_in_rebase(struct repo *r, const struct move_req *m,
			  const char *head, struct strbuf *msg)
{
	struct strbuf path = STRBUF_INIT, spec = STRBUF_INIT;
	char *now = NULL, *t0 = NULL, *t1 = NULL;
	struct repo w = { .tty = r->tty };
	int rc = -1;

	if (m->how != MOVE_INTO && m->how != MOVE_NEW && m->how != MOVE_REWORD)
		return repo_err(r, YOUR_REBASE_MSG);
	if (count_conflicts(r) > 0)
		return repo_err(r,
				"resolve the conflicts of your rebase first");
	if (r->carve_dir[0] != '/')
		sb_addf(&path, "%s/", r->top);
	sb_addf(&path, "%s", r->carve_dir);
	if (mkdir(path.buf, 0777) && errno != EEXIST) {
		repo_err(r, "cannot create %s: %s", path.buf, strerror(errno));
		goto out;
	}
	sb_adds(&path, "/rebase-move");
	/* remove one left by a crash first */
	git_quiet(r, "worktree", "remove", "--force", path.buf, NULL);
	git_quiet(r, "worktree", "prune", NULL);
	sb_reset(&r->err);
	if (git_quiet(r, "worktree", "add", "-q", "--detach", path.buf, head,
		      NULL))
		goto out;
	if (chdir(path.buf) || repo_open(&w)) {
		repo_err(r, "cannot use the worktree %s: %s", path.buf,
			 sb_str(&w.err));
		goto remove;
	}
	rc = move_lines(&w, m, msg);
	if (rc == 1) {
		move_abort(&w);
		rc = repo_err(r, "the move conflicts on its way to HEAD: "
				 "nothing changed; move the lines once your "
				 "rebase is done");
	} else if (rc) {
		repo_err(r, "%s", sb_str(&w.err));
	} else {
		now = repo_rev(&w, "HEAD");
	}
remove:
	if (chdir(r->top))
		rc = repo_err(r, "cannot chdir to %s: %s", r->top,
			      strerror(errno));
	if (!rc && now) {
		sb_addf(&spec, "%s^{tree}", head);
		t0 = repo_rev(r, spec.buf);
		sb_reset(&spec);
		sb_addf(&spec, "%s^{tree}", now);
		t1 = repo_rev(r, spec.buf);
		/*
		 * The move checks the tip itself. HEAD moves only if the
		 * check passed.
		 */
		if (!t0 || !t1 || strcmp(t0, t1))
			rc = repo_err(r, "the move would change the tree your "
					 "rebase stopped at: nothing changed");
		else if (git_quiet(r, "update-ref", "-m",
				   "git carve: move in "
				   "a rebase",
				   "--no-deref", "HEAD", now, head, NULL))
			rc = -1;
		else
			rc = rebase_amend_follow(r, head, now);
	} else if (!rc) {
		rc = repo_err(r, "the move left no HEAD");
	}
	git_quiet(r, "worktree", "remove", "--force", path.buf, NULL);
out:
	repo_release(&w);
	sb_release(&path);
	sb_release(&spec);
	free(now);
	free(t0);
	free(t1);
	return rc;
}

/*
 * The general case is one rebase from the parent of the oldest commit
 * touched, with a break where each step is done. move_lines() takes the
 * ways without a rebase first.
 */
static int move_by_rebase(struct repo *r, const struct move_req *m,
			  const char *head, struct strbuf *msg)
{
	struct strbuf rem = STRBUF_INIT, add = STRBUF_INIT, todo = STRBUF_INIT,
		      steps = STRBUF_INIT, expect = STRBUF_INIT,
		      note = STRBUF_INIT;
	static const char *const hows[] = { "out", "drop",   "into",
					    "new", "reword", "edit" };
	char *full = NULL, *dest = NULL, *base = NULL;
	const char *at[2], *what[2];
	size_t nsteps = 0;
	long dc, dd = -1;
	int rc = -1;
	bool dirty;

	if (check_series_commit(r, m->commit, &full))
		goto out;
	dc = depth_of(r, full);
	if (m->how == MOVE_INTO || m->how == MOVE_NEW) {
		if (!m->dest || check_series_commit(r, m->dest, &dest))
			goto out;
		dd = depth_of(r, dest);
		if (m->how == MOVE_INTO && !strcmp(dest, full)) {
			repo_err(r, "the lines are in %.7s already", full);
			goto out;
		}
	}
	/* the patches, checked before anything changes */
	if (m->how == MOVE_EDIT) {
		if (edit_patch(r, full, m->path, m->content, m->len, &add))
			goto out;
	} else if (m->how != MOVE_REWORD) {
		char name[8];

		snprintf(name, sizeof(name), "%.7s", full);
		if (commit_patches(r, full, name, m->path, m->sel, &rem, &add))
			goto out;
	}
	/* the steps, in the order the rebase gets to them (oldest first) */
	if (m->how == MOVE_REWORD || m->how == MOVE_EDIT) {
		at[nsteps] = full;
		what[nsteps++] = m->how == MOVE_EDIT ? "amend-add" : "reword";
	} else if (m->how == MOVE_OUT || m->how == MOVE_DROP) {
		at[nsteps] = full;
		what[nsteps++] = "amend-remove";
	} else {
		const char *dstep = m->how == MOVE_INTO ? "amend-add" : "new";

		if (dd > dc) {
			/*
			 * When an older commit gets them, the commit they
			 * come from no longer adds them once it is picked
			 * again, and there is nothing to take out there.
			 */
			at[nsteps] = dest;
			what[nsteps++] = dstep;
		} else {
			/* out here, into the commit there (or a new one) */
			at[nsteps] = full;
			what[nsteps++] = "amend-remove";
			at[nsteps] = dest;
			what[nsteps++] = dstep;
		}
	}
	/* the editor of a new commit says which lines, and from where */
	if (m->how == MOVE_NEW) {
		char *subj = repo_commit_subject(r, full);

		sb_addf(&note,
			"Lines moved out of %.7s \"%s\",\ninto this new "
			"commit:\n",
			full, subj ? subj : "");
		free(subj);
		commit_note_patch(&note, sb_str(&add));
	}
	/* from the parent of the oldest commit touched */
	base = parent_of(r, (m->how == MOVE_INTO || m->how == MOVE_NEW) &&
					    dd > dc ?
				    dest :
				    full);
	if (build_todo(r, base, at, what, nsteps, &todo, &steps))
		goto out;
	dirty = !tree_is_clean(r, NULL);
	if (state_str_set(r, "op", "move") || state_str_set(r, "tip", head) ||
	    state_str_set(r, "how", hows[m->how]) ||
	    state_write(r, "steps", steps.buf, steps.len) ||
	    state_str_set(r, "step", "0") ||
	    state_write(r, "remove", rem.buf, rem.len) ||
	    state_write(r, "add", add.buf, add.len) ||
	    state_str_set(r, "newmsg",
			  m->co && m->co->message ? m->co->message : "") ||
	    state_write(r, "newnote", note.buf, note.len) ||
	    state_str_set(r, "saved", dirty ? "1" : "0") ||
	    state_str_set(r, "target", full)) {
		state_remove(r);
		goto out;
	}
	/* what the tip must be after it, the same or without the lines */
	expected_tree(r, head,
		      m->how == MOVE_OUT || m->how == MOVE_DROP ? 1 :
		      m->how == MOVE_EDIT			? 2 :
								  0,
		      &expect);
	if (state_write(r, "expect", expect.buf, expect.len))
		goto fail;
	/* your changes out of the way, put back at the end */
	if (dirty && (save_worktree(r) < 0 ||
		      git_quiet(r, "reset", "-q", "--hard", NULL)))
		goto fail;
	if (start_rebase(r, base, todo.buf))
		goto fail;
	rc = run(r);
	if (rc < 0 && move_in_progress(r)) {
		/* an error in a step leaves nothing half done behind */
		roll_back(r);
	}
	if (!rc && msg) {
		static const char *const done[] = {
			TAKEN_OUT_MSG,
			"dropped from %.7s",
			"moved from %.7s",
			"moved from %.7s into a new commit",
			"%.7s has its new message",
			"%.7s has the lines as edited",
		};

		sb_addf(msg, done[m->how], full);
	}
	goto out;
fail:
	roll_back(r);
	rc = -1;
out:
	free(full);
	free(dest);
	free(base);
	sb_release(&rem);
	sb_release(&add);
	sb_release(&todo);
	sb_release(&steps);
	sb_release(&note);
	sb_release(&expect);
	return rc;
}

/*
 * Lines of HEAD (sel of its diff of path, NULL for all of it) into a new
 * commit right before HEAD (before) or right after it, committed as co
 * says. Without a message the editor opens, and hooks run. HEAD keeps its
 * message and author, and the tip's tree does not change. On a failure
 * HEAD is restored.
 */
static int carve_head(struct repo *r, bool before, const char *path,
		      const char *sel, const struct commit_opts *co,
		      struct strbuf *msg)
{
	char *head = NULL, *tree = NULL, *htree = NULL, *ptree = NULL;
	struct strbuf patch = STRBUF_INIT, what = STRBUF_INIT;
	struct strbuf note = STRBUF_INIT;
	struct commit_opts c2;
	int rc = -1, n = 0;

	if (op_in_progress(r))
		return repo_err(r, OP_BUSY_MSG);
	if (!tree_is_clean(r, NULL)) {
		refuse_dirty(r);
		goto out;
	}
	head = repo_rev(r, "HEAD");
	htree = repo_rev(r, "HEAD^{tree}");
	ptree = repo_rev(r, "HEAD^^{tree}");
	if (!head || !htree || !ptree) {
		repo_err(r, "HEAD has no parent to carve against");
		goto out;
	}
	if (commit_patch(r, "HEAD", "the commit", path, sel,
			 before ? PATCH_STAGE : PATCH_UNSTAGE, &patch, &n))
		goto out;
	if (n)
		sb_addf(&what, "%d changed line%s of %s", n, n == 1 ? "" : "s",
			path);
	else
		sb_addf(&what, "%s", path);
	/* the editor says what this commit is, the lines and where from */
	{
		char *subj = repo_commit_subject(r, head);

		sb_addf(&note,
			"Lines carved out of %.7s \"%s\",\ninto this "
			"new commit %s it:\n",
			head, subj ? subj : "", before ? "before" : "after");
		free(subj);
		commit_note_patch(&note, sb_str(&patch));
		c2 = *co;
		c2.note = sb_str(&note);
	}
	git_status_of(r, "update-index", "-q", "--refresh", NULL);
	if (before) {
		/*
		 * The parent plus the lines, committed as a new commit,
		 * then the commit on top of it with its tree and message
		 * as they were. The work tree is the commit's all along.
		 */
		if (git_quiet(r, "reset", "-q", "--soft", "HEAD^", NULL) ||
		    git_quiet(r, "read-tree", "HEAD", NULL) ||
		    apply_patch(r, &patch, false))
			goto rollback;
		tree = git_line(r, "write-tree", NULL);
		if (!tree || !strcmp(tree, htree)) {
			repo_err(r, WHOLE_COMMIT_MSG);
			goto rollback;
		}
		if (carve_commit(r, &c2) ||
		    git_quiet(r, "read-tree", head, NULL) ||
		    git_env(r, git_quiet_editor, NULL, "commit", "-q", "-C",
			    head, NULL))
			goto rollback;
		sb_addf(msg, "carved %s into a new commit before it", what.buf);
	} else {
		/* out of the commit, then committed again on top of it */
		if (git_apply(r, &patch, APPLY_INDEX | APPLY_REVERSE))
			goto rollback;
		tree = git_line(r, "write-tree", NULL);
		if (!tree || !strcmp(tree, ptree)) {
			repo_err(r, WHOLE_COMMIT_MSG);
			goto rollback;
		}
		if (git_env(r, git_quiet_editor, NULL, "commit", "-q",
			    "--amend", "--no-edit", NULL) ||
		    git_apply(r, &patch, APPLY_INDEX) || carve_commit(r, &c2))
			goto rollback;
		sb_addf(msg, "carved %s into a new commit after it", what.buf);
	}
	{
		/* in your rebase, its edit stop follows HEAD */
		char *now = repo_rev(r, "HEAD");

		if (now)
			rebase_amend_follow(r, head, now);
		free(now);
	}
	rc = 0;
	goto out;
rollback:
	/* the tracked files were clean, and the commit is exactly as it was */
	if (git_quiet(r, "reset", "-q", "--hard", head, NULL))
		repo_warn(r, "could not restore %.12s: git reset --hard %s",
			  head, head);
	rc = -1;
out:
	sb_release(&patch);
	sb_release(&what);
	sb_release(&note);
	free(head);
	free(tree);
	free(htree);
	free(ptree);
	return rc;
}

int move_lines(struct repo *r, const struct move_req *m, struct strbuf *msg)
{
	char *full = NULL, *head = NULL;
	int rc = -1;

	/* lines out of HEAD, dropped or reworded, straight without a rebase */
	head = repo_rev(r, "HEAD");
	full = resolve_commit(r, m->commit);
	sb_reset(&r->err);
	if (head && full && !strcmp(head, full) &&
	    (m->how == MOVE_OUT || m->how == MOVE_DROP ||
	     m->how == MOVE_REWORD)) {
		if (move_in_progress(r) || count_conflicts(r) > 0) {
			repo_err(r, OP_BUSY_MSG);
			goto out;
		}
		rc = move_head(r, m, head, msg);
		if (!rc) {
			/* in your rebase, its edit stop follows HEAD */
			char *now = repo_rev(r, "HEAD");

			if (now)
				rc = rebase_amend_follow(r, head, now);
			free(now);
		}
		goto out;
	}
	if (head && full && !strcmp(head, full) && m->how == MOVE_NEW &&
	    m->dest) {
		/*
		 * a new commit right after HEAD, or right before it as a
		 * carve
		 */
		char *d = resolve_commit(r, m->dest);
		char *parent = parent_of(r, head);
		bool before;
		bool after;

		after = d && !strcmp(d, head);
		before = d && parent && !strcmp(d, parent);

		free(d);
		free(parent);
		sb_reset(&r->err);
		if (after || before) {
			struct commit_opts co = { 0 };

			if (m->co)
				co = *m->co;
			rc = carve_head(r, before, m->path, m->sel, &co, msg);
			goto out;
		}
	}
	if (head && foreign_rebase(r)) {
		rc = move_in_rebase(r, m, head, msg);
		goto out;
	}
	rc = move_by_rebase(r, m, head, msg);
out:
	free(full);
	free(head);
	return rc;
}

int move_continue(struct repo *r)
{
	char *pending;
	int rc;

	if (!move_in_progress(r))
		return repo_err(r, "no move in progress");
	if (count_conflicts(r) > 0)
		return repo_err(r,
				"resolve the conflicts and stage them first");
	pending = state_str(r, "pending");
	if (pending && *pending) {
		/* the step whose lines conflicted is resolved, commit it */
		rc = commit_step(r, pending);
		free(pending);
		if (rc)
			return -1;
		state_str_set(r, "pending", "");
		rc = rebase_continue(r);
		if (rc < 0)
			return -1;
	} else {
		free(pending);
		/* git commits a pick that conflicted and was resolved */
		if (rebase_in_progress(r) && !at_break(r)) {
			state_str_set(r, "conflicted", "1");
			if (rebase_continue(r) < 0)
				return -1;
		}
	}
	return run(r);
}

int move_abort(struct repo *r)
{
	if (!move_in_progress(r))
		return repo_err(r, "no move in progress");
	return roll_back(r);
}
