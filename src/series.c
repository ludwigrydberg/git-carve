// SPDX-License-Identifier: MIT
/*
 * fixup-to, defer and squash. Their rebases are "git rebase -i" with a
 * scripted sequence editor (rebase.c), and user configuration and hooks
 * apply as usual. Message editors are suppressed with GIT_EDITOR=true.
 */
#include "series.h"
#include "status.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "patch.h"
#include "rename.h"
#include "split.h"
#include "message.h"
#include "opstate.h"
#include "rebase.h"
#include "cdiff.h"

#define NOTHING_TO_MOVE_MSG "nothing staged to move into %.7s"
#define NO_STAGED_CHANGE_MSG "cannot compute the staged change"

static char *subject_of(struct repo *r, const char *commit)
{
	char *s = repo_commit_subject(r, commit);

	return s ? s : xstrdup("");
}

/* ---- rebase ----------------------------------------------------------- */

/* ---- diffs of commits -------------------------------------------------- */

/* ---- state -------------------------------------------------------------- */

int series_state_load(struct repo *r, struct series_state *st)
{
	char *op;

	carve_refs_migrate(r);
	op = state_str(r, "op");
	memset(st, 0, sizeof(*st));
	st->rebasing = rebase_in_progress(r);
	st->conflicts = st->rebasing ? count_conflicts(r) : 0;
	if (op && !strcmp(op, "fixup"))
		st->op = OP_FIXUP;
	else if (op && !strcmp(op, "defer"))
		st->op = OP_DEFER;
	else if (op && !strcmp(op, "move"))
		st->op = OP_MOVE;
	else if (op && !strcmp(op, "squash"))
		st->op = OP_SQUASH;
	free(op);
	if (st->op != OP_NONE) {
		st->target = state_str(r, "target");
		st->subject = subject_of(r, st->target ? st->target : "HEAD");
	} else if (foreign_rebase(r)) {
		st->foreign_rebase = true;
	}
	return 0;
}

void series_state_release(struct series_state *st)
{
	free(st->target);
	free(st->subject);
	memset(st, 0, sizeof(*st));
}

void series_status_line(const struct series_state *st, struct strbuf *out)
{
	if (st->foreign_rebase) {
		sb_adds(out, "REBASE in progress (not started by git carve): "
			     "finish it with git rebase --continue / --abort");
		return;
	}
	if (st->op == OP_NONE)
		return;
	sb_addf(out, "%s %.7s \"%s\"",
		st->op == OP_FIXUP  ? "FIXUP-TO" :
		st->op == OP_MOVE   ? "MOVE" :
		st->op == OP_SQUASH ? "SQUASH" :
				      "DEFER",
		st->target, st->subject);
	if (st->conflicts)
		sb_addf(out,
			" \xe2\x80\x94 %d conflicted file%s: resolve, "
			"stage",
			st->conflicts, st->conflicts == 1 ? "" : "s");
	else if (st->rebasing)
		sb_adds(out, " \xe2\x80\x94 stopped");
}

/* ---- fixup-to ----------------------------------------------------------- */

/* Tree of `tree` with `patch` applied, through a temporary index. */
static char *tree_plus_patch(struct repo *r, const char *commit,
			     const char *patch, size_t plen, bool zero)
{
	struct strbuf envs = STRBUF_INIT, out = STRBUF_INIT, e = STRBUF_INIT;
	struct run_opts o = { .in = patch, .inlen = plen };
	char *idx = carve_path(r, "expect-index");
	struct argv a = ARGV_INIT;
	const char *env[2];
	char *res = NULL;

	/* our directory, where no operation has made it yet */
	if (mkdir(r->carve_dir, 0777) && errno != EEXIST) {
		free(idx);
		repo_err(r, "cannot create %s", r->carve_dir);
		return NULL;
	}
	unlink(idx);
	sb_addf(&envs, "GIT_INDEX_FILE=%s", idx);
	env[0] = envs.buf;
	env[1] = NULL;
	o.env = env;
	if (git_env(r, env, NULL, "read-tree", commit, NULL))
		goto out;
	argv_pushl(&a, "apply", "--cached", "--whitespace=nowarn", NULL);
	if (zero)
		argv_push(&a, "--unidiff-zero");
	argv_push(&a, "-");
	if (run_git(&a, &o, NULL, &e))
		goto out;
	if (git_env(r, env, &out, "write-tree", NULL))
		goto out;
	sb_rtrim(&out);
	res = sb_detach(&out);
out:
	unlink(idx);
	free(idx);
	argv_clear(&a);
	sb_release(&envs);
	sb_release(&out);
	sb_release(&e);
	return res;
}

/* A deletion (in commit -> HEAD) strictly inside HEAD lines [a, b]? */
static bool gap_inside(const struct fdiff *m, int a, int b)
{
	size_t i;

	for (i = 0; m && i < m->nhunks; i++) {
		const struct hunk *h = &m->hunks[i];

		if (!h->new_count && h->new_start >= a && h->new_start < b)
			return true;
	}
	return false;
}

/*
 * Apply the staged change (relative to HEAD) to commit's tree, moving
 * its hunks from HEAD's line numbers to commit's. This is exact when the
 * changed lines are the same in commit and HEAD, which is what makes
 * "fixup into an older commit" meaningful. Returns the new tree or NULL.
 */
