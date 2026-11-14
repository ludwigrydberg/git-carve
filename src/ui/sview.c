// SPDX-License-Identifier: MIT
/* The Series pane (struct sview). */
#include "sview.h"
#include "ui_int.h"
#include "../rebase.h"

#include <stdlib.h>
#include <string.h>

/*
 * A range's first chunk is at most this many commits, and more load on
 * demand.
 */
#define SV_CHUNK 200

size_t sv_rows(void)
{
	return ui.sv.n + (ui.sv.own ? 1 : 0);
}

const char *sv_commit(size_t row)
{
	size_t k;

	if (ui.sv.own && !row)
		return NULL;
	k = row - (ui.sv.own ? 1 : 0);
	return k < ui.sv.n ? ui.sv.ids[k] : NULL;
}

/* The row of commit id, or (size_t)-1. */
size_t sv_row_of(const char *id)
{
	size_t k;

	for (k = 0; id && k < ui.sv.n; k++)
		if (!strcmp(ui.sv.ids[k], id))
			return k + (ui.sv.own ? 1 : 0);
	return (size_t)-1;
}

/* Room for one more commit in the arrays. */
static void sv_reserve(struct sview *s)
{
	if (s->n + 1 <= s->alloc)
		return;
	s->alloc = s->alloc ? s->alloc * 2 : 64;
	s->ids = xrealloc(s->ids, s->alloc * sizeof(*s->ids));
	s->lines = xrealloc(s->lines, s->alloc * sizeof(*s->lines));
	s->refs = xrealloc(s->refs, s->alloc * sizeof(*s->refs));
	s->merge = xrealloc(s->merge, s->alloc * sizeof(*s->merge));
}

/*
 * The branches of a full %D decoration ("HEAD -> refs/heads/main,
 * refs/remotes/origin/main, refs/heads/dev"), without HEAD, the branch
 * HEAD is on (the title says it) and remote HEADs, space separated, by
 * their full names, or NULL.
 */
static char *branches_of(const char *d, size_t len)
{
	struct strbuf out = STRBUF_INIT;
	const char *end = d + len;

	while (d < end) {
		const char *c = memchr(d, ',', (size_t)(end - d));
		size_t n;

		n = c ? (size_t)(c - d) : (size_t)(end - d);

		if (n >= 8 && !strncmp(d, "HEAD -> ", 8)) {
			d += 8;
			n -= 8;
			/* HEAD's own branch is in the title already */
			d += n;
			n = 0;
		}
		if (n && !(n == 4 && !strncmp(d, "HEAD", 4)) &&
		    !(n > 5 && !strncmp(d + n - 5, "/HEAD", 5)))
			sb_addf(&out, "%s%.*s", out.len ? " " : "", (int)n, d);
		d += n;
		if (d < end && *d == ',')
			d++;
		while (d < end && *d == ' ')
			d++;
	}
	return out.len ? sb_detach(&out) : (sb_release(&out), NULL);
}

/*
 * Load up to count more commits, older than the ones loaded, from the
 * first parent of the oldest one on. --skip would walk all the loaded ones
 * again each time.
 */
