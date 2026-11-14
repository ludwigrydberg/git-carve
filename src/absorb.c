// SPDX-License-Identifier: MIT
/*
 * absorb, moving unstaged change blocks into the commits that own their
 * lines.
 */
#include "absorb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diff.h"
#include "patch.h"
#include "series.h"
#include "split.h"
#include "opstate.h"
#include "status.h"
#include "rebase.h"
#include "cdiff.h"

void absorb_plan_free(struct absorb_hunk *v, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		free(v[i].path);
		free(v[i].spec);
		free(v[i].target);
		free(v[i].why);
	}
	free(v);
}

/*
 * The commits of range, or of the series (series_revs) for NULL, newest
 * first, one full id per line.
 */
static int range_commits(struct repo *r, const char *range, struct strbuf *out)
{
	struct strbuf key = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	char *head, *name = NULL;
	int rc = 0;
	size_t i;

	argv_pushl(&a, "rev-list", "--no-merges", NULL);
	if (range)
		argv_push(&a, range);
	else
		series_revs(r, &a, &name);
	argv_push(&a, "--");
	/* the plan's list again for its apply, while HEAD is the same */
	head = repo_rev(r, "HEAD");
	for (i = 0; i < a.n; i++)
		sb_addf(&key, "%s ", a.v[i]);
	sb_adds(&key, head ? head : "-");
	if (r->ab_key && !strcmp(r->ab_key, key.buf)) {
		sb_add(out, sb_str(&r->ab_list), r->ab_list.len);
		goto out;
	}
	if (run_git(&a, NULL, out, NULL)) {
		rc = repo_err(r, "cannot list the commits of %s",
			      range ? range : "the series");
		goto out;
	}
	if (!out->len) {
		rc = range ? repo_err(r, "no commits in %s", range) :
			     repo_err(r, "no commits above %s",
				      name && strcmp(name, "pushed") ?
					      name :
					      "the remote branches");
		goto out;
	}
	free(r->ab_key);
	r->ab_key = sb_detach(&key);
	sb_reset(&r->ab_list);
	sb_add(&r->ab_list, out->buf, out->len);
out:
	free(head);
	free(name);
	argv_clear(&a);
	sb_release(&key);
	return rc;
}

static bool in_commits(const struct strbuf *commits, const char *id)
{
	const char *p = sb_str(commits);
	size_t len = strlen(id);

	for (; (p = strstr(p, id)); p += len)
		if ((p == commits->buf || p[-1] == '\n') &&
		    (p[len] == '\n' || !p[len]))
			return true;
	return false;
}

/* The lines of HEAD's file a hunk's target is blamed from. */
struct blame_range {
	int a, b;	   /* 0 when there is nothing to blame */
	struct strbuf ids; /* the distinct commits, one per line */
	int n;
};

/* Add id to r's commits unless it is there already. */
static void add_commit(struct blame_range *br, const char *id)
{
	const char *p = sb_str(&br->ids);

	for (; (p = strstr(p, id)); p++)
		if ((p == br->ids.buf || p[-1] == '\n') && p[40] == '\n')
			return;
	sb_addf(&br->ids, "%s\n", id);
	br->n++;
}

/* One more line of commit id. */
static void count_commit(struct blame_count **v, size_t *n, size_t *alloc,
			 const char *id)
{
	size_t i;

	for (i = 0; i < *n; i++)
		if (!strcmp((*v)[i].id, id)) {
			(*v)[i].n++;
			return;
		}
	ALLOC_GROW(*v, *n + 1, *alloc);
	(*v)[*n].id = xstrdup(id);
	(*v)[(*n)++].n = 1;
}

/*
 * One git blame of path in HEAD for all ranges (-L each), every line's
 * commit counted in the ranges it lies in. -M follows lines moved in
 * the file; -C, lines from other files, with carve.absorbCopies (several
 * times slower on a big tree).
 */