static char *staged_change_onto(struct repo *r, const char *commit)
{
	struct strbuf raw = STRBUF_INIT, out = STRBUF_INIT, mo = STRBUF_INIT;
	struct argv a = ARGV_INIT, b = ARGV_INIT;
	struct fdiff *files = NULL, *mf = NULL;
	size_t nfiles = 0, f, h, mn = 0;
	struct strbuf ren = STRBUF_INIT;
	struct rename *rv = NULL;
	char *tree = NULL;
	size_t nr = 0, k;

	push_diff_args(&a);
	argv_pushl(&a, "-U0", "--cached", "--full-index", "HEAD", NULL);
	if (run_git(&a, NULL, &raw, NULL) ||
	    diff_parse(sb_str(&raw), raw.len, &files, &nfiles, NULL))
		goto out;
	/* a file renamed since commit has its old name there */
	if (!git_out(r, &ren, GIT_PLAIN, "diff", "-M", "--name-status", "-z",
		     "--no-color", commit, "HEAD", "--", NULL))
		nr = parse_renames(sb_str(&ren), ren.len, false, &rv);
	/*
	 * what the commits from commit to HEAD did to those files, in one
	 * -U0 diff for all
	 */
	argv_push(&b, "--literal-pathspecs");
	push_diff_args(&b);
	argv_pushl(&b, "-U0", "-M", commit, "HEAD", "--", NULL);
	for (f = 0; f < nfiles; f++) {
		const char *path = files[f].new_path;

		if (!path)
			continue;
		argv_push(&b, path);
		for (k = 0; k < nr; k++)
			if (!strcmp(rv[k].to, path))
				argv_push(&b, rv[k].from);
	}
	if (run_git(&b, NULL, &mo, NULL) ||
	    diff_parse(sb_str(&mo), mo.len, &mf, &mn, NULL))
		goto out;
	for (f = 0; f < nfiles; f++) {
		const struct fdiff *fd = &files[f];
		const char *path = fd->new_path;
		const char *opath = path;
		struct fdiff *m = NULL;
		bool ok = true;
		size_t i;

		if (!path || fd->binary || fd->is_new || fd->is_deleted ||
		    fd->is_rename || fd->mode_change || !fd->nhunks)
			goto out;
		for (k = 0; path && k < nr; k++)
			if (!strcmp(rv[k].to, path))
				opath = rv[k].from;
		/* the file's diff since commit, under its name in HEAD */
		for (i = 0; i < mn && !m; i++)
			if (mf[i].new_path && !strcmp(mf[i].new_path, path))
				m = &mf[i];
		if (m && (m->is_new || m->is_deleted || m->binary))
			ok = false;
		sb_addf(&out, "diff --git a/%s b/%s\n--- a/%s\n+++ b/%s\n",
			opath, opath, opath, opath);
		for (h = 0; ok && h < fd->nhunks; h++) {
			const struct hunk *hk = &fd->hunks[h];
			int s = hk->old_start, c = hk->old_count, s2, x, j;

			if (c) {
				s2 = fdiff_map_line(m, s);
				for (x = s; x < s + c && ok; x++)
					if (fdiff_map_line(m, x) !=
					    s2 + (x - s))
						ok = false;
				if (gap_inside(m, s, s + c - 1))
					ok = false;
			} else {
				s2 = s ? fdiff_map_line(m, s) : 0;
			}
			if (!ok || s2 < 0) {
				ok = false;
				break;
			}
			sb_addf(&out, "@@ -%d,%d +%d,%d @@\n", s2, c,
				hk->new_start + (s2 - s), hk->new_count);
			for (j = 0; j < (int)hk->nlines; j++) {
				const struct dline *d = &hk->lines[j];

				sb_addch(&out, d->op);
				sb_add(&out, d->text, d->len);
				sb_addch(&out, '\n');
				if (d->no_eol)
					sb_adds(&out, NO_EOL_MARKER);
			}
		}
		if (!ok)
			goto out;
	}
	tree = tree_plus_patch(r, commit, out.buf, out.len, true);
out:
	fdiff_free_array(mf, mn);
	argv_clear(&b);
	sb_release(&mo);
	fdiff_free_array(files, nfiles);
	free_renames(rv, nr);
	sb_release(&ren);
	argv_clear(&a);
	sb_release(&raw);
	sb_release(&out);
	return tree;
}

static int fixup_done(struct repo *r)
{
	char *target = state_str(r, "target"), *fixup = state_str(r, "fixup");
	char *expect = state_str(r, "expect-tree"), *cached;
	char *conflicted = state_str(r, "conflicted");
	char *now = repo_rev(r, "HEAD^{tree}");
	char *tip = state_str(r, "tip");
	int rc = 0;

	cached = state_str(r, "cached");
	if (expect && now && strcmp(expect, now)) {
		if (!conflicted && tip) {
			/*
			 * Git merged cleanly, but the series does not end
			 * in "old tip + change". Typically the change
			 * touches lines the target does not have, which a
			 * later commit added, and the merge dropped it.
			 * Undo.
			 */
			char *op = state_str(r, "op");
			bool squash;

			squash = op && !strcmp(op, "squash");

			free(op);
			/* abort finishes a rollback that fails half way */
			if (state_str_set(r, "rollback", "1")) {
				rc = rollback_failed(r, tip, false);
				goto out;
			}
			if (git_quiet(r, "reset", "-q", "--hard", tip, NULL) ||
			    restore_worktree(r,
					     cached && !strcmp(cached, "1"))) {
				rc = rollback_failed(r, tip, true);
				goto out;
			}
			state_remove(r);
			if (squash) {
				rc = repo_err(r, "the squash changed the "
						 "content of the series; "
						 "nothing was changed");
				goto out;
			}
			rc = repo_err(r,
				      "cannot move the change into %.7s: "
				      "some of the selected lines are not "
				      "in it as they are in HEAD (a later "
				      "commit added or changed them); "
				      "nothing was changed",
				      target);
			goto out;
		}
		repo_warn(r,
			  "warning: after your conflict resolution the "
			  "series ends in a different tree than the old "
			  "tip plus the change (old tip: %.7s)",
			  tip);
	}
	/*
	 * After fixup-to the staged change is in the commit and the index
	 * is HEAD. After squash the index is as it was, with the same
	 * content below it.
	 */
	{
		char *op = state_str(r, "op");
		bool squash;

		squash = op && !strcmp(op, "squash");

		free(op);
		/* the series is done, and abort would undo it */
		if (restore_worktree(r, squash))
			rc = repo_err(r, "the change is in the series, but "
					 "your files could not be put back; "
					 "they are saved in " WT_REF);
	}
	state_remove(r);
out:
	free(target);
	free(fixup);
	free(tip);
	free(expect);
	free(cached);
	free(conflicted);
	free(now);
	return rc;
}