static void sv_load(size_t count)
{
	const char *pos, *end, *l, *from;
	struct strbuf out = STRBUF_INIT;
	struct sview *s = &ui.sv;
	size_t len, got = 0;

	if (s->end || !s->tip || !count)
		return;
	from = s->n ? s->next : s->tip;
	if (!from) {
		s->end = true;
		return;
	}
	/* HEAD's own branch is in the title */
	if (repo_first_parents(ui.r, from, count, ui.branch, &out)) {
		msg_repo_err();
		s->end = true;
		goto out;
	}
	pos = sb_str(&out);
	end = pos + out.len;
	while ((l = next_line(&pos, end, &len))) {
		const char *a = memchr(l, 0x1f, len), *b, *d;

		if (!a)
			continue;
		b = memchr(a + 1, 0x1f, len - (size_t)(a + 1 - l));
		if (!b)
			continue;
		d = memchr(b + 1, 0x1f, len - (size_t)(b + 1 - l));
		if (!d)
			continue;
		sv_reserve(s);
		s->ids[s->n] = xstrndup(l, (size_t)(a - l));
		/* more than one parent makes a merge */
		s->merge[s->n] = memchr(a + 1, ' ', (size_t)(b - a - 1)) !=
				 NULL;
		/* the next load starts at the first parent, none for a root */
		free(s->next);
		s->next = b > a + 1 ? xstrndup(a + 1, strcspn(a + 1, " \x1f")) :
				      NULL;
		s->refs[s->n] = branches_of(b + 1, (size_t)(d - b - 1));
		s->lines[s->n] = xstrndup(d + 1, len - (size_t)(d + 1 - l));
		s->n++;
		got++;
	}
	if (got < count)
		s->end = true;
out:
	sb_release(&out);
}

void sv_grow(size_t row)
{
	struct sview *s = &ui.sv;
	size_t want;

	if (s->end || row + 1 < sv_rows())
		return;
	/* the range itself a chunk at a time, past it a step at a time */
	if (s->n < s->inrange) {
		want = s->inrange - s->n;
		if (want > SV_CHUNK)
			want = SV_CHUNK;
	} else {
		/* past the range a step, then twice as many each time */
		if (s->grow < s->step)
			s->grow = s->step;
		want = s->grow;
		if (s->grow < SV_CHUNK)
			s->grow *= 2;
	}
	sv_load(want);
}

/* "refs/remotes/origin/master" -> "origin/master" */
static char *short_ref(const char *full)
{
	static const char *const pre[] = { "refs/heads/", "refs/remotes/",
					   "refs/", NULL };
	size_t i;

	for (i = 0; pre[i]; i++)
		if (!strncmp(full, pre[i], strlen(pre[i])))
			return xstrdup(full + strlen(pre[i]));
	return xstrdup(full);
}

/* Both NULL, or the same string. */
static bool same_str(const char *a, const char *b)
{
	return a == b || (a && b && !strcmp(a, b));
}

/* HEAD's upstream by its full name, "" without one. */
static char *upstream_of_head(struct repo *r)
{
	char *up = repo_upstream(r);

	return up ? up : xstrdup("");
}

int sv_open(struct repo *r, const char *range)
{
	const char *dots = range ? strstr(range, "..") : NULL;
	struct strbuf spec = STRBUF_INIT;
	struct sview *s = &ui.sv;
	long cnt;

	s->follow = true;
	s->step = 5;
	if (repo_config_int(r, "carve.seriesStep", 0) > 0)
		s->step = (size_t)repo_config_int(r, "carve.seriesStep", 0);
	/* your series only, <base>.. or <base>..HEAD */
	if (range && (!dots || dots == range || dots[2] == '.' ||
		      (dots[2] && strcmp(dots + 2, "HEAD"))))
		return repo_err(r,
				"'%s': the series is your branch: "
				"<base>.. or <base>..HEAD (see 'git carve "
				"help')",
				range);
	s->own = true;
	s->range = range ? xstrdup(range) : NULL;
	s->right = xstrdup("HEAD");
	s->tip = repo_rev(r, "HEAD^{commit}");
	if (!s->tip)
		return 0; /* no commit yet */
	if (range) {
		/* the base given */
		char *base = xstrndup(range, (size_t)(dots - range));

		sb_addf(&spec, "%s^{commit}", base);
		s->base = repo_rev(r, spec.buf);
		s->base_name = base;
		if (!s->base)
			return repo_err(r, "unknown revision '%s'", base);
	} else if ((s->base = rebase_onto(r))) {
		/* in your rebase (HEAD detached): what it replays */
		sb_addf(&spec, "rebase onto %.7s", s->base);
		s->base_name = sb_detach(&spec);
	} else {
		/*
		 * HEAD's upstream by its full name, asked once with one
		 * git and again on F5. Its commit comes from the cat-file
		 * worker.
		 */
		if (!ui.upstream || !same_str(ui.upstream_for, ui.branch)) {
			free(ui.upstream);
			free(ui.upstream_for);
			ui.upstream = upstream_of_head(r);
			ui.upstream_for = ui.branch ? xstrdup(ui.branch) : NULL;
		}
		if (*ui.upstream) {
			s->base = repo_rev(r, ui.upstream);
			if (s->base)
				s->base_name = short_ref(ui.upstream);
		}
	}
	sb_reset(&r->err);
	s->inrange = 1;
	if (s->base) {
		cnt = series_count(r, s->tip, s->base);
		s->inrange = cnt >= 0 ? (size_t)cnt : 1;
		s->bounded = true;
	} else if (!range && repo_has_remotes(r)) {
		/* without an upstream, yours are what no remote branch has */
		cnt = series_count(r, s->tip, NULL);
		if (cnt >= 0) {
			s->inrange = (size_t)cnt;
			s->bounded = true;
			free(s->base_name);
			s->base_name = xstrdup("pushed");
		}
	}
	/* the range, at least its tip */
	sv_load(s->inrange > SV_CHUNK ? SV_CHUNK : s->inrange ? s->inrange : 1);
	sb_release(&spec);
	return 0;
}

