// SPDX-License-Identifier: MIT
#include "patch.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rename.h"

/* One output line before end-of-line fix-ups. */
struct pline {
	char op;
	const char *text;
	size_t len;
	bool flag; /* "\ No newline" as given by the source diff */
};

static void emit_line(struct strbuf *out, char op, const char *text, size_t len,
		      bool no_eol)
{
	sb_addch(out, op);
	sb_add(out, text, len);
	sb_addch(out, '\n');
	if (no_eol)
		sb_adds(out, NO_EOL_MARKER);
}

static void emit_header(struct strbuf *out, const char *path)
{
	const char *tab = strchr(path, ' ') ? "\t" : "";
	struct strbuf a = STRBUF_INIT, b = STRBUF_INIT;

	sb_adds(&a, "a/");
	sb_adds(&a, path);
	sb_adds(&b, "b/");
	sb_adds(&b, path);
	sb_adds(out, "diff --git ");
	quote_path(out, a.buf);
	sb_addch(out, ' ');
	quote_path(out, b.buf);
	sb_adds(out, "\n--- ");
	quote_path(out, a.buf);
	sb_adds(out, tab);
	sb_adds(out, "\n+++ ");
	quote_path(out, b.buf);
	sb_adds(out, tab);
	sb_addch(out, '\n');
	sb_release(&a);
	sb_release(&b);
}

/*
 * Emit one hunk. "Real" is the side that must match the index as it is,
 * the old side for stage and the new side for unstage. "Target" is the
 * result. Returns the change in line count, target minus real.
 */
static int emit_hunk(struct strbuf *out, const struct hunk *h,
		     enum patch_dir dir, int delta)
{
	int old_cnt = 0, new_cnt = 0, real_start, real_cnt, tgt_start, tgt_cnt;
	struct strbuf body = STRBUF_INIT;
	bool stage = dir == PATCH_STAGE;
	bool seen_tgt = false;
	struct pline *pl;
	size_t n = 0, i;
	bool *tgt_last;
	char real_op;
	size_t *ord;

	real_op = stage ? '-' : '+';
	ord = xcalloc(h->nlines, sizeof(*ord));
	pl = xcalloc(h->nlines, sizeof(*pl));
	tgt_last = xcalloc(h->nlines, sizeof(*tgt_last));

	hunk_order(h, ord);
	for (i = 0; i < h->nlines; i++) {
		const struct dline *d = &h->lines[ord[i]];
		char op = d->op;

		if (op != ' ' && !d->sel) {
			if (op == real_op)
				op = ' ';
			else
				continue;
		}
		pl[n++] = (struct pline){ op, d->text, d->len, d->no_eol };
	}

	/* Which entry is the last one present on the target side? */
	for (i = n; i-- > 0;) {
		bool on_tgt = pl[i].op == ' ' || pl[i].op != real_op;

		if (on_tgt && !seen_tgt) {
			tgt_last[i] = true;
			seen_tgt = true;
		}
	}

	for (i = 0; i < n; i++) {
		const struct pline *p = &pl[i];
		bool tgt_noeol;

		tgt_noeol = p->flag && tgt_last[i];

		switch (p->op) {
		case ' ':
			if (p->flag == tgt_noeol) {
				emit_line(&body, ' ', p->text, p->len, p->flag);
				old_cnt++;
				new_cnt++;
				break;
			}
			/*
			 * same text with a different end of line, written
			 * as a change
			 */
			emit_line(&body, '-', p->text, p->len,
				  stage ? p->flag : tgt_noeol);
			emit_line(&body, '+', p->text, p->len,
				  stage ? tgt_noeol : p->flag);
			old_cnt++;
			new_cnt++;
			break;
		case '-':
			emit_line(&body, '-', p->text, p->len,
				  stage ? p->flag : tgt_noeol);
			old_cnt++;
			break;
		case '+':
			emit_line(&body, '+', p->text, p->len,
				  stage ? tgt_noeol : p->flag);
			new_cnt++;
			break;
		default:
			/* a diff line is ' ', '-' or '+' */
			break;
		}
	}

	real_start = stage ? h->old_start : h->new_start;
	real_cnt = stage ? old_cnt : new_cnt;
	tgt_cnt = stage ? new_cnt : old_cnt;
	tgt_start = real_start + delta;
	if (!real_cnt && tgt_cnt)
		tgt_start++;
	else if (real_cnt && !tgt_cnt)
		tgt_start--;
	if (stage)
		sb_addf(out, "@@ -%d,%d +%d,%d @@\n", real_start, real_cnt,
			tgt_start, tgt_cnt);
	else
		sb_addf(out, "@@ -%d,%d +%d,%d @@\n", tgt_start, tgt_cnt,
			real_start, real_cnt);
	sb_add(out, body.buf, body.len);
	sb_release(&body);
	free(pl);
	free(tgt_last);
	free(ord);
	return tgt_cnt - real_cnt;
}