static int blame_file(struct repo *r, const char *path, struct blame_range *v,
		      size_t n, struct blame_count **cnt, size_t *ncnt)
{
	struct strbuf out = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	const char *pos, *end, *l;
	size_t len, i, any = 0;
	size_t calloc_ = 0;
	int rc;

	argv_pushl(&a, "blame", "-M", "--porcelain", NULL);
	if (repo_config_bool(r, "carve.absorbCopies", false))
		argv_push(&a, "-C");
	for (i = 0; i < n; i++) {
		char lr[64];

		if (!v[i].a)
			continue;
		snprintf(lr, sizeof(lr), "%d,%d", v[i].a, v[i].b);
		argv_pushl(&a, "-L", lr, NULL);
		any++;
	}
	if (!any) {
		argv_clear(&a);
		return 0;
	}
	argv_pushl(&a, "HEAD", "--", path, NULL);
	rc = run_git(&a, NULL, &out, NULL);
	argv_clear(&a);
	pos = sb_str(&out);
	end = pos + out.len;
	while (!rc && (l = next_line(&pos, end, &len))) {
		const char *num;
		int final = 0;
		char id[41];
		long orig;
		size_t k;

		/* "<40 hex> <orig> <final> [<count>]": a line's header */
		if (len < 41 || l[40] != ' ')
			continue;
		for (k = 0; k < 40; k++)
			if (!strchr("0123456789abcdef", l[k]))
				break;
		/* the orig number, then the final one */
		num = l + 41;
		if (k < 40 || parse_long(num, &num, &orig) || num[0] != ' ' ||
		    parse_int(num + 1, NULL, &final))
			continue;
		memcpy(id, l, 40);
		id[40] = '\0';
		for (i = 0; i < n; i++)
			if (v[i].a && final >= v[i].a && final <= v[i].b)
				add_commit(&v[i], id);
		if (cnt && ncnt)
			count_commit(cnt, ncnt, &calloc_, id);
	}
	sb_release(&out);
	return rc ? -1 : 0;
}

/* Number of lines of path in HEAD (through the cat-file worker). */
static int head_lines(struct repo *r, const char *path)
{
	struct strbuf spec = STRBUF_INIT, out = STRBUF_INIT;
	int n = 0;

	sb_addf(&spec, "HEAD:%s", path);
	if (!repo_cat(r, spec.buf, &out, NULL)) {
		n = (int)count_lines(out.buf, out.len);
		if (out.len && out.buf[out.len - 1] != '\n')
			n++;
	}
	sb_release(&spec);
	sb_release(&out);
	return n;
}

/* The lines of HEAD a -U0 hunk is blamed from, or why there are none. */
static void hunk_range(const struct hunk *h, int nlines, struct blame_range *br,
		       struct strbuf *why)
{
	if (h->old_count) {
		/* the removed lines themselves */
		br->a = h->old_start;
		br->b = h->old_start + h->old_count - 1;
		return;
	}
	/* for an insertion, the lines around it */
	br->a = h->old_start > 0 ? h->old_start : 1;
	br->b = h->old_start + 1 <= nlines ? h->old_start + 1 : br->a;
	if (br->a > nlines) {
		br->a = br->b = 0;
		sb_adds(why, "no lines around it to blame");
	}
}

/* The target from the commits blamed for a hunk, or why it has none. */
static void hunk_target(const struct blame_range *br,
			const struct strbuf *commits, struct absorb_hunk *ah,
			struct strbuf *why)
{
	char id[41];

	if (!br->a)
		return;
	if (br->n != 1) {
		sb_addf(why, "its lines come from %d commits", br->n);
		return;
	}

	snprintf(id, sizeof(id), "%.40s", sb_str(&br->ids));
	if (!in_commits(commits, id)) {
		sb_addf(why, "its lines come from %.7s, outside the series",
			id);
		return;
	}
	ah->target = xstrdup(id);
}