static int stopped(struct repo *r, const char *what)
{
	int n = count_conflicts(r);

	state_str_set(r, "conflicted", "1");
	repo_err(r,
		 "%s stopped with %d conflicted file%s: resolve, stage "
		 "them, then {finish} to continue or {abort} to "
		 "give up",
		 what, n, n == 1 ? "" : "s");
	return 1;
}

/*
 * Run "git rebase -i" from the target's parent with the given sequence
 * editor script, or keep the todo for NULL. Returns 0 when done, 1 when
 * stopped, or -1.
 */
static int run_rebase(struct repo *r, const char *target, const char *seq,
		      bool autosquash)
{
	char *parent = parent_of(r, target);
	struct strbuf e = STRBUF_INIT;
	int rc;

	rc = rebase_start(r, parent, seq, autosquash, &e);
	if (rebase_in_progress(r))
		rc = 1;
	else if (rc)
		rc = git_fail(r, "git rebase", &e);
	free(parent);
	sb_release(&e);
	return rc;
}

/*
 * After the rebase of a fixup-to ran (run_rebase's rc): stopped on a
 * conflict (1), not started (put back, -1), or done and checked.
 */
static int fixup_ran(struct repo *r, int rc)
{
	if (rc == 1)
		return stopped(r, "moving the change");
	if (rc < 0) {
		series_abort(r);
		return -1;
	}
	return fixup_done(r);
}

int fixup_commit(struct repo *r, const char *commit, const struct fixup_opts *o)
{
	struct strbuf msg = STRBUF_INIT, body = STRBUF_INIT;
	char *full = NULL, *subject = NULL, *path = NULL;
	bool edit = o->amend && !o->message && !o->file;
	struct run_opts ro = { .tty = true };
	struct strbuf err = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	int rc = -1;

	if (check_series_commit(r, commit, &full))
		goto out;
	if (!o->amend) {
		/* "fixup! <subject>", as git commit makes it */
		argv_push(&a, "commit");
		if (o->quiet)
			argv_push(&a, "-q");
		argv_pushl(&a, "--fixup", full, NULL);
		goto run;
	}
	/*
	 * "amend! <subject>" and then the commit's new message, either
	 * given or the commit's own in the editor. git 2.32's
	 * --fixup=amend: makes the same, and it is written here for older
	 * git. Its content is what is staged, which may be nothing.
	 */
	subject = subject_of(r, full);
	if (o->message) {
		sb_adds(&body, o->message);
	} else if (o->file) {
		if (read_file(o->file, &body)) {
			repo_err(r, "cannot read %s", o->file);
			goto out;
		}
	} else if (repo_commit_message(r, full, &body)) {
		goto out;
	}
	sb_rtrim(&body);
	sb_addf(&msg, "amend! %s\n\n%s\n", subject, sb_str(&body));
	if (state_write(r, "COMMIT_MSG", msg.buf, msg.len))
		goto out;
	path = carve_path(r, "COMMIT_MSG");
	if (edit) {
		/* only git's notes go (below the scissors): #define stays */
		char cc[32];

		snprintf(cc, sizeof(cc), "core.commentChar=%c",
			 commit_comment_char(r));
		argv_pushl(&a, "-c", cc, "commit", "--cleanup=scissors", "-e",
			   NULL);
	} else {
		argv_push(&a, "commit");
	}
	if (o->quiet)
		argv_push(&a, "-q");
	argv_pushl(&a, "--allow-empty", "-F", path, NULL);
run:
	if (run_git(&a, &ro, NULL, &err)) {
		/*
		 * git commit says why. Nothing staged is said plainly, and
		 * only now, which saves a git when it works.
		 */
		if (!o->amend && !have_staged(r)) {
			repo_err(r, "nothing staged for a fixup of %.7s", full);
		} else {
			repo_err(r, "git commit failed");
			repo_err_detail(r, &err);
		}
		goto out;
	}
	rc = 0;
out:
	/* what the caller staged for it goes back */
	if (rc && o->was_clean)
		git_status_of(r, "reset", "-q", NULL);
	if (path)
		unlink(path);
	argv_clear(&a);
	sb_release(&err);
	sb_release(&msg);
	sb_release(&body);
	free(full);
	free(subject);
	free(path);
	return rc;
}

/*
 * Record a fixup of the staged change into full about to start, with the
 * tip to replay, the change and the tree it makes.
 */
static int fixup_record(struct repo *r, const char *full, const char *head,
			bool was_clean, const struct strbuf *patch)
{
	char *expect = git_line(r, "write-tree", NULL);
	int rc = -1;

	if (!expect || state_str_set(r, "op", "fixup") ||
	    state_str_set(r, "target", full) || state_str_set(r, "tip", head) ||
	    state_str_set(r, "cached", was_clean ? "0" : "1") ||
	    state_str_set(r, "expect-tree", expect) ||
	    state_write(r, "patch", patch->buf, patch->len))
		goto out;
	rc = 0;
out:
	free(expect);
	return rc;
}

/*
 * A fixup that failed. Once the work tree is saved (begun) abort undoes
 * it all, before that only its files and what the caller staged go.
 */
static void fixup_failed(struct repo *r, bool begun, bool was_clean)
{
	if (begun) {
		series_abort(r);
		return;
	}
	state_remove(r);
	if (was_clean)
		git_status_of(r, "reset", "-q", NULL);
}