static bool hunk_selected(const struct hunk *h)
{
	size_t i;

	for (i = 0; i < h->nlines; i++)
		if (h->lines[i].sel && h->lines[i].op != ' ')
			return true;
	return false;
}

int build_patch(const struct fdiff *fd, enum patch_dir dir, struct strbuf *out,
		struct strbuf *err)
{
	const char *why, *path;
	int delta = 0, nh = 0;
	size_t i;

	if (!fdiff_line_ops(fd, &why)) {
		sb_adds(err, why);
		return -1;
	}
	path = fd->new_path ? fd->new_path : fd->old_path;
	if (!path) {
		sb_adds(err, "diff without a file name");
		return -1;
	}
	for (i = 0; i < fd->nhunks; i++) {
		if (!hunk_selected(&fd->hunks[i]))
			continue;
		if (!nh++)
			emit_header(out, path);
		delta += emit_hunk(out, &fd->hunks[i], dir, delta);
	}
	return nh;
}

int git_apply(struct repo *r, const struct strbuf *patch, unsigned flags)
{
	struct run_opts o = { .in = patch->buf, .inlen = patch->len };
	struct strbuf e = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	int rc;

	argv_pushl(&a, GIT_APPLY_PLAIN, "apply", NULL);
	if (flags & APPLY_CACHED)
		argv_push(&a, "--cached");
	if (flags & APPLY_INDEX)
		argv_push(&a, "--index");
	if (flags & APPLY_ZERO)
		argv_push(&a, "--unidiff-zero");
	if (flags & APPLY_REVERSE)
		argv_push(&a, "--reverse");
	argv_pushl(&a, "--recount", "--whitespace=nowarn", "-", NULL);
	rc = run_git(&a, &o, NULL, &e);
	if (rc)
		git_fail(r, "git apply", &e);
	argv_clear(&a);
	sb_release(&e);
	return rc ? -1 : 0;
}

/* The flags of git_apply. */
static unsigned apply_flags(bool reverse, bool cached, bool zero)
{
	return (reverse ? APPLY_REVERSE : 0) | (cached ? APPLY_CACHED : 0) |
	       (zero ? APPLY_ZERO : 0);
}

int apply_patch(struct repo *r, const struct strbuf *patch, bool reverse)
{
	return git_apply(r, patch, apply_flags(reverse, true, false));
}

int apply_patch_zero(struct repo *r, const struct strbuf *patch, bool reverse,
		     bool zero)
{
	return git_apply(r, patch, apply_flags(reverse, true, zero));
}

int apply_selection(struct repo *r, const struct fdiff *fd, enum patch_dir dir)
{
	struct strbuf patch = STRBUF_INIT, err = STRBUF_INIT;
	int nh, rc;

	nh = build_patch(fd, dir, &patch, &err);
	if (nh < 0) {
		rc = repo_err(r, "%s", err.buf);
	} else if (!nh) {
		rc = repo_err(r, "nothing selected");
	} else {
		char *from = repo_index_stamp(r);

		rc = git_apply(r, &patch,
			       apply_flags(dir == PATCH_UNSTAGE, true,
					   fd->zero_context));
		/* kept for undo, the patch and the index around it */
		if (!rc) {
			sb_reset(&r->last_patch);
			sb_add(&r->last_patch, patch.buf, patch.len);
			r->last_patch_reverse = dir == PATCH_UNSTAGE;
			r->last_patch_zero = fd->zero_context;
			free(r->last_patch_from);
			r->last_patch_from = from;
			from = NULL;
		}
		free(from);
	}
	sb_release(&patch);
	sb_release(&err);
	return rc;
}

int intent_to_add(struct repo *r, const char *path)
{
	return git_quiet(r, "--literal-pathspecs", "add", "-N", "--", path,
			 NULL) ?
		       -1 :
		       0;
}

static bool in_head(struct repo *r, const char *path)
{
	struct strbuf spec = STRBUF_INIT;
	int rc;

	sb_addf(&spec, "HEAD:%s", path);
	rc = repo_rev_status(r, spec.buf);
	sb_release(&spec);
	return rc == 0;
}

/*
 * After unstaging every line of a new file the index holds an empty blob.
 * Turn it back into an intent-to-add entry, which looks untouched.
 */