/* The selection of a -U0 hunk, such as "o5-7,n5-8". */
static char *hunk_spec(const struct hunk *hk)
{
	struct strbuf spec = STRBUF_INIT;

	if (hk->old_count)
		sb_addf(&spec, "o%d-%d", hk->old_start,
			hk->old_start + hk->old_count - 1);
	if (hk->new_count)
		sb_addf(&spec, "%sn%d-%d", spec.len ? "," : "", hk->new_start,
			hk->new_start + hk->new_count - 1);
	return sb_detach(&spec);
}

/* Plan the hunks of one file, with one blame for all of them. */
static void plan_file(struct repo *r, const struct fdiff *fd, const char *path,
		      const struct strbuf *commits, struct absorb_hunk **v,
		      size_t *n, size_t *alloc)
{
	struct blame_range *br = xcalloc(fd->nhunks + 1, sizeof(*br));
	struct strbuf *why = xcalloc(fd->nhunks + 1, sizeof(*why));
	int nlines = head_lines(r, path);
	bool failed;
	size_t h;

	for (h = 0; h < fd->nhunks; h++)
		hunk_range(&fd->hunks[h], nlines, &br[h], &why[h]);
	failed = blame_file(r, path, br, fd->nhunks, NULL, NULL) != 0;
	for (h = 0; h < fd->nhunks; h++) {
		const struct hunk *hk = &fd->hunks[h];
		struct absorb_hunk *ah;

		ALLOC_GROW(*v, *n + 1, *alloc);
		ah = &(*v)[(*n)++];
		memset(ah, 0, sizeof(*ah));
		ah->path = xstrdup(path);
		ah->line = hk->new_count ? hk->new_start : hk->old_start;
		ah->new_start = hk->new_start;
		ah->new_count = hk->new_count;
		ah->spec = hunk_spec(hk);
		if (failed && br[h].a)
			sb_adds(&why[h], "git blame failed");
		else
			hunk_target(&br[h], commits, ah, &why[h]);
		if (why[h].len)
			ah->why = sb_detach(&why[h]);
		sb_release(&br[h].ids);
		sb_release(&why[h]);
	}
	free(br);
	free(why);
}

int absorb_plan(struct repo *r, const char *range, struct absorb_hunk **out,
		size_t *nout)
{
	struct diff_opts zero = { .context = 0 };
	struct strbuf commits = STRBUF_INIT;
	size_t n = 0, alloc = 0, nf = 0, f;
	struct absorb_hunk *v = NULL;
	struct fdiff *fds = NULL;
	int rc = -1;

	*out = NULL;
	*nout = 0;
	if (have_staged(r)) {
		repo_err(r, "the index has staged changes: absorb works on "
			    "unstaged ones; commit or unstage first");
		goto out;
	}
	if (range_commits(r, range, &commits) ||
	    load_diff_all(r, false, &zero, &fds, &nf))
		goto out;
	for (f = 0; f < nf; f++) {
		const struct fdiff *fd = &fds[f];
		const char *path;

		path = fd->new_path ? fd->new_path : fd->old_path;

		if (!path)
			continue;
		if (!fdiff_line_ops(fd, NULL) || fd->is_new) {
			ALLOC_GROW(v, n + 1, alloc);
			memset(&v[n], 0, sizeof(v[n]));
			v[n].path = xstrdup(path);
			v[n].why = xstrdup("a whole-file change");
			n++;
			continue;
		}
		plan_file(r, fd, path, &commits, &v, &n, &alloc);
	}
	*out = v;
	*nout = n;
	v = NULL;
	n = 0;
	rc = 0;
out:
	absorb_plan_free(v, n);
	fdiff_free_array(fds, nf);
	sb_release(&commits);
	return rc;
}

void absorb_describe(struct repo *r, const struct absorb_hunk *v, size_t n,
		     struct strbuf *out)
{
	struct strbuf names = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	size_t i;