int fixup_to(struct repo *r, const char *commit, bool was_clean)
{
	struct strbuf patch = STRBUF_INIT, seq = STRBUF_INIT, msg = STRBUF_INIT;
	char *full = NULL, *head = NULL, *fix = NULL, *tree = NULL,
	     *subject = NULL;
	struct argv a = ARGV_INIT;
	bool begun = false;
	int rc = -1;

	/*
	 * While splitting HEAD the only commit to move a change into is the
	 * one below the split (HEAD): a part of the split commit that
	 * belongs there. Older ones wait until the split is finished.
	 */
	if (!repo_rev_status(r, SPLIT_REF)) {
		char *t = resolve_commit(r, commit);
		char *h = repo_rev(r, "HEAD");
		bool below;

		below = t && h && !strcmp(t, h);

		free(t);
		free(h);
		if (below && !split_amend_base(r))
			return 0;
		if (!below)
			repo_err(r,
				 "while splitting, a change can only go into "
				 "the commit below the split (HEAD); finish "
				 "the split first");
		if (was_clean)
			git_status_of(r, "reset", "-q", NULL);
		return -1;
	}
	if (check_series_commit(r, commit, &full))
		goto unstage;
	head = repo_rev(r, "HEAD");
	if (!head) {
		repo_err(r, "cannot read HEAD");
		goto unstage;
	}
	if (!strcmp(head, full)) {
		/* the target is HEAD, a plain amend */
		if (!have_staged(r)) {
			repo_err(r, NOTHING_TO_MOVE_MSG, full);
			goto unstage;
		}
		if (git_quiet(r, "commit", "-q", "--amend", "--no-edit", NULL))
			goto unstage;
		rc = 0;
		goto out;
	}

	/* the change to move, as a patch against HEAD */
	push_diff_args(&a);
	argv_pushl(&a, "--cached", "--binary", "--full-index", NULL);
	if (run_git(&a, NULL, &patch, NULL)) {
		repo_err(r, NO_STAGED_CHANGE_MSG);
		goto unstage;
	}
	/* the diff tells, without a git diff-index before it */
	if (!patch.len) {
		repo_err(r, NOTHING_TO_MOVE_MSG, full);
		goto unstage;
	}
	if (fixup_record(r, full, head, was_clean, &patch))
		goto fail;

	/*
	 * Preferably build "target + change" directly and put it right
	 * after the target. Then only later commits that really touch the
	 * same lines can conflict. If the change does not apply to the
	 * target's tree, fall back to "commit --fixup" and autosquash.
	 */
	tree = staged_change_onto(r, full);
	subject = subject_of(r, full);
	if (tree) {
		sb_addf(&msg, "fixup! %s", subject);
		fix = NULL;
		{
			struct strbuf out = STRBUF_INIT;

			if (git_in(r, msg.buf, msg.len, &out, "commit-tree",
				   tree, "-p", full, NULL)) {
				sb_release(&out);
				goto fail;
			}
			sb_rtrim(&out);
			fix = sb_detach(&out);
		}
		if (state_str_set(r, "fixup", fix) ||
		    state_str_set(r, "fixup-mode", "direct"))
			goto fail;
		/* the change now travels in the rebase */
		if (save_worktree(r) < 0)
			goto fail;
		begun = true;
		if (git_quiet(r, "reset", "-q", "--hard", NULL))
			goto fail;
		sb_addf(&seq,
			"awk -v c=%s -v f=%s '{ print } "
			"($1 == \"pick\" || $1 == \"p\") && index(c, $2) == 1 "
			"{ print \"fixup \" f }' \"$1\" >\"$1.carve\" && "
			"mv \"$1.carve\" \"$1\" #",
			full, fix);
		rc = run_rebase(r, full, seq.buf, false);
	} else {
		if (save_worktree(r) < 0)
			goto fail;
		begun = true;
		if (git_quiet(r, "commit", "-q", "--fixup", full, NULL))
			goto fail;
		fix = repo_rev(r, "HEAD");
		if (state_str_set(r, "fixup", fix) ||
		    state_str_set(r, "fixup-mode", "autosquash") ||
		    git_quiet(r, "reset", "-q", "--hard", NULL))
			goto fail;
		rc = run_rebase(r, full, NULL, true);
	}
	rc = fixup_ran(r, rc);
	goto out;
fail:
	fixup_failed(r, begun, was_clean);
	rc = -1;
	goto out;
unstage:
	if (was_clean)
		git_status_of(r, "reset", "-q", NULL);
out:
	free(full);
	free(head);
	free(fix);
	free(tree);
	free(subject);
	argv_clear(&a);
	sb_release(&patch);
	sb_release(&seq);
	sb_release(&msg);
	return rc;
}

char *change_tree_onto(struct repo *r, const char *commit)
{
	return staged_change_onto(r, commit);
}

int fixup_trees(struct repo *r, char *const *targets, char *const *trees,
		size_t n, bool was_clean)
{
	struct strbuf patch = STRBUF_INIT, seq = STRBUF_INIT,
		      pairs = STRBUF_INIT;
	char *full = NULL, *head = NULL;
	struct argv a = ARGV_INIT;
	bool begun = false;
	int rc = -1;
	size_t i;

	/* targets[0], the oldest, is where the rebase starts */
	if (!n || check_series_commit(r, targets[0], &full))
		goto unstage;
	head = repo_rev(r, "HEAD");
	push_diff_args(&a);
	argv_pushl(&a, "--cached", "--binary", "--full-index", NULL);
	if (!head || run_git(&a, NULL, &patch, NULL) || !patch.len) {
		repo_err(r, NO_STAGED_CHANGE_MSG);
		goto unstage;
	}
	if (fixup_record(r, full, head, was_clean, &patch) ||
	    state_str_set(r, "fixup-mode", "direct"))
		goto fail;
	/* every target + its change, a commit to go right after it */
	for (i = 0; i < n; i++) {
		struct strbuf msg = STRBUF_INIT, id = STRBUF_INIT;
		char *subject = subject_of(r, targets[i]);
		int frc;

		sb_addf(&msg, "fixup! %s", subject);
		frc = git_in(r, msg.buf, msg.len, &id, "commit-tree", trees[i],
			     "-p", targets[i], NULL);
		sb_rtrim(&id);
		if (!frc)
			sb_addf(&pairs, "%s%s:%s", i ? " " : "", targets[i],
				id.buf);
		if (!frc && !i)
			frc = state_str_set(r, "fixup", id.buf);
		free(subject);
		sb_release(&msg);
		sb_release(&id);
		if (frc)
			goto fail;
	}
	if (save_worktree(r) < 0)
		goto fail;
	begun = true;
	if (git_quiet(r, "reset", "-q", "--hard", NULL))
		goto fail;
	/* after the pick of each target its fixup, in one rebase for all */
	sb_addf(&seq,
		"awk -v p='%s' 'BEGIN { n = split(p, kv, \" \") } { print } "
		"$1 == \"pick\" || $1 == \"p\" { for (i = 1; i <= n; i++) "
		"{ split(kv[i], x, \":\"); if (index(x[1], $2) == 1) "
		"print \"fixup \" x[2] } }' \"$1\" >\"$1.carve\" && "
		"mv \"$1.carve\" \"$1\" #",
		pairs.buf);
	rc = run_rebase(r, full, seq.buf, false);
	rc = fixup_ran(r, rc);
	goto out;
fail:
	fixup_failed(r, begun, was_clean);
	rc = -1;
	goto out;
unstage:
	if (was_clean)
		git_status_of(r, "reset", "-q", NULL);
out:
	free(full);
	free(head);
	argv_clear(&a);
	sb_release(&patch);
	sb_release(&seq);
	sb_release(&pairs);
	return rc;
}