void sv_release(void)
{
	struct sview *s = &ui.sv;
	size_t i;

	for (i = 0; i < s->n; i++) {
		free(s->ids[i]);
		free(s->lines[i]);
		free(s->refs[i]);
	}
	free(s->ids);
	free(s->lines);
	free(s->refs);
	free(s->merge);
	free(s->range);
	free(s->tip);
	free(s->right);
	free(s->base);
	free(s->base_name);
	free(s->next);
	memset(s, 0, sizeof(*s));
}

/*
 * The label of commit k, its distance from the tip as HEAD, ~1, ~2 and so
 * on.
 */
void sv_label(size_t k, char *buf, size_t len)
{
	if (!k)
		snprintf(buf, len, "%s", ui.sv.own ? "HEAD" : "tip");
	else
		snprintf(buf, len, "~%zu", k);
}

bool head_shown(void)
{
	return ui.sv.own && ui.sv.n && ui.ccommit &&
	       !strcmp(ui.ccommit, ui.sv.ids[0]);
}

/*
 * The tip moved by one commit, on top of the old one (a commit) or in its
 * place (an amend), and the base is as it was. That commit comes from the
 * cat-file worker instead of git rev-list and git log for the series.
 */
static bool sv_step(const char *tip)
{
	char type[16], *parent = NULL, *sub = NULL, *base = NULL;
	struct strbuf obj = STRBUF_INIT, line = STRBUF_INIT;
	struct sview *s = &ui.sv;
	int nparents = 0, abbrev;
	bool ok = false, on_top;
	const char *p, *q;

	/* the upstream asked for the branch HEAD is on now */
	if (!s->own || !s->n || !s->tip || !ui.upstream ||
	    !same_str(ui.upstream_for, ui.branch) ||
	    git_path_exists(ui.r, "rebase-merge") ||
	    git_path_exists(ui.r, "rebase-apply"))
		return false;
	/* the base as it was, the range's or the upstream's commit */
	if (s->range) {
		struct strbuf spec = STRBUF_INIT;

		sb_addf(&spec, "%s^{commit}", s->base_name);
		base = repo_rev(ui.r, spec.buf);
		sb_release(&spec);
	} else if (ui.upstream && *ui.upstream) {
		base = repo_rev(ui.r, ui.upstream);
	}
	sb_reset(&ui.r->err);
	if (!same_str(base, s->base) || repo_cat(ui.r, tip, &obj, type) ||
	    strcmp(type, "commit"))
		goto out;
	/* "tree <id>", then a "parent <id>" line per parent */
	for (p = sb_str(&obj);
	     !strncmp(p, "tree ", 5) || !strncmp(p, "parent ", 7); p = q + 1) {
		q = strchr(p, '\n');
		if (!q)
			goto out;
		if (!strncmp(p, "parent ", 7)) {
			nparents++;
			free(parent);
			parent = xstrndup(p + 7, (size_t)(q - p - 7));
		}
	}
	if (nparents != 1)
		goto out;
	on_top = !strcmp(parent, s->tip);
	/* the list is the first-parent chain, and ids[1] is the old tip's */
	if (!on_top && !(s->n >= 2 && !strcmp(parent, s->ids[1])))
		goto out;
	sub = repo_commit_subject(ui.r, tip);
	if (!sub)
		goto out;
	abbrev = (int)strcspn(s->lines[0], " ");
	sb_addf(&line, "%.*s %s", abbrev, tip, sub);
	if (on_top) {
		sv_reserve(s);
		memmove(s->ids + 1, s->ids, s->n * sizeof(*s->ids));
		memmove(s->lines + 1, s->lines, s->n * sizeof(*s->lines));
		memmove(s->merge + 1, s->merge, s->n * sizeof(*s->merge));
		memmove(s->refs + 1, s->refs, s->n * sizeof(*s->refs));
		s->refs[0] = NULL;
		s->n++;
		if (s->bounded)
			s->inrange++;
	} else {
		/*
		 * after an amend the branches at the old commit are not at
		 * this one
		 */
		free(s->ids[0]);
		free(s->lines[0]);
		free(s->refs[0]);
		s->refs[0] = NULL;
	}
	s->ids[0] = xstrdup(tip);
	s->lines[0] = sb_detach(&line);
	s->merge[0] = false;
	free(s->tip);
	s->tip = xstrdup(tip);
	ok = true;
out:
	free(parent);
	free(sub);
	free(base);
	sb_release(&obj);
	sb_release(&line);
	return ok;
}

