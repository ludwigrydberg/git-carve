// SPDX-License-Identifier: MIT
#include "status.h"
#include "unit.h"

static void test_parse(void)
{
	static const char in[] = "M  b.c\0 M a.c\0?? new file.c\0"
				 "UU conflict.c\0AM added.c\0 D gone.c\0"
				 "R  to.c\0from.c\0";
	struct file_status *fs = NULL;
	size_t n;

	n = parse_status(in, sizeof(in) - 1, &fs);

	CHECK_INT(n, 7);
	if (n != 7)
		return;
	/* sorted by path */
	CHECK_STR(fs[0].path, "a.c");
	CHECK(fs_unstaged(&fs[0]) && !fs_staged(&fs[0]));
	CHECK_STR(fs[1].path, "added.c");
	CHECK(fs_unstaged(&fs[1]) && fs_staged(&fs[1]));
	CHECK_STR(fs[2].path, "b.c");
	CHECK(!fs_unstaged(&fs[2]) && fs_staged(&fs[2]));
	CHECK_STR(fs[3].path, "conflict.c");
	CHECK(fs[3].unmerged && fs_unstaged(&fs[3]) && !fs_staged(&fs[3]));
	CHECK_STR(fs[4].path, "gone.c");
	CHECK_INT(fs[4].y, 'D');
	CHECK_STR(fs[5].path, "new file.c");
	CHECK_INT(fs[5].y, '?');
	CHECK(fs_unstaged(&fs[5]) && !fs_staged(&fs[5]));
	CHECK_STR(fs[6].path, "to.c");
	free_status(fs, n);
}

static void test_merge(void)
{
	static const char sc[] = " M b.c\0?? new.c\0?? a.txt\0?? z/y.c\0";
	struct file_status *t = NULL, *s = NULL, *m = NULL;
	size_t ns = parse_status(sc, sizeof(sc) - 1, &s);
	static const char tr[] = " M b.c\0 A new.c\0";
	size_t nt;
	size_t n;

	nt = parse_status(tr, sizeof(tr) - 1, &t);
	n = merge_untracked(t, nt, s, ns, &m);

	/* new.c was made intent-to-add after the scan, and tracked wins */
	CHECK_INT(n, 4);
	if (n == 4) {
		CHECK_STR(m[0].path, "a.txt");
		CHECK_INT(m[0].y, '?');
		CHECK_STR(m[1].path, "b.c");
		CHECK_INT(m[1].y, 'M');
		CHECK_STR(m[2].path, "new.c");
		CHECK_INT(m[2].y, 'A');
		CHECK_STR(m[3].path, "z/y.c");
	}
	free_status(t, nt);
	free_status(s, ns);
	free_status(m, n);
}

static void test_splice(void)
{
	struct file_status *t = NULL, *a = NULL, *old = NULL;
	static const char tr[] = " M a.c\0 M b.c\0 M d.c\0";
	size_t nt = parse_status(tr, sizeof(tr) - 1, &t);
	static const char nw[] = "M  b.c\0A  c.c\0";
	const char *paths[] = { "b.c", "c.c" };
	size_t nold = 0, n;
	size_t na;

	na = parse_status(nw, sizeof(nw) - 1, &a);

	n = status_splice(&t, nt, paths, 2, a, na, &old, &nold);
	CHECK_INT(n, 4);
	if (n == 4) {
		CHECK_STR(t[0].path, "a.c");
		CHECK_STR(t[1].path, "b.c");
		CHECK_INT(t[1].x, 'M');
		CHECK_INT(t[1].y, ' ');
		CHECK_STR(t[2].path, "c.c");
		CHECK_STR(t[3].path, "d.c");
	}
	/* the old entry of b.c, for the caller */
	CHECK_INT(nold, 1);
	if (nold == 1)
		CHECK_INT(old[0].y, 'M');
	free_status(old, nold);
	/* a path with no entry now leaves the list */
	n = status_splice(&t, n, paths, 1, NULL, 0, &old, &nold);
	CHECK_INT(n, 3);
	if (n == 3)
		CHECK_STR(t[1].path, "c.c");
	free_status(old, nold);
	free_status(t, n);
}

/* gi's line in info/exclude, exactly that path from the top */
static void test_exclude_pattern(void)
{
	struct strbuf sb = STRBUF_INIT;

	exclude_pattern("tags", &sb);
	CHECK_STR(sb.buf, "/tags");
	sb_reset(&sb);
	exclude_pattern("dir/a*b?[c].txt", &sb);
	CHECK_STR(sb.buf, "/dir/a\\*b\\?\\[c].txt");
	sb_reset(&sb);
	exclude_pattern("#x", &sb);
	CHECK_STR(sb.buf, "/\\#x");
	sb_reset(&sb);
	exclude_pattern("!y", &sb);
	CHECK_STR(sb.buf, "/\\!y");
	sb_release(&sb);
}

int main(void)
{
	RUN(test_parse);
	RUN(test_exclude_pattern);
	RUN(test_merge);
	RUN(test_splice);
	return unit_done("status");
}
