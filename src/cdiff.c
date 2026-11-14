// SPDX-License-Identifier: MIT
/* The diff of a commit, its files, a file's diff and patches of its lines. */
#include "cdiff.h"
#include "rename.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int load_commit_diff(struct repo *r, const char *commit, const char *path,
		     const struct diff_opts *opts, struct fdiff **out)
{
	return load_commit_diff_from(r, commit, path, NULL, opts, out);
}

int load_commit_diff_from(struct repo *r, const char *commit, const char *path,
			  const char *from, const struct diff_opts *opts,
			  struct fdiff **out)
{
	struct strbuf o = STRBUF_INIT, e = STRBUF_INIT, perr = STRBUF_INIT;
	char *id = resolve_commit(r, commit);
	struct run_opts ro = { 0 };
	struct fdiff *files = NULL;
	struct argv a = ARGV_INIT;
	char *parent;
	size_t n = 0;
	int rc = -1;

	*out = NULL;
	if (!id)
		return repo_err(r, "unknown commit '%s'", commit);
	parent = parent_of(r, id);
	argv_push(&a, "--literal-pathspecs");
	push_diff_args(&a);
	push_diff_opts(&a, opts);
	/* a rename, both paths found as one */
	if (from)
		argv_push(&a, "-M");
	argv_pushl(&a, parent ? parent : repo_empty_tree(r), id, "--", path,
		   NULL);
	if (from)
		argv_push(&a, from);
	diff_run_opts(opts, &ro);
	if (run_git(&a, &ro, &o, &e) || diff_stopped(opts)) {
		if (diff_stopped(opts))
			rc = 0;
		else
			git_fail(r, "git diff", &e);
		goto out;
	}
	if (diff_parse(o.buf, o.len, &files, &n, &perr)) {
		repo_err(r, "%s", perr.buf);
		goto out;
	}
	if (n == 1) {
		*out = xmalloc(sizeof(**out));
		**out = files[0];
		free(files);
		files = NULL;
		n = 0;
	}
	rc = 0;
out:
	fdiff_free_array(files, n);
	free(id);
	free(parent);
	argv_clear(&a);
	sb_release(&o);
	sb_release(&e);
	sb_release(&perr);
	return rc;
}

int load_commit_diff_all(struct repo *r, const char *commit,
			 const struct diff_opts *opts, struct fdiff **out,
			 size_t *n)
{
	struct strbuf o = STRBUF_INIT, e = STRBUF_INIT, perr = STRBUF_INIT;
	char *id = resolve_commit(r, commit);
	struct run_opts ro = { 0 };
	struct argv a = ARGV_INIT;
	char *parent;
	int rc = -1;

	*out = NULL;
	*n = 0;
	if (!id)
		return repo_err(r, "unknown commit '%s'", commit);
	parent = parent_of(r, id);
	push_diff_args(&a);
	push_diff_opts(&a, opts);
	argv_pushl(&a, parent ? parent : repo_empty_tree(r), id, NULL);
	push_diff_excludes(&a, opts);
	diff_run_opts(opts, &ro);
	/* a load stopped early is no error, just no diff */
	if (run_git(&a, &ro, &o, &e) && !diff_stopped(opts))
		git_fail(r, "git diff", &e);
	else if (!diff_stopped(opts) && diff_parse(o.buf, o.len, out, n, &perr))
		repo_err(r, "%s", perr.buf);
	else
		rc = 0;
	free(id);
	free(parent);
	argv_clear(&a);
	sb_release(&o);
	sb_release(&e);
	sb_release(&perr);
	return rc;
}

/* commit_patch for the unstage and/or the stage rule, one diff. */
static int patches(struct repo *r, const char *commit, const char *name,
		   const char *path, const char *sel, struct strbuf *rem,
		   struct strbuf *add, int *nlines)
{
	struct strbuf *patch = rem ? rem : add;
	struct strbuf err = STRBUF_INIT;
	struct fdiff *fd = NULL;
	const char *why;
	int rc = -1, n;