void sv_refresh(bool full)
{
	struct strbuf spec = STRBUF_INIT;
	struct sview *s = &ui.sv;
	size_t sel = s->sel;
	char *tip, *range;
	bool follow;

	follow = s->follow;

	if (!s->right)
		return;
	if (s->fresh) {
		s->fresh = false;
		return;
	}
	if (full) {
		/* the upstream may have been set or changed outside */
		char *up = upstream_of_head(ui.r);

		if (ui.upstream && !strcmp(up, ui.upstream) &&
		    same_str(ui.upstream_for, ui.branch)) {
			free(up);
			full = false; /* as it was, the pane stays */
		} else {
			free(ui.upstream);
			free(ui.upstream_for);
			ui.upstream = up;
			ui.upstream_for = ui.branch ? xstrdup(ui.branch) : NULL;
		}
	}
	sb_addf(&spec, "%s^{commit}", s->right);
	tip = repo_rev(ui.r, spec.buf);
	sb_release(&spec);
	if (!full && tip && s->tip && !strcmp(tip, s->tip)) {
		free(tip);
		return;
	}
	if (!full && tip && sv_step(tip)) {
		free(tip);
		return;
	}
	free(tip);
	/* the tip moved (a commit, an amend, a split): the series again */
	range = s->range ? xstrdup(s->range) : NULL;
	sv_release();
	if (sv_open(ui.r, range))
		msg_repo_err();
	s->sel = sel < sv_rows() ? sel : sv_rows() ? sv_rows() - 1 : 0;
	s->follow = follow;
	free(range);
}

void sv_follow(bool dirty)
{
	struct sview *s = &ui.sv;

	/*
	 * until a row is chosen, the work tree while it has changes, else
	 * HEAD
	 */
	if (s->own && s->follow)
		s->sel = dirty || sv_rows() < 2 ? 0 : 1;
}

/* Load the rest of the range (up to a chunk): a list of all of it. */
void sv_load_range(void)
{
	struct sview *s = &ui.sv;

	if (s->n < s->inrange)
		sv_load(s->inrange - s->n > SV_CHUNK ? SV_CHUNK :
						       s->inrange - s->n);
}
