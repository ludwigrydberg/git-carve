// SPDX-License-Identifier: MIT
#include "status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int status_cmp(const void *a, const void *b)
{
	return strcmp(((const struct file_status *)a)->path,
		      ((const struct file_status *)b)->path);
}

static bool is_unmerged(char x, char y)
{
	return x == 'U' || y == 'U' || (x == 'A' && y == 'A') ||
	       (x == 'D' && y == 'D');
}

size_t parse_status(const char *buf, size_t len, struct file_status **out)
{
	const char *p = buf ? buf : "", *end = buf ? p + len : p;
	struct file_status *v = NULL;
	size_t n = 0, alloc = 0;

	/* "XY <path>\0" each, and a path has one character at least */
	while (end - p > 3) {
		const char *nul = memchr(p + 3, '\0', (size_t)(end - p - 3));
		struct file_status *f;

		if (!nul)
			break;
		ALLOC_GROW(v, n + 1, alloc);
		f = &v[n++];
		f->x = p[0];
		f->y = p[1];
		f->path = xstrndup(p + 3, (size_t)(nul - p - 3));
		f->unmerged = is_unmerged(f->x, f->y);
		f->from_x = f->from_y = NULL;
		f->seen_tracked = f->ix_known = f->in_index = false;
		if (f->x == '?') {
			f->x = ' ';
			f->y = '?';
		}
		p = nul + 1;
		/* renames and copies carry the source path, skip it */
		if (p[-1] == '\0' && (v[n - 1].x == 'R' || v[n - 1].x == 'C')) {
			nul = memchr(p, '\0', (size_t)(end - p));
			p = nul ? nul + 1 : end;
		}
	}
	if (n)
		qsort(v, n, sizeof(*v), status_cmp);
	*out = v;
	return n;
}

struct file_status file_status_untracked(char *path)
{
	struct file_status f;

	memset(&f, 0, sizeof(f));
	f.path = path;
	f.x = ' ';
	f.y = '?';
	return f;
}
size_t parse_untracked(const char *buf, size_t len, struct file_status **out)
{
	const char *p = buf ? buf : "", *end = buf ? p + len : p;
	struct file_status *v = NULL;
	size_t n = 0, alloc = 0;

	while (p < end) {
		const char *nul = memchr(p, '\0', (size_t)(end - p));

		if (!nul)
			break;
		if (nul > p) {
			ALLOC_GROW(v, n + 1, alloc);
			v[n++] = file_status_untracked(
				xstrndup(p, (size_t)(nul - p)));
		}
		p = nul + 1;
	}
	if (n > 1)
		qsort(v, n, sizeof(*v), status_cmp);
	*out = v;
	return n;
}

void status_args(struct argv *a, bool untracked)
{
	/*
	 * The background scan takes no optional index lock, since it must
	 * not hold the index while a command of ours runs. The tracked
	 * status runs in the foreground and may write the refreshed index
	 * (stat data), which spares every git after it the refresh. git
	 * skips that write when another git holds the lock.
	 */
	if (untracked)
		/* with this set git's untracked cache serves -uall too */
		argv_pushl(a, "--no-optional-locks", "-c",
			   "status.showUntrackedFiles=all", NULL);
	argv_pushl(a, GIT_PLAIN, "status", "--porcelain=v1", "-z",
		   "--no-renames",
		   untracked ? "--untracked-files=all" : "--untracked-files=no",
		   NULL);
}

int load_status_mode(struct repo *r, bool untracked, struct file_status **out,
		     size_t *n)
{
	struct strbuf o = STRBUF_INIT, e = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	int rc;

	status_args(&a, untracked);
	rc = git_argv(r, &a, &o, &e);
	if (rc)
		git_fail(r, "git status", &e);
	argv_clear(&a);
	sb_release(&e);
	if (rc) {
		sb_release(&o);
		return -1;
	}
	*n = parse_status(sb_str(&o), o.len, out);
	sb_release(&o);
	return 0;
}

int load_status_paths(struct repo *r, const char *const *paths, size_t np,
		      struct file_status **out, size_t *n)
{
	struct strbuf o = STRBUF_INIT, e = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	size_t i;
	int rc;

	argv_push(&a, "--literal-pathspecs");
	status_args(&a, false);
	argv_push(&a, "--");
	for (i = 0; i < np; i++)
		argv_push(&a, paths[i]);
	rc = run_git(&a, NULL, &o, &e);
	if (rc)
		git_fail(r, "git status", &e);
	argv_clear(&a);
	sb_release(&e);
	if (rc) {
		sb_release(&o);
		return -1;
	}
	*n = parse_status(sb_str(&o), o.len, out);
	sb_release(&o);
	return 0;
}

static bool listed_path(const char *path, const char *const *paths, size_t np)
{
	size_t i;

	for (i = 0; i < np; i++)
		if (!strcmp(path, paths[i]))
			return true;
	return false;
}