/* ---- defer ------------------------------------------------------------- */

static int commit_new(struct repo *r, const char *target)
{
	struct run_opts o = { .tty = true };
	char *msg = state_str(r, "newmsg");
	struct strbuf tmpl = STRBUF_INIT;
	struct strbuf err = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	char *path = NULL;
	char *subject;
	int rc;

	subject = subject_of(r, target);

	argv_pushl(&a, "commit", "-q", NULL);
	if (msg && *msg) {
		argv_pushl(&a, "-m", msg, NULL);
	} else {
		sb_addf(&tmpl, "\n\n# Split out of %.7s \"%s\".\n", target,
			subject);
		state_write(r, "COMMIT_TEMPLATE", tmpl.buf, tmpl.len);
		path = carve_path(r, "COMMIT_TEMPLATE");
		argv_pushl(&a, "-t", path, NULL);
	}
	rc = run_git(&a, &o, NULL, &err);
	if (rc) {
		repo_err(r, "git commit failed");
		repo_err_detail(r, &err);
	}
	sb_release(&err);
	argv_clear(&a);
	free(msg);
	free(subject);
	free(path);
	sb_release(&tmpl);
	return rc ? -1 : 0;
}

/* Continue the defer state machine until done or stopped. */
static int defer_run(struct repo *r)
{
	struct strbuf patch = STRBUF_INIT, old = STRBUF_INIT,
		      range = STRBUF_INIT;
	char *phase, *target, *after, *tip, *conflicted;
	int rc = -1;

	for (;;) {
		phase = state_str(r, "phase");
		target = state_str(r, "target");
		after = state_str(r, "after");
		if (!phase || !target) {
			free(phase);
			free(target);
			free(after);
			return repo_err(r, "defer state is damaged");
		}
		if (!strcmp(phase, "apply")) {
			struct strbuf e = STRBUF_INIT;
			/* at the break after "after": bring the change back */
			struct argv a = ARGV_INIT;
			struct run_opts o = { 0 };

			sb_reset(&patch);
			state_read(r, "patch", &patch);
			o.in = patch.buf;
			o.inlen = patch.len;
			argv_pushl(&a, "apply", "--index", "--3way",
				   "--whitespace=nowarn", "-", NULL);
			rc = run_git(&a, &o, NULL, &e);
			argv_clear(&a);
			sb_release(&e);
			if (rc) {
				state_str_set(r, "phase", "commit");
				rc = stopped(r, "applying the deferred change");
				goto out;
			}
			state_str_set(r, "phase", "commit");
		} else if (!strcmp(phase, "commit")) {
			if (count_conflicts(r)) {
				rc = repo_err(r, "resolve and stage the "
						 "conflicted files first");
				goto out;
			}
			if (commit_new(r, target)) {
				repo_err(r, "committing the deferred change "
					    "failed; {finish} tries again, "
					    "{abort} gives up");
				goto out;
			}
			{
				char *nc = repo_rev(r, "HEAD");

				state_str_set(r, "newcommit", nc ? nc : "");
				free(nc);
			}
			state_str_set(r, "phase", "rebase");
			if (!after) {
				char *nt = repo_rev(r, "HEAD^{tree}");
				struct strbuf spec = STRBUF_INIT;
				char *ot;

				sb_addf(&spec, "%s^{tree}", target);
				ot = repo_rev(r, spec.buf);
				if (!nt || !ot || strcmp(nt, ot))
					repo_warn(r,
						  "warning: the new commit's "
						  "tree differs from %.7s",
						  target);
				free(nt);
				free(ot);
				sb_release(&spec);
			}
		} else if (!strcmp(phase, "rebase")) {
			char *newc = repo_rev(r, "HEAD");

			if (rebase_in_progress(r)) {
				rc = rebase_continue(r);
				if (rc < 0) {
					free(newc);
					goto out;
				}
				/* without a break of ours, it stopped */
				if (rc == 1 && (!after || count_conflicts(r))) {
					free(newc);
					rc = stopped(r, "replaying the series");
					goto out;
				}
				if (rc == 1 && after) {
					/* stopped at our break */
					sb_reset(&range);
					state_str_set(r, "phase", "apply");
					free(newc);
					free(phase);
					free(target);
					free(after);
					continue;
				}
			}
			/* done, and descendants must have kept their trees */
			tip = state_str(r, "tip");
			conflicted = state_str(r, "conflicted");
			sb_reset(&old);
			sb_reset(&range);
			rc = 0;
			if (tip && !conflicted) {
				const char *from = after ? after : target;
				int failed;

				sb_addf(&range, "%s..%s", from, tip);
				failed = list_commits(r, range.buf, &old);
				sb_reset(&range);
				/* commits above the new one */
				if (after) {
					char *nc = state_str(r, "newcommit");

					sb_addf(&range, "%s..HEAD",
						nc ? nc : newc);
					free(nc);
				} else {
					sb_addf(&range, "%s..HEAD", newc);
				}
				rc = check_replayed(
					r, failed ? NULL : sb_str(&old),
					range.buf);
			}
			free(tip);
			free(conflicted);
			free(newc);
			state_remove(r);
			goto out;
		} else {
			rc = repo_err(r, "unknown defer phase '%s'", phase);
			goto out;
		}
		free(phase);
		free(target);
		free(after);
	}
out:
	free(phase);
	free(target);
	free(after);
	sb_release(&patch);
	sb_release(&old);
	sb_release(&range);
	return rc;
}