	/* "<full> <short> <subject>" of every target, in one git log */
	argv_pushl(&a, "log", "--no-walk=unsorted", "--no-color",
		   "--format=%H %h %s", NULL);
	for (i = 0; i < n; i++)
		if (v[i].target)
			argv_push(&a, v[i].target);
	if (a.n > 4)
		run_git(&a, NULL, &names, NULL);
	argv_clear(&a);
	for (i = 0; i < n; i++) {
		if (v[i].target) {
			const char *p = strstr(sb_str(&names), v[i].target);
			const char *s;

			s = p ? p + strlen(v[i].target) + 1 : NULL;

			if (s)
				sb_addf(out, "%s:%d -> %.*s\n", v[i].path,
					v[i].line, (int)strcspn(s, "\n"), s);
			else
				sb_addf(out, "%s:%d -> %s\n", v[i].path,
					v[i].line, v[i].target);
		} else {
			sb_addf(out, "%s%s%.0d stays: %s\n", v[i].path,
				v[i].line ? ":" : "", v[i].line, v[i].why);
		}
	}
	sb_release(&names);
}

/*
 * The selection of target's hunks of path in the unstaged -U0 diff now.
 * Planned hunks are found again by their work-tree lines, which moving
 * other hunks into commits does not change.
 */
static void target_spec(const struct absorb_hunk *v, size_t n,
			const char *target, const struct fdiff *fd,
			struct strbuf *spec, int *count)
{
	const char *path = fd->new_path ? fd->new_path : fd->old_path;
	size_t i, h;

	for (i = 0; path && i < n; i++) {
		if (!v[i].target || strcmp(v[i].target, target) ||
		    strcmp(v[i].path, path))
			continue;
		for (h = 0; h < fd->nhunks; h++) {
			const struct hunk *hk = &fd->hunks[h];
			char *s;

			if (hk->new_start != v[i].new_start ||
			    hk->new_count != v[i].new_count)
				continue;
			s = hunk_spec(hk);
			sb_addf(spec, "%s%s", spec->len ? "," : "", s);
			free(s);
			(*count)++;
			break;
		}
	}
}

/*
 * Stage the hunks of the targets given (ended by NULL) as fds has them,
 * with one patch for all files and one git apply. fds are diffs of the
 * work tree as it is, and no file is diffed again.
 */
static int stage_targets(struct repo *r, const struct absorb_hunk *v, size_t n,
			 const char *const *targets, const struct fdiff *fds,
			 size_t nf, int *count)
{
	struct strbuf patch = STRBUF_INIT, err = STRBUF_INIT;
	size_t f, t;
	int rc = 0;

	for (f = 0; !rc && f < nf; f++) {
		struct strbuf spec = STRBUF_INIT;
		struct fdiff fd;

		for (t = 0; targets[t]; t++)
			target_spec(v, n, targets[t], &fds[f], &spec, count);
		if (spec.len) {
			fdiff_dup(&fds[f], &fd);
			if (sel_apply(&fd, spec.buf, &err) < 0 ||
			    build_patch(&fd, PATCH_STAGE, &patch, &err) < 0)
				rc = repo_err(r, "%s", err.buf);
			fdiff_free(&fd);
		}
		sb_release(&spec);
	}
	/* fds are -U0 diffs (load_diff_all with zero context) */
	if (!rc && patch.len)
		rc = apply_patch_zero(r, &patch, false, true);
	sb_release(&patch);
	sb_release(&err);
	return rc ? -1 : 0;
}

/*
 * All targets in one rebase. Each target's tree is made with its blocks,
 * staged alone and then unstaged again, and then everything is staged for
 * fixup_trees. Returns -2 when a block does not apply to its target
 * directly, and the caller then goes one target at a time. Else returns as
 * fixup_trees.
 */