static void restore_ita(struct repo *r, const char *path)
{
	struct fdiff *fd = NULL;

	if (in_head(r, path) || !path_in_worktree(path))
		return;
	if (load_diff(r, path, true, &fd) || !fd)
		return;
	if (fd->is_new && !fd->nhunks && !fd->binary &&
	    !git_quiet(r, "update-index", "--force-remove", "--", path, NULL))
		intent_to_add(r, path);
	fdiff_free(fd);
	free(fd);
}

int stage_lines(struct repo *r, const char *path, const char *spec,
		enum patch_dir dir)
{
	struct strbuf err = STRBUF_INIT;
	struct fdiff *fd = NULL;
	bool added_ita = false;
	int n, rc = -1, lrc;
	char *src = NULL;

	/*
	 * A rename is diffed against the old file. In the work tree its
	 * move is staged with the lines, after the selection is checked
	 * out. Staged, its lines are those of the new path against the old
	 * one.
	 */
	if (dir == PATCH_STAGE) {
		int in = 1;

		/*
		 * the diff first, since a changed tracked file needs
		 * nothing else
		 */
		if (load_diff(r, path, false, &fd))
			goto out;
		if (!fd || fd->is_new)
			in = path_in_index(r, path);
		if (in < 0)
			goto out;
		if (!in && path_in_worktree(path)) {
			/*
			 * untracked, which is a rename's new half, or
			 * intent-to-add
			 */
			src = worktree_rename_source(r, path);
			if (src) {
				fdiff_free(fd);
				free(fd);
				fd = NULL;
			} else {
				if (intent_to_add(r, path))
					goto out;
				added_ita = true;
				/*
				 * The diff of the untracked file is that
				 * of the empty intent-to-add entry,
				 * without git diff.
				 */
			}
		}
	} else {
		src = staged_rename_source(r, path);
	}
	if (fd)
		lrc = 0;
	else if (!src)
		lrc = load_diff(r, path, dir == PATCH_UNSTAGE, &fd);
	else if (dir == PATCH_STAGE)
		lrc = rename_diff(r, src, path, NULL, &fd);
	else
		lrc = staged_rename_diff(r, src, path, NULL, &fd);
	if (lrc)
		goto out;
	if (!fd) {
		repo_err(r, "%s: no %s changes", path,
			 dir == PATCH_STAGE ? "unstaged" : "staged");
		goto out;
	}
	n = sel_apply(fd, spec, &err);
	if (n < 0) {
		repo_err(r, "%s", err.buf);
		goto out;
	}
	if (!n) {
		repo_err(r, NO_CHANGED_LINES_MSG, spec);
		goto out;
	}
	if (src && dir == PATCH_STAGE && stage_move(r, src, path))
		goto out;
	rc = apply_selection(r, fd, dir);
	if (!rc && dir == PATCH_UNSTAGE)
		restore_ita(r, path);
out:
	if (rc && added_ita)
		git_status_of(r, "update-index", "--force-remove", "--", path,
			      NULL);
	fdiff_free(fd);
	free(fd);
	free(src);
	sb_release(&err);
	return rc;
}

/*
 * git <cmd...> -- paths, one process for all of them. Many paths go on
 * stdin (--pathspec-from-file, git 2.26) to stay under the argument limit.
 * 15,000 kernel paths are 700 kB, and macOS allows 1 MB for everything.
 */
static int git_paths(struct repo *r, const char *const *cmd,
		     const char *const *paths, size_t n)
{
	struct strbuf e = STRBUF_INIT, in = STRBUF_INIT;
	struct run_opts ro = { 0 };
	struct argv a = ARGV_INIT;
	size_t i;
	int rc;

	argv_push(&a, "--literal-pathspecs");
	for (i = 0; cmd[i]; i++)
		argv_push(&a, cmd[i]);
	if (n > PATHSPEC_ARGS_MAX) {
		for (i = 0; i < n; i++)
			sb_add(&in, paths[i], strlen(paths[i]) + 1);
		argv_pushl(&a, "--pathspec-from-file=-", "--pathspec-file-nul",
			   NULL);
		ro.in = in.buf;
		ro.inlen = in.len;
	} else {
		argv_push(&a, "--");
		for (i = 0; i < n; i++)
			argv_push(&a, paths[i]);
	}
	rc = run_git(&a, &ro, NULL, &e);
	if (rc)
		git_fail(r, cmd[0], &e);
	argv_clear(&a);
	sb_release(&e);
	sb_release(&in);
	return rc ? -1 : 0;
}

int stage_files(struct repo *r, const char *const *paths, size_t n)
{
	static const char *const add[] = { "add", "-A", NULL };

	return git_paths(r, add, paths, n);
}