	if (nlines)
		*nlines = 0;
	if (!patch)
		return repo_err(r, "no patch asked for");
	if (!sel) {
		char *parent = parent_of(r, commit);
		struct strbuf e = STRBUF_INIT;
		struct argv a = ARGV_INIT;

		argv_push(&a, "--literal-pathspecs");
		push_diff_args(&a);
		argv_pushl(&a, "--binary", "--full-index",
			   parent ? parent : repo_empty_tree(r), commit, "--",
			   path, NULL);
		rc = run_git(&a, NULL, patch, &e) ?
			     git_fail(r, "git diff", &e) :
			     0;
		if (!rc && !patch->len)
			rc = repo_err(r, "%s does not change %s", name, path);
		/* all of it is the same patch both ways */
		if (!rc && rem && add)
			sb_add(add, rem->buf, rem->len);
		free(parent);
		argv_clear(&a);
		sb_release(&e);
		return rc;
	}
	if (load_commit_diff(r, commit, path, NULL, &fd))
		goto out;
	if (!fd) {
		repo_err(r, "%s does not change %s", name, path);
		goto out;
	}
	if (!fdiff_line_ops(fd, &why)) {
		repo_err(r, "%s: %s; take the whole file", path, why);
		goto out;
	}
	n = sel_apply(fd, sel, &err);
	if (n < 0) {
		repo_err(r, "%s", err.buf);
		goto out;
	}
	if (!n) {
		repo_err(r, NO_CHANGED_LINES_MSG, sel);
		goto out;
	}
	if ((rem && build_patch(fd, PATCH_UNSTAGE, rem, &err) < 0) ||
	    (add && build_patch(fd, PATCH_STAGE, add, &err) < 0)) {
		repo_err(r, "%s", err.buf);
		goto out;
	}
	if (nlines)
		*nlines = n;
	rc = 0;
out:
	if (fd) {
		fdiff_free(fd);
		free(fd);
	}
	sb_release(&err);
	return rc;
}

int commit_patch(struct repo *r, const char *commit, const char *name,
		 const char *path, const char *sel, enum patch_dir dir,
		 struct strbuf *patch, int *nlines)
{
	return patches(r, commit, name, path, sel,
		       dir == PATCH_UNSTAGE ? patch : NULL,
		       dir == PATCH_UNSTAGE ? NULL : patch, nlines);
}

int commit_patches(struct repo *r, const char *commit, const char *name,
		   const char *path, const char *sel, struct strbuf *rem,
		   struct strbuf *add)
{
	return patches(r, commit, name, path, sel, rem, add, NULL);
}

int commit_files(struct repo *r, const char *commit, struct cfile **out,
		 size_t *n)
{
	struct strbuf o = STRBUF_INIT;
	const char *p, *end;
	size_t alloc = 0;

	*out = NULL;
	*n = 0;
	/*
	 * "X\0path\0" per file and "R<score>\0old\0new\0" for a rename.
	 * --root diffs a root commit against nothing.
	 */
	if (git_out(r, &o, "diff-tree", "-r", "--root", "--no-commit-id", "-M",
		    "--name-status", "-z", commit, NULL)) {
		sb_release(&o);
		return -1;
	}
	p = sb_str(&o);
	end = p + o.len;
	while (p < end) {
		const char *st = p, *path;

		p += strlen(p) + 1;
		if (p >= end)
			break;
		path = p;
		p += strlen(p) + 1;
		ALLOC_GROW(*out, *n + 1, alloc);
		(*out)[*n].status = st[0];
		(*out)[*n].from = NULL;
		if ((st[0] == 'R' || st[0] == 'C') && p < end) {
			(*out)[*n].from = xstrdup(path);
			path = p;
			p += strlen(p) + 1;
		}
		/* a copy is a new file here, as in the work tree view */
		if (st[0] == 'C') {
			(*out)[*n].status = 'A';
			free((*out)[*n].from);
			(*out)[*n].from = NULL;
		}
		(*out)[*n].path = xstrdup(path);
		(*n)++;
	}
	sb_release(&o);
	return 0;
}

void cfiles_free(struct cfile *f, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		free(f[i].path);
		free(f[i].from);
	}
	free(f);
}

int commit_show(struct repo *r, const char *commit, struct strbuf *out)
{
	return git_out(r, out, GIT_PLAIN, "-c", "diff.noprefix=false", "show",
		       "--no-color", "--no-ext-diff", "--stat", "-p",
		       "--format=commit %H%nAuthor: %an <%ae>%n%n%w(0,4,4)%B",
		       commit, "--", NULL);
}