static int apply_at_once(struct repo *r, const struct absorb_hunk *v, size_t n,
			 char **targets, size_t nt, int *moved)
{
	char **trees = xcalloc(nt + 1, sizeof(*trees));
	struct diff_opts zero = { .context = 0 };
	struct fdiff *fds = NULL;
	int rc = -1, count = 0;
	const char **all;
	size_t nf = 0, t;

	all = xcalloc(nt + 1, sizeof(*all));

	if (load_diff_all(r, false, &zero, &fds, &nf))
		goto out;
	for (t = 0; t < nt; t++) {
		const char *one[2] = { targets[t], NULL };
		int c = 0;

		all[t] = targets[t];
		if (stage_targets(r, v, n, one, fds, nf, &c))
			goto unstage;
		trees[t] = change_tree_onto(r, targets[t]);
		if (git_quiet(r, "reset", "-q", NULL))
			goto out;
		if (!trees[t]) {
			rc = -2;
			goto out;
		}
	}
	if (stage_targets(r, v, n, all, fds, nf, &count))
		goto unstage;
	rc = fixup_trees(r, targets, trees, nt, true);
	if (rc >= 0)
		*moved += count;
	goto out;
unstage:
	git_status_of(r, "reset", "-q", NULL);
out:
	for (t = 0; t < nt; t++)
		free(trees[t]);
	free(trees);
	free(all);
	fdiff_free_array(fds, nf);
	return rc;
}

int absorb_apply_plan(struct repo *r, const char *range,
		      const struct absorb_hunk *plan, size_t n, int *moved)
{
	struct absorb_hunk *v = xcalloc(n + 1, sizeof(*v));
	struct diff_opts zero = { .context = 0 };
	struct strbuf commits = STRBUF_INIT;
	const char *pos, *end, *l;
	char **targets = NULL;
	size_t i, j, nt = 0;
	size_t len;
	int rc = 0;

	*moved = 0;
	/* our copy, where the targets are crossed out as they are done */
	for (i = 0; i < n; i++) {
		v[i] = plan[i];
		v[i].path = xstrdup(plan[i].path);
		v[i].spec = plan[i].spec ? xstrdup(plan[i].spec) : NULL;
		v[i].target = plan[i].target ? xstrdup(plan[i].target) : NULL;
		v[i].why = plan[i].why ? xstrdup(plan[i].why) : NULL;
	}
	/*
	 * The targets oldest first (the commits come newest first), all
	 * moved in one rebase when every block applies to its target as it
	 * is there
	 */
	if (range_commits(r, range, &commits))
		rc = -1;
	targets = xcalloc(n + 1, sizeof(*targets));
	pos = sb_str(&commits);
	end = pos + commits.len;
	while (!rc && (l = next_line(&pos, end, &len))) {
		for (i = 0; i < n; i++)
			if (v[i].target && strlen(v[i].target) == len &&
			    !strncmp(v[i].target, l, len))
				break;
		if (i < n) {
			memmove(targets + 1, targets, nt * sizeof(*targets));
			targets[0] = v[i].target;
			nt++;
		}
	}
	if (!rc && nt) {
		rc = apply_at_once(r, v, n, targets, nt, moved);
		if (rc != -2)
			nt = 0; /* done, or failed */
		else
			rc = 0;
	}
	/*
	 * one target at a time, newest first, since a rewrite leaves the
	 * older ones as they are
	 */
	for (j = nt; !rc && j > 0; j--) {
		const char *one[2] = { targets[j - 1], NULL };
		struct fdiff *fds = NULL;
		size_t nf = 0;
		int count = 0;

		if (load_diff_all(r, false, &zero, &fds, &nf)) {
			rc = -1;
			break;
		}
		rc = stage_targets(r, v, n, one, fds, nf, &count);
		fdiff_free_array(fds, nf);
		if (rc) {
			git_status_of(r, "reset", "-q", NULL);
			break;
		}
		if (count) {
			rc = fixup_to(r, one[0], true);
			if (rc >= 0)
				*moved += count;
		}
	}
	free(targets);
	absorb_plan_free(v, n);
	sb_release(&commits);
	return rc;
}

void blame_counts_free(struct blame_count *v, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		free(v[i].id);
	free(v);
}

static int by_lines(const void *a, const void *b)
{
	const struct blame_count *x = a, *y = b;

	if (x->n != y->n)
		return (x->n < y->n) - (x->n > y->n);
	return strcmp(x->id, y->id);
}