size_t status_splice(struct file_status **v, size_t n, const char *const *paths,
		     size_t np, struct file_status *add, size_t nadd,
		     struct file_status **old, size_t *nold)
{
	struct file_status *res = xcalloc(n + nadd + 1, sizeof(*res));
	struct file_status *gone = xcalloc(n + 1, sizeof(*gone));
	size_t i, j = 0, k = 0, ng = 0;

	/* both lists are sorted by path, merge what stays with add */
	for (i = 0; i < n; i++) {
		if (listed_path((*v)[i].path, paths, np)) {
			gone[ng++] = (*v)[i];
			continue;
		}
		while (j < nadd && status_cmp(&add[j], &(*v)[i]) < 0)
			res[k++] = add[j++];
		res[k++] = (*v)[i];
	}
	while (j < nadd)
		res[k++] = add[j++];
	free(*v);
	free(add);
	*v = res;
	*old = gone;
	*nold = ng;
	return k;
}

int load_status(struct repo *r, struct file_status **out, size_t *n)
{
	return load_status_mode(r, true, out, n);
}

size_t merge_untracked(const struct file_status *tracked, size_t nt,
		       const struct file_status *scan, size_t ns,
		       struct file_status **out)
{
	struct file_status *v = xcalloc(nt + ns + 1, sizeof(*v));
	size_t n = 0, i, j;

	for (i = 0; i < nt; i++) {
		v[n] = tracked[i];
		v[n].from_x = v[n].from_y = NULL;
		v[n++].path = xstrdup(tracked[i].path);
	}
	for (j = 0; j < ns; j++) {
		bool dup = false;

		if (scan[j].y != '?')
			continue;
		/* tracked by now (e.g. marked intent-to-add since the scan) */
		for (i = 0; i < nt && !dup; i++)
			dup = !strcmp(tracked[i].path, scan[j].path);
		if (dup)
			continue;
		v[n] = scan[j];
		v[n].from_x = v[n].from_y = NULL;
		v[n++].path = xstrdup(scan[j].path);
	}
	if (n)
		qsort(v, n, sizeof(*v), status_cmp);
	*out = v;
	return n;
}

void free_status(struct file_status *fs, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		free(fs[i].path);
		free(fs[i].from_x);
		free(fs[i].from_y);
	}
	free(fs);
}

void exclude_pattern(const char *path, struct strbuf *out)
{
	const char *p;

	/* anchored at the top, globs and a leading # or ! taken literally */
	sb_addch(out, '/');
	for (p = path; *p; p++) {
		if (strchr("*?[\\", *p) || (p == path && strchr("#!", *p)))
			sb_addch(out, '\\');
		sb_addch(out, *p);
	}
}

int exclude_path(struct repo *r, const char *path)
{
	struct strbuf cur = STRBUF_INIT, line = STRBUF_INIT;
	char *file = repo_git_path_dup(r, "info/exclude");
	char *slash;
	int rc = -1;
	FILE *fp;

	if (!file)
		return -1;
	slash = strrchr(file, '/');
	if (slash) {
		*slash = '\0';
		mkdir(file, 0777); /* info/ may be missing */
		*slash = '/';
	}
	read_file(file, &cur);
	if (cur.len && cur.buf[cur.len - 1] != '\n')
		sb_addch(&line, '\n');
	exclude_pattern(path, &line);
	sb_addch(&line, '\n');
	fp = fopen(file, "a");
	if (!fp) {
		repo_err(r, "cannot write %s", file);
		goto out;
	}
	if (fwrite(line.buf, 1, line.len, fp) != line.len)
		repo_err(r, "cannot write %s", file);
	else
		rc = 0;
	if (fclose(fp) && !rc)
		rc = repo_err(r, "cannot write %s", file);
out:
	free(file);
	sb_release(&cur);
	sb_release(&line);
	return rc;
}

bool tree_is_clean(struct repo *r, struct strbuf *what)
{
	struct strbuf out = STRBUF_INIT;
	const char *pos, *end, *l;
	size_t len;
	bool clean;
	int n = 0;

	if (git_out(r, &out, "status", "--porcelain", "--untracked-files=no",
		    NULL)) {
		sb_release(&out);
		return false;
	}
	pos = sb_str(&out);
	end = sb_str(&out) + out.len;
	while (what && (l = next_line(&pos, end, &len))) {
		if (len < 4)
			continue;
		if (n++ < 3)
			sb_addf(what, "%s%.*s", n > 1 ? ", " : "",
				(int)(len - 3), l + 3);
	}
	if (what && n > 3)
		sb_addf(what, " and %d more", n - 3);
	clean = !out.len;
	sb_release(&out);
	return clean;
}

int refuse_dirty(struct repo *r)
{
	return repo_err(r, DIRTY_TREE_MSG);
}

bool have_staged(struct repo *r)
{
	int rc = git_status_of(r, "diff-index", "--cached", "--quiet", "HEAD",
			       "--", NULL);
	struct strbuf out = STRBUF_INIT;
	bool any;

	if (rc == 0 || rc == 1)
		return rc == 1;
	/* before the first commit anything in the index is staged */
	git_out(r, &out, "ls-files", NULL);
	any = out.len > 0;
	sb_release(&out);
	return any;
}
