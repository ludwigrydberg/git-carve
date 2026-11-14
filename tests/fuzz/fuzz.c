// SPDX-License-Identifier: MIT
/*
 * Randomized invariant tests for line staging, run inside a scratch git
 * repository (cwd). For each iteration we commit a random file, write a
 * random edit of it, pick random selections and check the invariants
 * (CONTRIBUTING.md defines them all).
 *
 *   I1  stage(S) then unstage(everything staged) restores the index blob
 *   I2  staging every line equals "git add" (same blob id)
 *   I3  the staged blob equals an in-memory reference model, for both
 *       stage and partial unstage
 *   I4  unselected changes remain in "git diff" (unique-line mode)
 *   I5  the work tree file is never modified
 *   I9  undoing the steps one by one restores each earlier index
 *
 * FUZZ_SEED (default 1) and FUZZ_ITERS (default 300) control the run.
 * FUZZ_START skips to an iteration to reproduce a failure quickly.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "diff.h"
#include "git.h"
#include "index.h"
#include "cdiff.h"
#include "opstate.h"
#include "patch.h"
#include "rebase.h"
#include "series.h"
#include "split.h"
#include "status.h"
#include "undo.h"

static uint64_t rng_state;

static uint64_t rnd(void)
{
	/* splitmix64, for identical sequences on every platform */
	uint64_t z = (rng_state += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static int rnd_n(int n)
{
	return n <= 0 ? 0 : (int)(rnd() % (uint64_t)n);
}

struct text {
	char **lines;
	size_t n, alloc;
	bool final_eol;
};

static void text_push(struct text *t, const char *s)
{
	ALLOC_GROW(t->lines, t->n + 1, t->alloc);
	t->lines[t->n++] = xstrdup(s);
}

static void text_insert(struct text *t, size_t at, const char *s)
{
	ALLOC_GROW(t->lines, t->n + 1, t->alloc);
	memmove(t->lines + at + 1, t->lines + at, (t->n - at) * sizeof(char *));
	t->lines[at] = xstrdup(s);
	t->n++;
}

static void text_remove(struct text *t, size_t at)
{
	free(t->lines[at]);
	memmove(t->lines + at, t->lines + at + 1,
		(t->n - at - 1) * sizeof(char *));
	t->n--;
}

static void text_free(struct text *t)
{
	size_t i;

	for (i = 0; i < t->n; i++)
		free(t->lines[i]);
	free(t->lines);
	memset(t, 0, sizeof(*t));
}

static void text_copy(struct text *dst, const struct text *src)
{
	size_t i;

	memset(dst, 0, sizeof(*dst));
	for (i = 0; i < src->n; i++)
		text_push(dst, src->lines[i]);
	dst->final_eol = src->final_eol;
}

static void text_serialize(const struct text *t, struct strbuf *sb)
{
	size_t i;

	for (i = 0; i < t->n; i++) {
		sb_adds(sb, t->lines[i]);
		if (i + 1 < t->n || t->final_eol)
			sb_addch(sb, '\n');
	}
}

static bool unique_mode;
static unsigned long unique_counter;

static void random_line(char *buf, size_t sz)
{
	static const char *vocab[] = { "a",	   "b", "c",   "",  "x y",
				       "\tint i;", "}", "a\r", "{", "b" };

	if (unique_mode)
		snprintf(buf, sz, "u%lu", unique_counter++);
	else
		snprintf(buf, sz, "%s", vocab[rnd_n((int)ARRAY_SIZE(vocab))]);
}

static void random_text(struct text *t)
{
	int n = rnd_n(4) == 0 ? rnd_n(4) : rnd_n(30);
	char buf[64];

	memset(t, 0, sizeof(*t));
	while (n--) {
		random_line(buf, sizeof(buf));
		text_push(t, buf);
	}
	t->final_eol = rnd_n(5) != 0;
}

static void random_edit(struct text *t)
{
	int ops = 1 + rnd_n(4);
	char buf[64];

	while (ops--) {
		int kind = rnd_n(7);
		size_t at;
		int len;

		len = 1 + rnd_n(5);
		at = (size_t)rnd_n((int)t->n + 1);

		switch (kind) {
		case 0:
		case 1: /* insert */
			while (len--) {
				random_line(buf, sizeof(buf));
				text_insert(t, at, buf);
			}
			break;
		case 2: /* delete */
			while (len-- && at < t->n)
				text_remove(t, at);
			break;
		case 3:
		case 4:
		case 5: /* modify */
			while (len-- && at < t->n) {
				random_line(buf, sizeof(buf));
				free(t->lines[at]);
				t->lines[at++] = xstrdup(buf);
			}
			break;
		case 6:
			t->final_eol = !t->final_eol;
			break;
		}
	}
}

static void parse_text(const char *buf, size_t len, struct text *t)
{
	const char *pos = buf ? buf : "", *end = pos + len, *l;
	size_t ll;

	memset(t, 0, sizeof(*t));
	while ((l = next_line(&pos, end, &ll))) {
		char *s = xstrndup(l, ll);

		text_push(t, s);
		free(s);
	}
	t->final_eol = len == 0 || buf[len - 1] == '\n';
}

/* A line of the reference model, its text and whether it lacked a newline. */
struct mline {
	const char *text;
	size_t len;
	bool no_eol;
};

/*
 * Apply the selected lines of a diff to base, which is the diff's old side
 * (stage) or new side (unstage), without building or applying a patch.
 */
static void model(const struct text *base, const struct fdiff *fd, bool unstage,
		  struct strbuf *out)
{
	struct mline *res = xcalloc(base->n + 4096, sizeof(*res));
	char base_op = unstage ? '+' : '-';
	size_t nres = 0, bi = 0, h, j;

	for (h = 0; fd && h < fd->nhunks; h++) {
		const struct hunk *hk = &fd->hunks[h];
		size_t first;
		int start;
		int count;

		start = unstage ? hk->new_start : hk->old_start;
		count = unstage ? hk->new_count : hk->old_count;
		first = count ? (size_t)start - 1 : (size_t)start;

		while (bi < first) {
			res[nres++] = (struct mline){
				base->lines[bi], strlen(base->lines[bi]),
				bi + 1 == base->n && !base->final_eol
			};
			bi++;
		}
		/*
		 * Walk in "pairs" order. A block of removals followed by
		 * additions is taken as -r0 +a0 -r1 +a1 ..., with the
		 * leftovers after.
		 */
		size_t *seq = xcalloc(hk->nlines + 1, sizeof(*seq)), ns = 0;
		size_t *rm = xcalloc(hk->nlines + 1, sizeof(*rm)), nrm;
		size_t *ad = xcalloc(hk->nlines + 1, sizeof(*ad)), nad, t;

		for (j = 0; j < hk->nlines;) {
			nrm = nad = 0;
			while (j < hk->nlines && hk->lines[j].op == '-')
				rm[nrm++] = j++;
			while (nrm && j < hk->nlines && hk->lines[j].op == '+')
				ad[nad++] = j++;
			if (!nrm) {
				seq[ns++] = j++;
				continue;
			}
			for (t = 0; t < nrm || t < nad; t++) {
				if (t < nrm && t < nad) {
					seq[ns++] = rm[t];
					seq[ns++] = ad[t];
				}
			}
			for (t = nad < nrm ? nad : nrm; t < nrm; t++)
				seq[ns++] = rm[t];
			for (t = nad < nrm ? nad : nrm; t < nad; t++)
				seq[ns++] = ad[t];
		}
		for (t = 0; t < ns; t++) {
			const struct dline *d = &hk->lines[seq[t]];
			bool on_base;

			on_base = d->op == ' ' || d->op == base_op;

			if (on_base) {
				bool keep = d->op == ' ' || !d->sel;

				if (keep)
					res[nres++] = (struct mline){
						base->lines[bi],
						strlen(base->lines[bi]),
						bi + 1 == base->n &&
							!base->final_eol
					};
				bi++;
			} else if (d->sel) {
				res[nres++] = (struct mline){ d->text, d->len,
							      d->no_eol };
			}
		}
		free(seq);
		free(rm);
		free(ad);
	}
	while (bi < base->n) {
		res[nres++] =
			(struct mline){ base->lines[bi],
					strlen(base->lines[bi]),
					bi + 1 == base->n && !base->final_eol };
		bi++;
	}
	for (j = 0; j < nres; j++) {
		sb_add(out, res[j].text, res[j].len);
		if (j + 1 < nres || !res[j].no_eol)
			sb_addch(out, '\n');
	}
	free(res);
}

static struct repo repo;
static const char *cur_path;
static bool cur_new_file; /* a file new in the index this iteration */
static unsigned long cur_iter;
static uint64_t cur_seed;

static void fatal(const char *what)
{
	fprintf(stderr,
		"FUZZ FAILURE: %s\n  iteration %lu (seed %llu), path %s\n"
		"  reproduce: FUZZ_SEED=%s FUZZ_START=%lu FUZZ_ITERS=1\n",
		what, cur_iter, (unsigned long long)cur_seed, cur_path,
		getenv("FUZZ_SEED") ? getenv("FUZZ_SEED") : "1", cur_iter);
	if (repo.err.len)
		fprintf(stderr, "  last error: %s\n", repo.err.buf);
	exit(1);
}

static void read_index_blob(const char *path, struct strbuf *out)
{
	struct strbuf spec = STRBUF_INIT;

	sb_addf(&spec, ":%s", path);
	if (git_out(&repo, out, "cat-file", "blob", spec.buf, NULL))
		fatal("cat-file");
	sb_release(&spec);
}

static char *blob_id(const char *path)
{
	struct strbuf spec = STRBUF_INIT;
	char *id;

	sb_addf(&spec, ":%s", path);
	id = git_line(&repo, "rev-parse", "-q", "--verify", spec.buf, NULL);
	sb_release(&spec);
	return id ? id : xstrdup("none");
}

static void check_worktree(const struct strbuf *expect)
{
	struct strbuf now = STRBUF_INIT;

	if (read_file(cur_path, &now) || now.len != expect->len ||
	    memcmp(sb_str(&now), sb_str(expect), now.len))
		fatal("I5: work tree file modified");
	sb_release(&now);
}

static void dump_diff(const char *label, const struct fdiff *fd)
{
	struct strbuf sb = STRBUF_INIT;
	size_t h, j;

	if (!fd)
		return;
	for (h = 0; h < fd->nhunks; h++)
		for (j = 0; j < fd->hunks[h].nlines; j++) {
			const struct dline *d = &fd->hunks[h].lines[j];

			sb_addf(&sb, "  %c%c %d %d |%.*s|%s\n",
				d->sel ? '*' : ' ', d->op, d->old_no, d->new_no,
				(int)d->len, d->text,
				d->no_eol ? " no_eol" : "");
		}
	fprintf(stderr, "%s:\n%s", label, sb_str(&sb));
	sb_release(&sb);
}

static void check_index_equals(const struct strbuf *expect, const char *inv,
			       const struct fdiff *fd)
{
	struct strbuf got = STRBUF_INIT;

	read_index_blob(cur_path, &got);
	if (got.len != expect->len ||
	    memcmp(sb_str(&got), sb_str(expect), got.len)) {
		fprintf(stderr, "--- index:\n%s\n--- expected:\n%s\n---\n",
			got.buf, sb_str(expect));
		dump_diff("diff with selection", fd);
		fatal(inv);
	}
	sb_release(&got);
}

/* Select a random subset of change lines, at least one, and build a spec. */
static size_t random_selection(struct fdiff *fd, struct strbuf *spec, bool all)
{
	int pct = all ? 100 : 10 + rnd_n(90);
	size_t h, j, n = 0, total = 0;

	for (h = 0; h < fd->nhunks; h++)
		for (j = 0; j < fd->hunks[h].nlines; j++)
			if (fd->hunks[h].lines[j].op != ' ')
				total++;
	if (!total)
		return 0;
	while (!n) {
		for (h = 0; h < fd->nhunks; h++) {
			for (j = 0; j < fd->hunks[h].nlines; j++) {
				struct dline *d = &fd->hunks[h].lines[j];

				if (d->op == ' ' || d->sel)
					continue;
				if (rnd_n(100) >= pct)
					continue;
				d->sel = true;
				n++;
				if (spec->len)
					sb_addch(spec, ',');
				if (d->op == '-')
					sb_addf(spec, "o%d", d->old_no);
				else
					sb_addf(spec, "n%d", d->new_no);
			}
		}
	}
	return n;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Collect "op text" of change lines (optionally only unselected). */
static size_t collect(const struct fdiff *fd, bool unsel_only, char ***out)
{
	size_t h, j, n = 0, alloc = 0;
	char **v = NULL;

	for (h = 0; fd && h < fd->nhunks; h++)
		for (j = 0; j < fd->hunks[h].nlines; j++) {
			const struct dline *d = &fd->hunks[h].lines[j];
			struct strbuf s = STRBUF_INIT;

			if (d->op == ' ' || (unsel_only && d->sel))
				continue;
			sb_addch(&s, d->op);
			sb_add(&s, d->text, d->len);
			ALLOC_GROW(v, n + 1, alloc);
			v[n++] = sb_detach(&s);
		}
	/* drop "-x" "+x" pairs, which change only the end of line */
	for (h = 0; h < n; h++) {
		if (!v[h] || v[h][0] != '-')
			continue;
		for (j = 0; j < n; j++) {
			if (v[j] && v[j][0] == '+' &&
			    !strcmp(v[h] + 1, v[j] + 1)) {
				free(v[h]);
				free(v[j]);
				v[h] = v[j] = NULL;
				break;
			}
		}
	}
	for (h = j = 0; h < n; h++)
		if (v[h])
			v[j++] = v[h];
	n = j;
	if (n)
		qsort(v, n, sizeof(*v), cmp_str);
	*out = v;
	return n;
}

static void free_strs(char **v, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

static void check_remaining(const struct fdiff *before)
{
	struct fdiff *after = NULL;
	size_t na, nb, i;
	char **a, **b;

	if (load_diff(&repo, cur_path, false, &after))
		fatal("load_diff after stage");
	na = collect(before, true, &a);
	nb = collect(after, false, &b);
	for (i = 0; i < na && i < nb; i++)
		if (strcmp(a[i], b[i]))
			break;
	if (na != nb || i < na) {
		dump_diff("before", before);
		dump_diff("after", after);
		fatal("I4: unselected changes not preserved");
	}
	free_strs(a, na);
	free_strs(b, nb);
	fdiff_free(after);
	free(after);
}

/*
 * I9. Every staging step is recorded for undo, and undoing them one by one
 * afterwards gives back each earlier index (write-tree), newest first,
 * without touching the work tree. undo_trees is an independent record of
 * the index tree before every step that changed it.
 */
static char *undo_trees[16];
static int nundo;
static bool undo_on;

/*
 * Begin a step of op. A line stage or unstage of a file that is not new is
 * recorded as its patch where the TUI would do that (undo_defer).
 */
static char *step_before; /* the index before the step (write-tree) */

static void step_begin(struct undo_state *u, const char *op, bool lines)
{
	if (!undo_on)
		return;
	/* the record of what undo must give back, our own and not undo's */
	free(step_before);
	step_before = git_line(&repo, "write-tree", NULL);
	u->as_patch = lines && !cur_new_file;
	undo_begin(&repo, u, op);
}

static void step_end(struct undo_state *u, const char *op)
{
	char *after;

	if (!undo_on)
		return;
	after = git_line(&repo, "write-tree", NULL);
	if (step_before && after && strcmp(step_before, after) &&
	    nundo < (int)ARRAY_SIZE(undo_trees))
		undo_trees[nundo++] = xstrdup(step_before);
	free(after);
	undo_end(&repo, u, op, cur_path);
	if (repo.warn.len) {
		fprintf(stderr, "%s", repo.warn.buf);
		fatal("I9: recording for undo failed");
	}
}

static void check_undo_all(const struct strbuf *worktree)
{
	struct undo_entry *v;
	size_t n;

	if (!undo_on)
		return;
	if (undo_list(&repo, &v, &n))
		fatal("I9: undo --list failed");
	undo_list_free(v, n);
	if ((int)n != nundo) {
		fprintf(stderr, "%zu entries, %d changes\n", n, nundo);
		fatal("I9: not one undo entry per change");
	}
	while (nundo > 0) {
		char *t;

		if (undo_last(&repo, false, NULL))
			fatal("I9: undo failed");
		t = git_line(&repo, "write-tree", NULL);
		nundo--;
		if (!t || strcmp(t, undo_trees[nundo])) {
			fprintf(stderr, "index %s, expected %s\n", t ? t : "?",
				undo_trees[nundo]);
			fatal("I9: undo did not restore the index");
		}
		free(t);
		free(undo_trees[nundo]);
		check_worktree(worktree);
	}
}

static bool iteration(unsigned long it, uint64_t seed)
{
	struct strbuf old_s = STRBUF_INIT, new_s = STRBUF_INIT,
		      expect = STRBUF_INIT, spec = STRBUF_INIT,
		      blob = STRBUF_INIT;
	struct diff_opts dopts = DIFF_OPTS_INIT;
	struct undo_state u = { 0 };
	struct text old, neu, idx;
	char path[64], *idx0, *id;
	struct fdiff *fd = NULL;
	bool new_file;
	bool ctx_mode;

	rng_state = seed;
	cur_iter = it;
	cur_seed = seed;
	undo_on = it % 2 == 0;
	/*
	 * every other one of those recorded as the TUI does, kept back and
	 * then written together (undo writes them first)
	 */
	repo.undo_defer = it % 4 == 0;
	nundo = 0;
	unique_mode = rnd_n(2);
	new_file = rnd_n(8) == 0;
	cur_new_file = new_file;
	snprintf(path, sizeof(path), "%s%lu.txt", new_file ? "new" : "f",
		 it % 4);
	cur_path = path;

	random_text(&old);
	if (new_file) {
		text_free(&old);
		memset(&old, 0, sizeof(old));
		old.final_eol = true;
	}
	text_copy(&neu, &old);
	random_edit(&neu);
	text_serialize(&old, &old_s);
	text_serialize(&neu, &new_s);
	/*
	 * A trailing empty line without a newline cannot be represented,
	 * since it serializes like no line at all. Parse again to get
	 * canonical texts.
	 */
	text_free(&old);
	text_free(&neu);
	parse_text(old_s.buf, old_s.len, &old);
	parse_text(new_s.buf, new_s.len, &neu);
	if (old_s.len == new_s.len &&
	    !memcmp(sb_str(&old_s), sb_str(&new_s), old_s.len))
		goto skip;

	/* HEAD and index hold old, the work tree holds new */
	if (new_file) {
		git_status_of(&repo, "rm", "-q", "--cached", "--ignore-unmatch",
			      "--", path, NULL);
		git_status_of(&repo, "commit", "-q", "-m", "rm", NULL);
	} else {
		if (write_file(path, old_s.buf, old_s.len) ||
		    git_quiet(&repo, "add", "--", path, NULL) ||
		    git_quiet(&repo, "commit", "-q", "--allow-empty", "-m",
			      "it", NULL))
			fatal("setup");
	}
	if (write_file(path, new_s.buf, new_s.len))
		fatal("write");
	idx0 = blob_id(path);

	/*
	 * I3 (stage), I4 and I5. Every third iteration stages what the TUI
	 * shows with other view options, -U0 to -U5 and sometimes -W.
	 */
	dopts.context = (int)(it / 3 % 6);
	dopts.function_context = it % 7 == 1;
	ctx_mode = it % 3 == 1;
	if (ctx_mode && new_file && intent_to_add(&repo, path))
		fatal("intent-to-add");
	if (load_diff_opts(&repo, path, false, ctx_mode ? &dopts : NULL, &fd) ||
	    !fd)
		fatal("load unstaged diff");
	random_selection(fd, &spec, false);
	model(&old, fd, false, &expect);
	step_begin(&u, "stage", true);
	if (ctx_mode ? apply_selection(&repo, fd, PATCH_STAGE) :
		       stage_lines(&repo, path, spec.buf, PATCH_STAGE)) {
		dump_diff("diff", fd);
		fprintf(stderr, "spec: %s\n", spec.buf);
		fatal("stage failed");
	}
	step_end(&u, "stage");
	check_worktree(&new_s);
	check_index_equals(&expect, "I3: staged blob differs from model", fd);
	if (unique_mode)
		check_remaining(fd);
	fdiff_free(fd);
	free(fd);
	fd = NULL;

	/* I3 (unstage): random subset of the staged diff */
	read_index_blob(path, &blob);
	parse_text(blob.buf, blob.len, &idx);
	/*
	 * The staged diff may be empty, since a selected pair can cancel
	 * out, as when dropping a final newline while unselected lines
	 * stay after it.
	 */
	if (load_diff(&repo, path, true, &fd))
		fatal("load staged diff");
	sb_reset(&spec);
	sb_reset(&expect);
	if (fd && fdiff_line_ops(fd, NULL)) {
		random_selection(fd, &spec, false);
		model(&idx, fd, true, &expect);
		step_begin(&u, "unstage", true);
		if (stage_lines(&repo, path, spec.buf, PATCH_UNSTAGE)) {
			dump_diff("staged diff", fd);
			fprintf(stderr, "spec: %s\n", spec.buf);
			fatal("partial unstage failed");
		}
		step_end(&u, "unstage");
		check_worktree(&new_s);
		check_index_equals(&expect,
				   "I3: unstaged blob differs from model", fd);
	}
	text_free(&idx);
	fdiff_free(fd);
	free(fd);
	fd = NULL;

	/* I1, unstage everything that is staged, line by line */
	if (load_diff(&repo, path, true, &fd))
		fatal("load staged diff (2)");
	if (fd && fdiff_line_ops(fd, NULL)) {
		sb_reset(&spec);
		random_selection(fd, &spec, true);
		step_begin(&u, "unstage", true);
		if (stage_lines(&repo, path, spec.buf, PATCH_UNSTAGE))
			fatal("full unstage failed");
		step_end(&u, "unstage");
		check_worktree(&new_s);
		id = blob_id(path);
		if (strcmp(id, idx0) &&
		    !(new_file && strcmp(idx0, "none") == 0)) {
			fprintf(stderr, "before %s after %s\n", idx0, id);
			fatal("I1: stage + unstage did not restore the index");
		}
		free(id);
	}
	fdiff_free(fd);
	free(fd);
	fd = NULL;

	/*
	 * set-staged. Writing back the region "as it will be staged"
	 * unchanged must equal staging the selection (I3), and the work
	 * tree stays untouched (I5).
	 */
	if (load_diff(&repo, path, false, &fd))
		fatal("load unstaged diff (3)");
	if (fd && fdiff_line_ops(fd, NULL)) {
		struct strbuf show = STRBUF_INIT, base = STRBUF_INIT;
		struct text bt;

		if (path_in_index(&repo, path) == 1)
			read_index_blob(path, &base);
		parse_text(sb_str(&base), base.len, &bt);
		sb_reset(&spec);
		sb_reset(&expect);
		random_selection(fd, &spec, false);
		model(&bt, fd, false, &expect);
		if (set_staged(&repo, path, spec.buf, false, NULL, 0, &show))
			fatal("set-staged --print failed");
		step_begin(&u, "set-staged", false);
		if (set_staged(&repo, path, spec.buf, false, sb_str(&show),
			       show.len, NULL)) {
			fprintf(stderr, "spec: %s\n", spec.buf);
			fatal("set-staged failed");
		}
		step_end(&u, "set-staged");
		check_worktree(&new_s);
		check_index_equals(&expect,
				   "set-staged of the region as shown "
				   "differs from staging it",
				   fd);
		text_free(&bt);
		sb_release(&show);
		sb_release(&base);
	}
	fdiff_free(fd);
	free(fd);
	fd = NULL;

	/* I2, staging every line equals git add */
	if (load_diff(&repo, path, false, &fd))
		fatal("load unstaged diff (2)");
	if (fd && fdiff_line_ops(fd, NULL)) {
		char *want;

		sb_reset(&spec);
		random_selection(fd, &spec, true);
		step_begin(&u, "stage", true);
		if (stage_lines(&repo, path, spec.buf, PATCH_STAGE))
			fatal("stage all failed");
		step_end(&u, "stage");
		check_worktree(&new_s);
		id = blob_id(path);
		want = git_line(&repo, "hash-object", "--", path, NULL);
		if (!want || strcmp(id, want)) {
			fprintf(stderr, "index %s, git add %s\n", id,
				want ? want : "?");
			fatal("I2: staging all lines differs from git add");
		}
		free(want);
		free(id);
	}
	fdiff_free(fd);
	free(fd);
	fd = NULL;
	check_undo_all(&new_s);
	free(idx0);
	text_free(&old);
	text_free(&neu);
	sb_release(&old_s);
	sb_release(&new_s);
	sb_release(&expect);
	sb_release(&spec);
	sb_release(&blob);
	git_status_of(&repo, "reset", "-q", "--hard", NULL);
	/* after undo a new file is untracked again, and reset leaves it */
	if (new_file)
		unlink(path);
	return true;
skip:
	text_free(&old);
	text_free(&neu);
	sb_release(&old_s);
	sb_release(&new_s);
	return false;
}

/*
 * I6 and I8. Build a random commit of several files and split it. Either
 * carve it into random pieces and finish, where the tree must equal the
 * original, or abort at a random point, where HEAD, index and work tree
 * must be restored.
 */
static bool split_iteration(unsigned long it, uint64_t seed)
{
	struct strbuf content[3], state = STRBUF_INIT;
	char paths[3][16], *orig, *otree, *t;
	int nfiles = 1 + rnd_n(3);
	bool changed = false;
	int abort_at;
	int round;
	int f;

	rng_state = seed;
	cur_iter = it;
	cur_seed = seed;
	unique_mode = rnd_n(2);
	cur_path = "(split)";
	for (f = 0; f < nfiles; f++) {
		int kind = rnd_n(10); /* 0 new file, 1 deleted */

		snprintf(paths[f], sizeof(paths[f]), "s%d.txt", f);
		struct strbuf sa = STRBUF_INIT;
		struct text a, b;
		content[f] = (struct strbuf)STRBUF_INIT;
		random_text(&a);
		text_copy(&b, &a);
		random_edit(&b);
		text_serialize(&a, &sa);
		text_serialize(&b, &content[f]);
		if (kind == 0 || !path_in_worktree(paths[f])) {
			/* the fat commit adds this file */
			git_status_of(&repo, "rm", "-q", "--cached",
				      "--ignore-unmatch", "--", paths[f], NULL);
			unlink(paths[f]);
		} else if (write_file(paths[f], sa.buf ? sa.buf : "", sa.len) ||
			   git_quiet(&repo, "add", "--", paths[f], NULL)) {
			fatal("split setup");
		}
		text_free(&a);
		text_free(&b);
		sb_release(&sa);
		if (kind == 1 && path_in_worktree(paths[f])) {
			sb_release(&content[f]);
			content[f].buf = NULL; /* deleted by the fat commit */
		}
	}
	if (git_quiet(&repo, "commit", "-q", "--allow-empty", "-m", "base",
		      NULL))
		fatal("split base commit");
	for (f = 0; f < nfiles; f++) {
		if (!content[f].buf) {
			if (path_in_worktree(paths[f]))
				changed = true;
			unlink(paths[f]);
			continue;
		}
		if (write_file(paths[f], content[f].buf, content[f].len))
			fatal("split write");
		changed = true;
	}
	if (git_quiet(&repo, "add", "-A", NULL) ||
	    git_status_of(&repo, "diff-index", "--cached", "--quiet", "HEAD",
			  NULL) == 0) {
		git_status_of(&repo, "reset", "-q", "--hard", NULL);
		for (f = 0; f < nfiles; f++)
			sb_release(&content[f]);
		return false;
	}
	(void)changed;
	if (git_quiet(&repo, "commit", "-q", "-m", "fat", "-m", "body", NULL))
		fatal("fat commit");
	orig = git_line(&repo, "rev-parse", "HEAD", NULL);
	otree = git_line(&repo, "rev-parse", "HEAD^{tree}", NULL);
	for (f = 0; f < nfiles; f++) {
		sb_addf(&state, "%s:", paths[f]);
		if (content[f].buf)
			sb_add(&state, content[f].buf, content[f].len);
		sb_addch(&state, '\n');
	}

	if (split_start(&repo, NULL))
		fatal("split_start");
	sb_reset(&repo.warn);
	abort_at = rnd_n(3) == 0 ? rnd_n(4) : -1;
	for (round = 0;; round++) {
		struct commit_opts co = { .message = "part", .quiet = true };
		bool any = false;

		if (round == abort_at) {
			if (split_abort(&repo, false))
				fatal("split_abort");
			t = git_line(&repo, "rev-parse", "HEAD", NULL);
			if (!t || strcmp(t, orig))
				fatal("I8: HEAD not restored");
			free(t);
			t = git_line(&repo, "write-tree", NULL);
			if (!t || strcmp(t, otree))
				fatal("I8: index not restored");
			free(t);
			for (f = 0; f < nfiles; f++) {
				bool exists = path_in_worktree(paths[f]);
				struct strbuf now = STRBUF_INIT;

				if (exists != !!content[f].buf)
					fatal("I8: file existence differs");
				if (exists && (read_file(paths[f], &now) ||
					       now.len != content[f].len ||
					       memcmp(sb_str(&now),
						      content[f].buf, now.len)))
					fatal("I8: work tree not restored");
				sb_release(&now);
			}
			break;
		}
		if (round > 60)
			fatal("split did not converge");
		for (f = 0; f < nfiles; f++) {
			struct strbuf spec = STRBUF_INIT;
			struct fdiff *fd = NULL;

			cur_path = paths[f];
			if (!path_in_worktree(paths[f]) &&
			    path_in_index(&repo, paths[f]) != 1)
				continue;
			if (load_diff(&repo, paths[f], false, &fd))
				fatal("split load_diff");
			if (!fd)
				continue;
			if (!rnd_n(3))
				goto next; /* leave this file for later */
			if (fdiff_line_ops(fd, NULL) && rnd_n(4)) {
				random_selection(fd, &spec, false);
				if (stage_lines(&repo, paths[f], spec.buf,
						PATCH_STAGE))
					fatal("split stage_lines");
			} else if (stage_file(&repo, paths[f])) {
				fatal("split stage_file");
			}
			any = true;
next:
			fdiff_free(fd);
			free(fd);
			sb_release(&spec);
		}
		if (any && have_staged(&repo) && carve_commit(&repo, &co))
			fatal("split commit");
		if (!any) {
			struct strbuf ids = STRBUF_INIT;
			bool left = false;

			for (f = 0; f < nfiles; f++) {
				struct fdiff *fd = NULL;

				if ((path_in_worktree(paths[f]) ||
				     path_in_index(&repo, paths[f]) == 1) &&
				    !load_diff(&repo, paths[f], false, &fd) &&
				    fd)
					left = true;
				fdiff_free(fd);
				free(fd);
			}
			if (left)
				continue; /* all skipped this round */
			if (have_staged(&repo) && carve_commit(&repo, &co))
				fatal("split final commit");
			if (split_finish(&repo, &ids)) {
				fprintf(stderr, "%s\n", repo.err.buf);
				fatal("I6: split did not finish");
			}
			t = git_line(&repo, "rev-parse", "HEAD^{tree}", NULL);
			if (!t || strcmp(t, otree))
				fatal("I6: tree differs from the original");
			free(t);
			sb_release(&ids);
			break;
		}
	}
	for (f = 0; f < nfiles; f++)
		sb_release(&content[f]);
	sb_release(&state);
	free(orig);
	free(otree);
	return true;
}

/* Stage random parts of every change and commit, until nothing is left. */
static void carve_until_done(const char *what)
{
	int round;

	for (round = 0; round < 80; round++) {
		struct commit_opts co = { .message = "piece", .quiet = true };
		struct file_status *fs = NULL;
		bool any = false;
		size_t n = 0, i;

		if (load_status(&repo, &fs, &n))
			fatal("load_status");
		for (i = 0; i < n; i++) {
			struct strbuf spec = STRBUF_INIT;
			struct fdiff *fd = NULL;

			if (!fs_unstaged(&fs[i]) || rnd_n(3) == 0)
				continue;
			cur_path = fs[i].path;
			if (load_diff(&repo, fs[i].path, false, &fd))
				fatal("carve load_diff");
			if (fd && fdiff_line_ops(fd, NULL) && rnd_n(4)) {
				random_selection(fd, &spec, false);
				if (stage_lines(&repo, fs[i].path, spec.buf,
						PATCH_STAGE))
					fatal("carve stage_lines");
			} else if (stage_file(&repo, fs[i].path)) {
				fatal("carve stage_file");
			}
			any = true;
			fdiff_free(fd);
			free(fd);
			sb_release(&spec);
		}
		free_status(fs, n);
		if (any && have_staged(&repo) && carve_commit(&repo, &co))
			fatal("carve commit");
		if (!any) {
			bool left = false;

			if (load_status(&repo, &fs, &n))
				fatal("load_status");
			for (i = 0; i < n; i++)
				left |= fs_unstaged(&fs[i]) ||
					fs_staged(&fs[i]);
			free_status(fs, n);
			if (!left)
				return;
		}
	}
	cur_path = what;
	fatal("carving did not converge");
}

/* Build a series of 4 commits of random edits on two files. */
static void random_series(void)
{
	static const char *const files[] = { "q0.txt", "q1.txt" };
	int c, f;

	for (c = 0; c < 4; c++) {
		for (f = 0; f < 2; f++) {
			struct strbuf cur = STRBUF_INIT, next = STRBUF_INIT;
			struct text t;

			if (c && rnd_n(2))
				continue;
			read_file(files[f], &cur);
			if (!c || !cur.len)
				random_text(&t);
			else
				parse_text(cur.buf, cur.len, &t);
			random_edit(&t);
			text_serialize(&t, &next);
			if (write_file(files[f], sb_str(&next), next.len))
				fatal("series write");
			text_free(&t);
			sb_release(&cur);
			sb_release(&next);
		}
		if (git_quiet(&repo, "add", "-A", NULL) ||
		    git_quiet(&repo, "commit", "-q", "--allow-empty", "-m",
			      "series", NULL))
			fatal("series commit");
	}
}

static char *rev(const char *spec)
{
	char *id = git_line(&repo, "rev-parse", "-q", "--verify", spec, NULL);

	if (!id)
		fatal("rev-parse");
	return id;
}

/* Trees of HEAD~(n-1) .. HEAD, oldest first, one per line. */
static void last_trees(int n, struct strbuf *out)
{
	int i;

	for (i = n - 1; i >= 0; i--) {
		char spec[32], *t;

		snprintf(spec, sizeof(spec), "HEAD~%d^{tree}", i);
		t = rev(spec);
		sb_addf(out, "%s\n", t);
		free(t);
	}
}

static bool series_iteration(unsigned long it, uint64_t seed)
{
	struct strbuf before = STRBUF_INIT, after = STRBUF_INIT,
		      ids = STRBUF_INIT;
	char spec[32], *target, *tip;
	int k, kind, rc;

	rng_state = seed;
	cur_iter = it;
	cur_seed = seed;
	unique_mode = rnd_n(2);
	cur_path = "(series)";
	random_series();
	tip = rev("HEAD");
	k = 1 + rnd_n(3); /* the target is HEAD~k */
	snprintf(spec, sizeof(spec), "HEAD~%d", k);
	target = rev(spec);
	kind = rnd_n(3);

	if (kind == 0) {
		/*
		 * split an older commit, and its descendants keep their
		 * trees
		 */
		last_trees(k, &before);
		if (split_start(&repo, target))
			fatal("split older");
		carve_until_done("(split older)");
		if (split_finish(&repo, &ids))
			fatal("I7: split finish of an older commit");
		last_trees(k, &after);
		if (strcmp(before.buf, after.buf))
			fatal("I7: descendants of a split changed trees");
		free(tip);
		tip = rev("HEAD^{tree}");
	} else if (kind == 1) {
		/* defer part of an older commit's diff of q0 or q1 */
		const char *path = rnd_n(2) ? "q0.txt" : "q1.txt";
		struct strbuf sel = STRBUF_INIT;
		struct fdiff *fd = NULL;

		cur_path = path;
		if (load_commit_diff(&repo, target, path, NULL, &fd))
			fatal("load_commit_diff");
		if (!fd || !fdiff_line_ops(fd, NULL)) {
			fdiff_free(fd);
			free(fd);
			goto done;
		}
		random_selection(fd, &sel, false);
		last_trees(k, &before);
		rc = defer_lines(&repo, target, path, sel.buf, NULL, "defer");
		fdiff_free(fd);
		free(fd);
		sb_release(&sel);
		if (rc < 0 && strstr(repo.err.buf, "nothing would be left")) {
			sb_reset(&repo.err);
			goto done;
		}
		if (rc)
			fatal("defer");
		/* the new commit sits at HEAD~k and has the target's tree */
		snprintf(spec, sizeof(spec), "HEAD~%d^{tree}", k);
		{
			struct strbuf s2 = STRBUF_INIT;
			char *nt = rev(spec);
			char *ot;

			sb_addf(&s2, "%s^{tree}", target);
			ot = rev(s2.buf);
			if (strcmp(nt, ot))
				fatal("defer: new commit tree != target tree");
			free(nt);
			free(ot);
			sb_release(&s2);
		}
		last_trees(k, &after);
		if (strcmp(before.buf, after.buf))
			fatal("I7: descendants of a defer changed trees");
	} else {
		/* fixup-to, a random edit in the work tree goes into target */
		const char *path = rnd_n(2) ? "q0.txt" : "q1.txt";
		struct strbuf cur = STRBUF_INIT, next = STRBUF_INIT,
			      sel = STRBUF_INIT, wt = STRBUF_INIT;
		struct fdiff *fd = NULL;
		char *want_tree = NULL;
		struct text t;

		cur_path = path;
		read_file(path, &cur);
		parse_text(sb_str(&cur), cur.len, &t);
		random_edit(&t);
		text_serialize(&t, &next);
		text_free(&t);
		write_file(path, sb_str(&next), next.len);
		if (load_diff(&repo, path, false, &fd))
			fatal("fixup load_diff");
		if (!fd || !fdiff_line_ops(fd, NULL)) {
			fdiff_free(fd);
			free(fd);
			sb_release(&cur);
			sb_release(&next);
			git_status_of(&repo, "checkout", "-q", "--", path,
				      NULL);
			goto done;
		}
		random_selection(fd, &sel, false);
		if (stage_lines(&repo, path, sel.buf, PATCH_STAGE))
			fatal("fixup stage");
		want_tree = git_line(&repo, "write-tree", NULL);
		rc = fixup_to(&repo, target, true);
		if (rc < 0 && !strstr(repo.err.buf, "cannot move the change"))
			fatal("fixup_to");
		if (rc != 0) {
			/*
			 * A conflict and then abort, or a refusal. Either
			 * way everything must be exactly as before.
			 */
			sb_reset(&repo.err);
			if (rc == 1 && series_abort(&repo))
				fatal("fixup abort");
			free(target);
			target = rev("HEAD");
			if (strcmp(target, tip))
				fatal("fixup abort: HEAD not restored");
			if (have_staged(&repo))
				fatal("fixup abort: index not restored");
			read_file(path, &wt);
			if (wt.len != next.len ||
			    memcmp(sb_str(&wt), sb_str(&next), wt.len))
				fatal("fixup abort: work tree not restored");
		} else {
			/*
			 * The tool checks I7 itself. Here, independently,
			 * the new tip must be the old tip plus the
			 * selection, and the work tree exactly what it
			 * was.
			 */
			char *ht = rev("HEAD^{tree}");

			if (!want_tree || strcmp(ht, want_tree))
				fatal("fixup: tip is not old tip + selection");
			if (have_staged(&repo))
				fatal("fixup: index not clean");
			read_file(path, &wt);
			if (wt.len != next.len ||
			    memcmp(sb_str(&wt), sb_str(&next), wt.len))
				fatal("fixup: work tree changed");
			free(ht);
		}
		fdiff_free(fd);
		free(fd);
		sb_release(&cur);
		sb_release(&next);
		sb_release(&sel);
		sb_release(&wt);
		free(want_tree);
		git_status_of(&repo, "reset", "-q", "--hard", NULL);
	}
done:
	free(target);
	free(tip);
	sb_release(&before);
	sb_release(&after);
	sb_release(&ids);
	return true;
}

int main(void)
{
	unsigned long iters = 300, start = 0, it, done = 0;
	uint64_t seed = 1;
	const char *s;

	if ((s = getenv("FUZZ_SEED")))
		seed = strtoull(s, NULL, 10);
	if ((s = getenv("FUZZ_ITERS")))
		iters = strtoul(s, NULL, 10);
	if ((s = getenv("FUZZ_START")))
		start = strtoul(s, NULL, 10);
	if (repo_open(&repo)) {
		fprintf(stderr, "fuzz: %s\n", repo.err.buf);
		return 1;
	}
	git_quiet(&repo, "commit", "-q", "--allow-empty", "-m", "root", NULL);
	for (it = start; it < start + iters; it++)
		if (it % 5 == 4 ? split_iteration(it, seed * 1000003ULL + it) :
		    it % 5 == 2 ? series_iteration(it, seed * 1000003ULL + it) :
				  iteration(it, seed * 1000003ULL + it))
			done++;
	printf("fuzz: seed %llu, %lu iterations, %lu effective\n",
	       (unsigned long long)seed, iters, done);
	repo_release(&repo);
	return 0;
}
