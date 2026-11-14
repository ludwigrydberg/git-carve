// SPDX-License-Identifier: MIT
#include "index.h"

#include <stdlib.h>
#include <string.h>

#include "patch.h"

/* Read the index entry of path (stage 0). *mode gets e.g. "100644". */
static int index_read(struct repo *r, const char *path, struct strbuf *blob,
		      char **mode)
{
	struct strbuf out = STRBUF_INIT, spec = STRBUF_INIT;
	char *sp, *tab;
	int rc = -1;

	if (index_entry_of(r, path, &out) < 0)
		goto done;
	/* "<mode> SP <sha> SP <stage> TAB <path>" */
	tab = strchr(sb_str(&out), '\t');
	sp = strchr(sb_str(&out), ' ');
	if (!tab || !sp) {
		repo_err(r, "%s: not in the index", path);
		goto done;
	}
	if (tab[-1] != '0' || strchr(tab + 1, '\n') != strrchr(out.buf, '\n')) {
		repo_err(r, "%s: unmerged; resolve the conflict first", path);
		goto done;
	}
	if (mode)
		*mode = xstrndup(out.buf, (size_t)(sp - out.buf));
	sb_addf(&spec, "%.*s", (int)(strchr(sp + 1, ' ') - sp - 1), sp + 1);
	/* intent-to-add entries point to the (maybe absent) empty blob */
	if (!strcmp(spec.buf, "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391") ||
	    !strcmp(spec.buf, "473a0f4c3be8a93681a267e3b1e9a7dcda1185436fe141f"
			      "7749120a303721813")) {
		sb_add(blob, "", 0);
		rc = 0;
		goto done;
	}
	rc = read_blob(r, spec.buf, blob);
done:
	if (rc && mode && *mode) {
		free(*mode);
		*mode = NULL;
	}
	sb_release(&out);
	sb_release(&spec);
	return rc;
}

/* Store data as the index entry of path. The work tree is not touched. */
static int index_write(struct repo *r, const char *path, const char *mode,
		       const char *data, size_t len)
{
	struct strbuf sha = STRBUF_INIT, info = STRBUF_INIT;
	int rc = -1;

	if (git_in(r, data, len, &sha, "hash-object", "-w", "--no-filters",
		   "--stdin", NULL))
		goto out;
	sb_rtrim(&sha);
	sb_addf(&info, "%s,%s,%s", mode, sha.buf, path);
	rc = git_quiet(r, "update-index", "--cacheinfo", info.buf, NULL) ? -1 :
									   0;
out:
	sb_release(&sha);
	sb_release(&info);
	return rc;
}

struct bline {
	const char *text;
	size_t len; /* without '\n' */
	bool eol;
};

static size_t split_blob(const char *blob, size_t len, struct bline **out)
{
	const char *pos = blob ? blob : "", *end = pos + len, *l;
	size_t n = 0, alloc = 0, ll;
	struct bline *v = NULL;

	while ((l = next_line(&pos, end, &ll))) {
		ALLOC_GROW(v, n + 1, alloc);
		v[n].text = l;
		v[n].len = ll;
		v[n].eol = l + ll < end;
		n++;
	}
	*out = v;
	return n;
}

void splice_lines(const char *blob, size_t bloblen, int start, int count,
		  const char *text, size_t len, struct strbuf *out)
{
	size_t first = (size_t)start - 1;
	struct bline *bl;
	size_t after;
	size_t n;
	size_t i;

	n = split_blob(blob, bloblen, &bl);
	after = first + (size_t)count;

	for (i = 0; i < first && i < n; i++) {
		sb_add(out, bl[i].text, bl[i].len);
		/* a line followed by inserted text needs its newline */
		if (bl[i].eol || i + 1 < n || len)
			sb_addch(out, '\n');
	}
	sb_add(out, text, len);
	if (len && text[len - 1] != '\n' && after < n)
		sb_addch(out, '\n');
	for (i = after; i < n; i++) {
		sb_add(out, bl[i].text, bl[i].len);
		if (bl[i].eol)
			sb_addch(out, '\n');
	}
	free(bl);
}