int defer_lines(struct repo *r, const char *commit, const char *path,
		const char *spec, const char *after, const char *msg)
{
	char *full = NULL, *head = NULL, *aft = NULL, *ptree = NULL,
	     *itree = NULL, *parent = NULL;
	struct strbuf err = STRBUF_INIT, patch = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	struct fdiff *fd = NULL;
	int rc = -1;

	if (check_series_commit(r, commit, &full))
		return -1;
	if (!tree_is_clean(r, NULL)) {
		refuse_dirty(r);
		goto out;
	}
	head = repo_rev(r, "HEAD");
	if (!head) {
		repo_err(r, "cannot read HEAD");
		goto out;
	}
	if (after) {
		struct strbuf range = STRBUF_INIT, out = STRBUF_INIT;

		aft = resolve_commit(r, after);
		if (aft) {
			sb_addf(&range, "%s..HEAD", full);
			list_commits(r, range.buf, &out);
		}
		if (!aft || !strstr(sb_str(&out), aft)) {
			repo_err(r, "%s is not a commit between %.7s and HEAD",
				 after, full);
			sb_release(&range);
			sb_release(&out);
			goto out;
		}
		sb_release(&range);
		sb_release(&out);
	}
	/* validate the selection before touching anything */
	if (load_commit_diff(r, full, path, NULL, &fd))
		goto out;
	if (!fd) {
		repo_err(r, "%.7s does not change %s", full, path);
		goto out;
	}
	if (sel_apply(fd, spec, &err) <= 0) {
		repo_err(r, "%s", err.len ? err.buf : "nothing selected");
		goto out;
	}
	if (!fdiff_line_ops(fd, NULL)) {
		const char *why;

		fdiff_line_ops(fd, &why);
		repo_err(r, "%s: %s", path, why);
		goto out;
	}
	if (state_str_set(r, "op", "defer") || state_str_set(r, "tip", head) ||
	    state_str_set(r, "target", full) ||
	    state_str_set(r, "newmsg", msg ? msg : "") ||
	    (aft && state_str_set(r, "after", aft))) {
		state_remove(r);
		goto out;
	}
	if (strcmp(head, full) && rebase_edit_at(r, full, aft)) {
		state_remove(r);
		goto out;
	}

	/* at the target, take the selection out of it */
	if (apply_selection(r, fd, PATCH_UNSTAGE))
		goto abort;
	parent = parent_of(r, full);
	ptree = parent ? repo_rev(r, "HEAD~1^{tree}") :
			 xstrdup(repo_empty_tree(r));
	itree = git_line(r, "write-tree", NULL);
	if (ptree && itree && !strcmp(ptree, itree)) {
		repo_err(r,
			 "the selection is all of %.7s; nothing would be "
			 "left of it",
			 full);
		goto abort;
	}
	if (git_quiet(r, "commit", "-q", "--amend", "--no-edit", NULL))
		goto abort;
	if (!aft) {
		if (git_quiet(r, "--literal-pathspecs", "add", "-A", "--", path,
			      NULL))
			goto abort;
		state_str_set(r, "phase", "commit");
	} else {
		/* carry the change to the break after "after" */
		argv_push(&a, "--literal-pathspecs");
		push_diff_args(&a);
		argv_pushl(&a, "--binary", "HEAD", "--", path, NULL);
		if (run_git(&a, NULL, &patch, NULL) ||
		    state_write(r, "patch", patch.buf, patch.len) ||
		    git_quiet(r, "reset", "-q", "--hard", "HEAD", NULL))
			goto abort;
		state_str_set(r, "phase", "rebase");
	}
	rc = defer_run(r);
	goto out;
abort:
	/* series_abort does the same for a defer, if this fails */
	if (rebase_in_progress(r) ?
		    rebase_abort(r) :
		    git_quiet(r, "reset", "-q", "--hard", head, NULL))
		rollback_failed(r, head, true);
	else
		state_remove(r);
	rc = -1;
out:
	fdiff_free(fd);
	free(fd);
	free(full);
	free(head);
	free(aft);
	free(ptree);
	free(itree);
	free(parent);
	argv_clear(&a);
	sb_release(&err);
	sb_release(&patch);
	return rc;
}

/* ---- squash -------------------------------------------------------------- */

/*
 * "fixup! ", "amend! ", "squash! " (repeated too) off a subject; *kind
 * the first one ('f', 'a', 's'), 0 for none, as git's autosquash reads
 * them.
 */
static const char *strip_fixup(const char *s, char *kind)
{
	*kind = 0;
	for (;;) {
		char k = starts_with(s, "fixup! ")  ? 'f' :
			 starts_with(s, "amend! ")  ? 'a' :
			 starts_with(s, "squash! ") ? 's' :
						      0;

		if (!k)
			return s;
		if (!*kind)
			*kind = k;
		s += k == 's' ? 8 : 7;
	}
}

static bool is_hex_prefix(const char *s, const char *id)
{
	size_t n = strlen(s);

	if (n < 4 || n > 40 || strspn(s, "0123456789abcdef") != n)
		return false;
	return !strncmp(s, id, n);
}