int blame_change(struct repo *r, const struct fdiff *fd,
		 struct blame_count **out, size_t *nout, int *nlines)
{
	bool *rm = NULL, *near = NULL, *use, any_sel = false;
	struct blame_range *br = NULL;
	size_t nbr = 0, abr = 0, h, j;
	int total, l, rc = -1;

	*out = NULL;
	*nout = 0;
	*nlines = 0;
	if (!fd->old_path || fd->is_new)
		return repo_err(r, "a new file: no lines of it in HEAD");
	if (fd->binary || fd->combined || !fd->nhunks)
		return repo_err(r, "no changed lines to blame");
	/* the last old line the diff names, without reading the file */
	for (h = 0, total = 0; h < fd->nhunks; h++)
		if (fd->hunks[h].old_start + fd->hunks[h].old_count > total)
			total = fd->hunks[h].old_start + fd->hunks[h].old_count;
	rm = xcalloc((size_t)total + 2, sizeof(*rm));
	near = xcalloc((size_t)total + 2, sizeof(*near));
	for (h = 0; h < fd->nhunks; h++)
		for (j = 0; j < fd->hunks[h].nlines; j++)
			if (fd->hunks[h].lines[j].sel &&
			    fd->hunks[h].lines[j].op != ' ')
				any_sel = true;
	for (h = 0; h < fd->nhunks; h++) {
		const struct hunk *hk = &fd->hunks[h];
		size_t k, e;
		/* the old line a block of changes comes after */
		int last;

		last = hk->old_count ? hk->old_start - 1 : hk->old_start;

		for (j = 0; j < hk->nlines; j = e) {
			bool has_rm = false, take_add = false;

			if (hk->lines[j].op == ' ') {
				last = hk->lines[j].old_no;
				e = j + 1;
				continue;
			}
			/* a block of changed lines, j..e */
			for (e = j; e < hk->nlines && hk->lines[e].op != ' ';
			     e++)
				if (hk->lines[e].op == '-')
					has_rm = true;
			for (k = j; k < e; k++) {
				const struct dline *d = &hk->lines[k];

				if (!d->sel && any_sel)
					continue;
				if (d->op == '-' && d->old_no <= total)
					rm[d->old_no] = true;
				else if (d->op == '+')
					take_add = true;
			}
			if (take_add && has_rm) {
				/* added lines replace the block's removed ones */
				for (k = j; k < e; k++)
					if (hk->lines[k].op == '-' &&
					    hk->lines[k].old_no <= total)
						rm[hk->lines[k].old_no] = true;
			} else if (take_add) {
				/* for an insertion, the lines around it */
				if (last >= 1 && last <= total)
					near[last] = true;
				/*
				 * the line after it, if the diff shows one (at
				 * the end of a hunk it may be the end of the
				 * file)
				 */
				if (e < hk->nlines && last + 1 <= total)
					near[last + 1] = true;
			}
			for (k = j; k < e; k++)
				if (hk->lines[k].op == '-')
					last = hk->lines[k].old_no;
		}
	}
	use = rm;
	for (l = 1; l <= total && !rm[l]; l++)
		;
	if (l > total)
		use = near;
	/* the runs of lines, an -L each */
	for (l = 1; l <= total; l++) {
		if (!use[l])
			continue;
		ALLOC_GROW(br, nbr + 1, abr);
		memset(&br[nbr], 0, sizeof(br[nbr]));
		br[nbr].a = l;
		while (l + 1 <= total && use[l + 1])
			l++;
		br[nbr].b = l;
		*nlines += br[nbr].b - br[nbr].a + 1;
		nbr++;
	}
	if (!nbr) {
		repo_err(r, "no lines around the change to blame");
		goto out;
	}
	if (blame_file(r, fd->old_path, br, nbr, out, nout)) {
		blame_counts_free(*out, *nout);
		*out = NULL;
		*nout = 0;
		goto out;
	}
	if (*nout)
		qsort(*out, *nout, sizeof(**out), by_lines);
	rc = 0;
out:
	for (h = 0; h < nbr; h++)
		sb_release(&br[h].ids);
	free(br);
	free(rm);
	free(near);
	return rc;
}