void region_release(struct region *rg)
{
	sb_release(&rg->text);
}

/*
 * Walk the index lines and the diff rows in file order. Every step may
 * consume an index line, produce a line of the result, or both. The steps
 * from the first to the last selected row form the region.
 */
struct walk {
	enum { BEFORE, IN, AFTER } state;
	int consumed_before, consumed;
	size_t produced_after;
	struct rline {
		const char *text;
		size_t len;
		bool no_eol;
	} *res;
	size_t nres, ares;
};

static void walk_step(struct walk *w, bool consume, bool produce,
		      const char *text, size_t len, bool no_eol)
{
	if (consume) {
		if (w->state == BEFORE)
			w->consumed_before++;
		else if (w->state == IN)
			w->consumed++;
	}
	if (!produce)
		return;
	if (w->state == AFTER) {
		w->produced_after++;
	} else if (w->state == IN) {
		ALLOC_GROW(w->res, w->nres + 1, w->ares);
		w->res[w->nres++] = (struct rline){ text, len, no_eol };
	}
}

int staged_region(const struct fdiff *fd, bool cached, const char *blob,
		  size_t bloblen, struct region *rg, struct strbuf *err)
{
	struct walk w = { BEFORE, 0, 0, 0, NULL, 0, 0 };
	size_t bi = 0, h, j, nsel = 0, seen = 0, k;
	size_t *ord = NULL;
	struct bline *bl;
	char idx_op;
	size_t n;
	n = split_blob(blob, bloblen, &bl);
	idx_op = cached ? '+' : '-'; /* change op of index lines */

	for (h = 0; h < fd->nhunks; h++)
		for (j = 0; j < fd->hunks[h].nlines; j++)
			if (fd->hunks[h].lines[j].sel)
				nsel++;

	if (!nsel) {
		free(bl);
		sb_adds(err, "nothing selected");
		return -1;
	}

	for (h = 0; h < fd->nhunks; h++) {
		const struct hunk *hk = &fd->hunks[h];
		size_t hfirst;
		int start;
		int cnt;

		start = cached ? hk->new_start : hk->old_start;
		cnt = cached ? hk->new_count : hk->old_count;
		hfirst = cnt ? (size_t)start - 1 : (size_t)start;

		for (; bi < hfirst && bi < n; bi++)
			walk_step(&w, true, true, bl[bi].text, bl[bi].len,
				  !bl[bi].eol);
		ord = xrealloc(ord, (hk->nlines + 1) * sizeof(*ord));
		hunk_order(hk, ord);
		for (j = 0; j < hk->nlines; j++) {
			const struct dline *d = &hk->lines[ord[j]];
			bool on_index;
			bool produce;

			on_index = d->op == ' ' || d->op == idx_op;

			if (d->sel && !seen++)
				w.state = IN;
			if (cached)
				produce = on_index;
			else
				produce = d->op == ' ' ||
					  (d->op == '-' && !d->sel) ||
					  (d->op == '+' && d->sel);
			walk_step(&w, on_index, produce, d->text, d->len,
				  d->no_eol);
			if (on_index)
				bi++;
			if (d->sel && seen == nsel)
				w.state = AFTER;
		}
	}
	for (; bi < n; bi++)
		walk_step(&w, true, true, bl[bi].text, bl[bi].len, !bl[bi].eol);

	rg->start = w.consumed_before + 1;
	rg->count = w.consumed;
	sb_reset(&rg->text);
	for (k = 0; k < w.nres; k++) {
		const struct rline *l = &w.res[k];

		sb_add(&rg->text, l->text, l->len);
		/* only the very last line of the file may lack a newline */
		if (!(k + 1 == w.nres && !w.produced_after && l->no_eol))
			sb_addch(&rg->text, '\n');
	}
	if (!rg->text.buf)
		sb_add(&rg->text, "", 0);
	free(w.res);
	free(ord);
	free(bl);
	return 0;
}