/*
 * The target of fixup i, an index below i or -1 for none, as git finds it.
 * That is the oldest commit with that subject, else the commit the subject
 * is an id prefix of, else the oldest whose subject starts with it.
 */
static long sq_target(const struct squash_plan *p, size_t i)
{
	const char *t;
	char kind;
	size_t j;

	t = strip_fixup(p->c[i].subject, &kind);

	for (j = 0; j < i; j++)
		if (!p->c[j].kind && !strcmp(p->c[j].subject, t))
			return (long)j;
	for (j = 0; j < i; j++)
		if (!p->c[j].kind && is_hex_prefix(t, p->c[j].id))
			return (long)j;
	for (j = 0; j < i; j++)
		if (!p->c[j].kind && *t && starts_with(p->c[j].subject, t))
			return (long)j;
	return -1;
}

/* Single-quoted for sh. */
static void sq_quote(struct strbuf *sb, const char *s)
{
	sb_addch(sb, '\'');
	for (; *s; s++) {
		if (*s == '\'')
			sb_adds(sb, "'\\''");
		else
			sb_addch(sb, *s);
	}
	sb_addch(sb, '\'');
}

void squash_plan_release(struct squash_plan *p)
{
	size_t i;

	for (i = 0; i < p->n; i++) {
		free(p->c[i].id);
		free(p->c[i].subject);
		free(p->c[i].body);
	}
	free(p->c);
	free(p->tip);
	sb_release(&p->todo);
	sb_release(&p->left);
	memset(p, 0, sizeof(*p));
}

int squash_plan(struct repo *r, const char *base, struct squash_plan *p)
{
	struct strbuf out = STRBUF_INIT, range = STRBUF_INIT;
	size_t i, alloc = 0, nmsg = 0;
	struct argv a = ARGV_INIT;
	const char *pos, *end;
	long first = -1;
	int rc = -1;

	memset(p, 0, sizeof(*p));
	p->tip = repo_rev(r, "HEAD");
	if (!p->tip)
		return repo_err(r, "no commits yet");
	/*
	 * The series, base..HEAD or else series_revs, since the squash
	 * starts at the oldest target anyway. One git gives id, parents,
	 * subject and message of each, newest first.
	 */
	argv_pushl(&a, "log", "--no-color", "--first-parent",
		   "--format=%x1e%H %P%x1f%s%x1f%B", NULL);
	if (base) {
		sb_addf(&range, "%s..HEAD", base);
		argv_push(&a, range.buf);
	} else {
		series_revs(r, &a, NULL);
	}
	argv_push(&a, "--");
	if (run_git(&a, NULL, &out, NULL)) {
		repo_err(r, "cannot list the commits of the series");
		goto out;
	}
	/* the records come newest first and are stored oldest first */
	for (end = sb_str(&out) + out.len; end > sb_str(&out);) {
		const char *rec, *rec_end = end, *us1, *us2, *sp;
		struct squash_commit *c;

		pos = end;
		while (pos > sb_str(&out) && pos[-1] != '\x1e')
			pos--;
		rec = pos;
		end = pos > sb_str(&out) ? pos - 1 : pos;
		us1 = memchr(rec, '\x1f', (size_t)(rec_end - rec));
		us2 = us1 ? memchr(us1 + 1, '\x1f',
				   (size_t)(rec_end - us1 - 1)) :
			    NULL;
		if (!us2)
			continue;
		ALLOC_GROW(p->c, p->n + 1, alloc);
		c = &p->c[p->n++];
		memset(c, 0, sizeof(*c));
		sp = memchr(rec, ' ', (size_t)(us1 - rec));
		c->id = xstrndup(rec,
				 sp ? (size_t)(sp - rec) : (size_t)(us1 - rec));
		for (; sp && sp < us1;
		     sp = memchr(sp + 1, ' ', (size_t)(us1 - sp - 1)))
			c->nparents++;
		c->subject = xstrndup(us1 + 1, (size_t)(us2 - us1 - 1));
		strip_fixup(c->subject, &c->kind);
		if (c->kind == 'a') {
			const char *msg = us2 + 1, *rest;
			/* the new message follows the subject's paragraph */
			struct strbuf b = STRBUF_INIT;
			size_t bl;

			sb_add(&b, msg, (size_t)(rec_end - msg));
			rest = strstr(sb_str(&b), "\n\n");
			c->body = xstrdup(rest ? rest + 2 : "");
			sb_release(&b);
			bl = strlen(c->body);
			while (bl && (c->body[bl - 1] == '\n' ||
				      c->body[bl - 1] == ' '))
				c->body[--bl] = '\0';
			if (!bl)
				c->kind = 'f'; /* no message, a fixup */
		}
	}
	if (!p->n) {
		repo_err(r, "no commits in %s", base ? range.buf : "HEAD");
		goto out;
	}
	for (i = 0; i < p->n; i++) {
		p->c[i].target = p->c[i].kind ? sq_target(p, i) : -1;
		if (p->c[i].kind && p->c[i].target < 0) {
			sb_addf(&p->left, "%.7s %s\n", p->c[i].id,
				p->c[i].subject);
			p->c[i].kind = 0; /* stays where it is */
			continue;
		}
		if (p->c[i].kind) {
			p->nfix++;
			if (first < 0 || p->c[i].target < first)
				first = p->c[i].target;
		}
	}
	if (first < 0) {
		repo_err(r, "no fixup!, amend! or squash! commit to squash%s",
			 p->left.len ? " (none has its commit in the series)" :
				       "");
		goto out;
	}
	for (i = (size_t)first; i < p->n; i++) {
		if (p->c[i].nparents > 1) {
			repo_err(r,
				 "%.7s is a merge commit: squash needs a "
				 "series without merges",
				 p->c[i].id);
			goto out;
		}
	}
	/* the todo has each commit, then its fixups in their order */
	p->first = (size_t)first;
	for (i = (size_t)first; i < p->n; i++) {
		bool any = false;
		size_t j;

		if (p->c[i].kind)
			continue;
		sb_addf(&p->todo, "pick %s\n", p->c[i].id);
		for (j = i + 1; j < p->n; j++) {
			char name[32], *path;

			if (!p->c[j].kind || p->c[j].target != (long)i)
				continue;
			any = true;
			sb_addf(&p->todo, "%s %s\n",
				p->c[j].kind == 's' ? "squash" : "fixup",
				p->c[j].id);
			if (p->c[j].kind != 'a')
				continue;
			/* amend!: its message replaces the commit's */
			snprintf(name, sizeof(name), "squash-msg-%zu", ++nmsg);
			path = carve_path(r, name);
			sb_adds(&p->todo, "exec git commit -q --amend "
					  "--no-verify --allow-empty -F ");
			sq_quote(&p->todo, path);
			sb_addch(&p->todo, '\n');
			free(path);
		}
		p->ntargets += any;
	}
	rc = 0;
out:
	if (rc)
		squash_plan_release(p);
	argv_clear(&a);
	sb_release(&out);
	sb_release(&range);
	return rc;
}

