// SPDX-License-Identifier: MIT
/* Conflict regions in work-tree files and taking one side (conflict.h). */
#include "conflict.h"

#include <stdlib.h>
#include <string.h>

/* A marker line is seven c, then the end of the line or a space and a label. */
static bool marker(const char *l, size_t len, char c, char **label)
{
	size_t i;

	if (len && l[len - 1] == '\r')
		len--;
	if (len < 7)
		return false;
	for (i = 0; i < 7; i++)
		if (l[i] != c)
			return false;
	if (len > 7 && l[7] != ' ')
		return false;
	if (label)
		*label = xstrndup(len > 8 ? l + 8 : "", len > 8 ? len - 8 : 0);
	return true;
}

size_t conflicts_parse(const char *buf, size_t len, struct conflict **out)
{
	enum { OUT, OURS, BASE, THEIRS } st = OUT;
	const char *start = buf ? buf : "", *pos = start, *end = pos + len, *l;
	struct conflict *v = NULL, cur;
	size_t n = 0, alloc = 0, ll;
	int line = 0;

	memset(&cur, 0, sizeof(cur));
	while ((l = next_line(&pos, end, &ll))) {
		size_t off = (size_t)(l - start), next = (size_t)(pos - start);
		char *label = NULL;

		line++;
		if (marker(l, ll, '<', &label)) {
			/* a new start drops an unfinished region */
			free(cur.ours_label);
			memset(&cur, 0, sizeof(cur));
			cur.start = off;
			cur.ours = next;
			cur.line = line;
			cur.ours_label = label;
			st = OURS;
		} else if (st == OURS && marker(l, ll, '|', NULL)) {
			cur.ours_end = off;
			cur.base = next;
			st = BASE;
		} else if ((st == OURS || st == BASE) &&
			   marker(l, ll, '=', NULL) &&
			   ll - (l[ll - 1] == '\r') == 7) {
			if (st == OURS) {
				cur.ours_end = off;
				cur.base = cur.base_end = off;
			} else {
				cur.base_end = off;
			}
			cur.theirs = next;
			st = THEIRS;
		} else if (st == THEIRS && marker(l, ll, '>', &label)) {
			cur.theirs_end = off;
			cur.end = next;
			cur.theirs_label = label;
			ALLOC_GROW(v, n + 1, alloc);
			v[n++] = cur;
			memset(&cur, 0, sizeof(cur));
			st = OUT;
		}
	}
	free(cur.ours_label);
	*out = v;
	return n;
}

void conflicts_free(struct conflict *c, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		free(c[i].ours_label);
		free(c[i].theirs_label);
	}
	free(c);
}

/* Stage path once no conflict marker is left in it. */
static int stage_if_resolved(struct repo *r, const char *path)
{
	struct strbuf data = STRBUF_INIT;
	struct conflict *c;
	size_t n;

	if (read_file(path, &data)) {
		sb_release(&data);
		return 0; /* a deleted side is left to the user */
	}
	n = conflicts_parse(sb_str(&data), data.len, &c);
	conflicts_free(c, n);
	sb_release(&data);
	if (n)
		return 0;
	return git_quiet(r, "--literal-pathspecs", "add", "--", path, NULL) ?
		       -1 :
		       0;
}

int resolve_conflict(struct repo *r, const char *path, int n, bool theirs)
{
	struct strbuf data = STRBUF_INIT, out = STRBUF_INIT;
	struct conflict *c = NULL;
	size_t nc = 0;
	int rc = -1;

	if (!n) {
		if (git_quiet(r, "--literal-pathspecs", "checkout",
			      theirs ? "--theirs" : "--ours", "--", path, NULL))
			return -1;
		return stage_if_resolved(r, path);
	}
	if (read_file(path, &data)) {
		repo_err(r, "cannot read %s", path);
		goto out;
	}
	nc = conflicts_parse(sb_str(&data), data.len, &c);
	if (n < 1 || (size_t)n > nc) {
		repo_err(r, "%s has %zu conflict%s, not %d", path, nc,
			 nc == 1 ? "" : "s", n);
		goto out;
	}
	{
		const struct conflict *k = &c[n - 1];
		size_t a;
		size_t b;

		a = theirs ? k->theirs : k->ours;
		b = theirs ? k->theirs_end : k->ours_end;

		sb_add(&out, data.buf, k->start);
		sb_add(&out, data.buf + a, b - a);
		sb_add(&out, data.buf + k->end, data.len - k->end);
	}
	if (write_file(path, sb_str(&out), out.len)) {
		repo_err(r, "cannot write %s", path);
		goto out;
	}
	rc = stage_if_resolved(r, path);
out:
	conflicts_free(c, nc);
	sb_release(&data);
	sb_release(&out);
	return rc;
}