/* update-index takes exact paths and looks them up directly in the index. */
int stage_tracked_paths(struct repo *r, const char *const *paths, size_t n)
{
	struct strbuf in = STRBUF_INIT, err = STRBUF_INIT;
	struct argv args = ARGV_INIT;
	struct run_opts opts = { 0 };
	size_t i;
	int rc;

	for (i = 0; i < n; i++)
		sb_add(&in, paths[i], strlen(paths[i]) + 1);
	opts.in = in.buf;
	opts.inlen = in.len;
	argv_pushl(&args, "update-index", "--remove", "-z", "--stdin", NULL);
	rc = run_git(&args, &opts, NULL, &err);
	argv_clear(&args);
	if (rc)
		git_fail(r, "update-index", &err);
	sb_release(&err);
	sb_release(&in);
	return rc ? -1 : 0;
}

/*
 * git restore --staged, which unlike git reset does not refresh the whole
 * index first. Without a HEAD to restore from, git rm --cached.
 */
int unstage_files(struct repo *r, const char *const *paths, size_t n)
{
	static const char *const restore[] = { "restore", "-q", "--staged",
					       NULL };
	static const char *const rm[] = { "rm", "-q", "--cached", NULL };

	if (!git_paths(r, restore, paths, n))
		return 0;
	if (have_head(r))
		return -1;
	return git_paths(r, rm, paths, n);
}

int unstage_all(struct repo *r)
{
	if (!git_quiet(r, "restore", "-q", "--staged", "--", ":/", NULL))
		return 0;
	if (have_head(r))
		return -1;
	sb_reset(&r->err);
	return git_quiet(r, "rm", "-r", "-q", "--cached", "--", ":/", NULL);
}

int stage_file(struct repo *r, const char *path)
{
	return stage_files(r, &path, 1);
}

int unstage_file(struct repo *r, const char *path)
{
	return unstage_files(r, &path, 1);
}

int discard_selection(struct repo *r, const struct fdiff *fd)
{
	struct strbuf err = STRBUF_INIT, patch = STRBUF_INIT;
	int rc = -1;

	/*
	 * Relative to the work tree the unselected changes stay. The
	 * unstage rule builds that patch, and --reverse takes the selected
	 * changes out of the work tree file.
	 */
	if (build_patch(fd, PATCH_UNSTAGE, &patch, &err) < 0)
		repo_err(r, "%s", err.buf);
	else
		rc = git_apply(r, &patch,
			       apply_flags(true, false, fd->zero_context));
	sb_release(&err);
	sb_release(&patch);
	return rc;
}

int discard_lines(struct repo *r, const char *path, const char *spec)
{
	struct strbuf err = STRBUF_INIT;
	int in = path_in_index(r, path);
	struct fdiff *fd = NULL;
	const char *why;
	int rc = -1;
	int n;

	if (in < 0)
		return -1;
	if (!in)
		return repo_err(r, "%s is untracked: discard the whole file",
				path);
	if (load_diff(r, path, false, &fd))
		goto out;
	if (!fd) {
		repo_err(r, "%s: no unstaged changes", path);
		goto out;
	}
	if (!fdiff_line_ops(fd, &why)) {
		repo_err(r, "%s: %s; discard the whole file", path, why);
		goto out;
	}
	n = sel_apply(fd, spec, &err);
	if (n < 0) {
		repo_err(r, "%s", err.buf);
		goto out;
	}
	if (!n) {
		repo_err(r, NO_CHANGED_LINES_MSG, spec);
		goto out;
	}
	rc = discard_selection(r, fd);
out:
	if (fd) {
		fdiff_free(fd);
		free(fd);
	}
	sb_release(&err);
	return rc;
}

int discard_file(struct repo *r, const char *path)
{
	int in = path_in_index(r, path);

	if (in < 0)
		return -1;
	/* untracked or only intent-to-add, the whole file is the change */
	if (!in || (!in_head(r, path) &&
		    git_status_of(r, "--literal-pathspecs", "diff", "--cached",
				  "--quiet", "--", path, NULL) == 0)) {
		if (in && git_quiet(r, "update-index", "--force-remove", "--",
				    path, NULL))
			return -1;
		if (unlink(path) && errno != ENOENT)
			return repo_err(r, "cannot remove %s: %s", path,
					strerror(errno));
		return 0;
	}
	return git_quiet(r, "--literal-pathspecs", "checkout", "-q", "--", path,
			 NULL) ?
		       -1 :
		       0;
}

int remove_file(struct repo *r, const char *path)
{
	int in = path_in_index(r, path);

	if (in < 0)
		return -1;
	if (!in)
		return repo_err(r, "%s is not tracked: discard deletes it",
				path);
	return git_quiet(r, "--literal-pathspecs", "rm", "-q", "-f", "--", path,
			 NULL) ?
		       -1 :
		       0;
}