/* Replace a region staged_region worked out of path's index entry. */
static int index_replace_region(struct repo *r, const char *path,
				const struct region *rg, const char *content,
				size_t len)
{
	struct strbuf blob = STRBUF_INIT, out = STRBUF_INIT;
	char *mode = NULL;
	int rc = -1;

	if (index_read(r, path, &blob, &mode))
		goto out;
	splice_lines(blob.buf, blob.len, rg->start, rg->count, content, len,
		     &out);
	rc = index_write(r, path, mode, sb_str(&out), out.len);
out:
	free(mode);
	sb_release(&blob);
	sb_release(&out);
	return rc;
}

int set_staged(struct repo *r, const char *path, const char *spec, bool cached,
	       const char *content, size_t len, struct strbuf *show)
{
	struct strbuf err = STRBUF_INIT, blob = STRBUF_INIT;
	struct region rg = { 0, 0, STRBUF_INIT };
	struct fdiff *fd = NULL;
	bool added_ita = false;
	int rc = -1;

	if (!cached && content) {
		int in = path_in_index(r, path);

		if (in < 0)
			return -1;
		if (!in && path_in_worktree(path)) {
			if (intent_to_add(r, path))
				return -1;
			added_ita = true;
		}
	}
	if (load_diff(r, path, cached, &fd))
		goto out;
	if (!fd) {
		repo_err(r, "%s: no %s changes", path,
			 cached ? "staged" : "unstaged");
		goto out;
	}
	if (!fdiff_line_ops(fd, NULL) && !fd->mode_only) {
		const char *why;

		fdiff_line_ops(fd, &why);
		repo_err(r, "%s: %s", path, why);
		goto out;
	}
	if (sel_apply_ex(fd, spec, true, &err) < 0) {
		repo_err(r, "%s", err.buf);
		goto out;
	}
	if (path_in_index(r, path) == 1 && index_read(r, path, &blob, NULL))
		goto out;
	if (staged_region(fd, cached, sb_str(&blob), blob.len, &rg, &err)) {
		repo_err(r, "%s", err.buf);
		goto out;
	}
	if (!content) {
		sb_add(show, rg.text.buf, rg.text.len);
		rc = 0;
		goto out;
	}
	rc = index_replace_region(r, path, &rg, content, len);
out:
	if (rc && added_ita)
		git_status_of(r, "update-index", "--force-remove", "--", path,
			      NULL);
	region_release(&rg);
	fdiff_free(fd);
	free(fd);
	sb_release(&err);
	sb_release(&blob);
	return rc;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int paths_in_index(struct repo *r, const char *const *paths, size_t n, bool *in)
{
	struct strbuf out = STRBUF_INIT;
	const char **v = NULL, *p, *end;
	size_t i, nv = 0, alloc = 0;
	struct argv a = ARGV_INIT;
	int rc = -1;

	if (!n)
		return 0;
	argv_pushl(&a, "--literal-pathspecs", "ls-files", "-z", NULL);
	if (n <= PATHSPEC_ARGS_MAX) {
		argv_push(&a, "--");
		for (i = 0; i < n; i++)
			argv_push(&a, paths[i]);
	}
	if (run_git(&a, NULL, &out, NULL))
		goto out;
	p = sb_str(&out);
	end = p + out.len;
	for (; p < end; p += strlen(p) + 1) {
		ALLOC_GROW(v, nv + 1, alloc);
		v[nv++] = p;
	}
	/* in index order already, sorted again in case a path is odd */
	if (nv)
		qsort(v, nv, sizeof(*v), cmp_str);
	for (i = 0; i < n; i++)
		in[i] = nv && bsearch(&paths[i], v, nv, sizeof(*v), cmp_str);
	rc = 0;
out:
	free(v);
	argv_clear(&a);
	sb_release(&out);
	return rc;
}