int squash_run(struct repo *r, struct squash_plan *p)
{
	struct strbuf seq = STRBUF_INIT;
	char *todo = NULL, *tree = NULL;
	size_t i, nmsg = 0;
	int rc = -1;

	if (rebase_in_progress(r))
		return repo_err(r, REBASE_BUSY_MSG);
	if (op_in_progress(r))
		return repo_err(r, OP_BUSY_MSG);
	/* the same content in other commits, and the tree must not change */
	sb_addf(&seq, "%s^{tree}", p->tip);
	tree = repo_rev(r, seq.buf);
	sb_reset(&seq);
	if (!tree || state_str_set(r, "op", "squash") ||
	    state_str_set(r, "target", p->c[p->first].id) ||
	    state_str_set(r, "tip", p->tip) ||
	    state_str_set(r, "cached", "1") ||
	    state_str_set(r, "expect-tree", tree) ||
	    state_write(r, "squash-todo", p->todo.buf, p->todo.len))
		goto fail;
	for (i = p->first; i < p->n; i++) {
		char name[32];

		if (p->c[i].kind != 'a')
			continue;
		snprintf(name, sizeof(name), "squash-msg-%zu", ++nmsg);
		if (state_write(r, name, p->c[i].body, strlen(p->c[i].body)))
			goto fail;
	}
	/* the todo is ours and replaces git's list */
	todo = carve_path(r, "squash-todo");
	sb_adds(&seq, "cp ");
	sq_quote(&seq, todo);
	sb_adds(&seq, " \"$1\" #");
	/*
	 * the work tree as it is, put back after the rebase (a clean one
	 * stays as it is)
	 */
	rc = save_worktree(r);
	if (rc < 0 || (!rc && git_quiet(r, "reset", "-q", "--hard", NULL))) {
		rc = -1;
		goto fail;
	}
	rc = run_rebase(r, p->c[p->first].id, seq.buf, false);
	if (rc == 1) {
		rc = stopped(r, "the squash");
	} else if (rc < 0) {
		series_abort(r);
		rc = -1;
	} else {
		rc = fixup_done(r);
	}
	goto out;
fail:
	state_remove(r);
out:
	free(todo);
	free(tree);
	sb_release(&seq);
	return rc;
}

/* ---- continue / abort ---------------------------------------------------- */

int series_continue(struct repo *r)
{
	struct series_state st;
	bool squash;
	int rc;

	series_state_load(r, &st);
	if (st.op == OP_NONE) {
		series_state_release(&st);
		return repo_err(r, "nothing in progress to continue");
	}
	if (st.conflicts) {
		series_state_release(&st);
		return repo_err(r, "resolve the conflicts and stage the "
				   "result first");
	}
	if (st.op == OP_DEFER) {
		series_state_release(&st);
		return defer_run(r);
	}
	squash = st.op == OP_SQUASH;
	series_state_release(&st);
	if (!rebase_in_progress(r))
		return fixup_done(r);
	rc = rebase_continue(r);
	if (rc < 0)
		return -1;
	if (rc == 1)
		return stopped(r, squash ? "the squash" : "moving the change");
	return fixup_done(r);
}

int series_abort(struct repo *r)
{
	char *cached = NULL, *fix = NULL, *head = NULL, *tip = NULL,
	     *rollback = NULL;
	struct series_state st;
	int rc = 0;

	series_state_load(r, &st);
	if (st.op == OP_NONE) {
		series_state_release(&st);
		return repo_err(r, "nothing in progress to give up");
	}
	if (st.rebasing && rebase_abort(r))
		rc = -1;
	if (!rc && st.op == OP_DEFER && !st.rebasing) {
		/* the target was HEAD, no rebase, just go back */
		tip = state_str(r, "tip");
		if (tip && git_quiet(r, "reset", "-q", "--hard", tip, NULL))
			rc = -1;
		free(tip);
		tip = NULL;
	}
	if (!rc && (st.op == OP_FIXUP || st.op == OP_SQUASH)) {
		/*
		 * Back to the original tip (dropping a fixup commit made on
		 * top of it), with the change unstaged in the work tree, or
		 * staged again if it was staged to begin with.
		 */
		fix = state_str(r, "fixup");
		tip = state_str(r, "tip");
		cached = state_str(r, "cached");
		head = repo_rev(r, "HEAD");
		rollback = state_str(r, "rollback");
		if (rollback && tip) {
			/* a rollback after the rebase that did not get through */
			if (git_quiet(r, "reset", "-q", "--hard", tip, NULL))
				rc = -1;
		} else if (fix && head && !strcmp(fix, head) && tip) {
			git_quiet(r, "reset", "-q", "--soft", tip, NULL);
		}
		if (!rc && restore_worktree(r, cached && !strcmp(cached, "1")))
			rc = -1;
	}
	if (!rc)
		state_remove(r);
	free(cached);
	free(fix);
	free(head);
	free(tip);
	free(rollback);
	series_state_release(&st);
	return rc;
}
