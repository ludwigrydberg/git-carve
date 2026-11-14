// SPDX-License-Identifier: MIT
#include <dirent.h>
#include <stdlib.h>

#include "diff.h"
#include "unit.h"

static const char *data_dir(void)
{
	const char *d = getenv("UNIT_DATA");

	return d ? d : "tests/unit/data";
}

static void dump_text(struct strbuf *sb, const char *s, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (s[i] == '\r')
			sb_adds(sb, "\\r");
		else if (s[i] == '\\')
			sb_adds(sb, "\\\\");
		else
			sb_addch(sb, s[i]);
	}
}

static void dump(const struct fdiff *fd, size_t n, struct strbuf *sb)
{
	size_t f, i, j;

	for (f = 0; f < n; f++, fd++) {
		const char *why;

		sb_addf(sb, "file old=%s new=%s\n",
			fd->old_path ? fd->old_path : "(null)",
			fd->new_path ? fd->new_path : "(null)");
		sb_addf(sb, "flags%s%s%s%s%s%s%s\n",
			fd->binary ? " binary" : "", fd->is_new ? " new" : "",
			fd->is_deleted ? " deleted" : "",
			fd->is_rename ? " rename" : "",
			fd->mode_change ? " mode_change" : "",
			fd->mode_only ? " mode_only" : "",
			fd->combined ? " combined" : "");
		sb_addf(sb, "line_ops %s\n",
			fdiff_line_ops(fd, &why) ? "yes" : why);
		for (i = 0; i < fd->nhdr; i++)
			sb_addf(sb, "hdr %s\n", fd->hdr[i]);
		for (i = 0; i < fd->nhunks; i++) {
			const struct hunk *h = &fd->hunks[i];

			sb_addf(sb, "hunk -%d,%d +%d,%d section='%s'\n",
				h->old_start, h->old_count, h->new_start,
				h->new_count, h->section);
			for (j = 0; j < h->nlines; j++) {
				const struct dline *d = &h->lines[j];

				sb_addf(sb, "  %c %3d %3d |", d->op, d->old_no,
					d->new_no);
				dump_text(sb, d->text, d->len);
				sb_addf(sb, "|%s\n",
					d->no_eol ? " no_eol" : "");
			}
		}
	}
}

static void golden(const char *name)
{
	struct strbuf path = STRBUF_INIT, in = STRBUF_INIT, exp = STRBUF_INIT,
		      got = STRBUF_INIT, err = STRBUF_INIT;
	struct fdiff *fd = NULL;
	size_t n = 0;

	sb_addf(&path, "%s/%s", data_dir(), name);
	CHECK_INT(read_file(path.buf, &in), 0);
	if (diff_parse(in.buf, in.len, &fd, &n, &err)) {
		fprintf(stderr, "%s: %s\n", name, err.buf);
		CHECK(!"parse failed");
		goto out;
	}
	dump(fd, n, &got);
	path.len -= strlen(".diff");
	sb_adds(&path, ".out");
	if (getenv("UNIT_UPDATE")) {
		write_file(path.buf, got.buf, got.len);
	} else if (read_file(path.buf, &exp)) {
		fprintf(stderr, "missing %s\n", path.buf);
		CHECK(!"golden file missing");
	} else if (strcmp(got.buf, exp.buf)) {
		fprintf(stderr, "%s: mismatch\n--- got:\n%s--- expected:\n%s",
			name, got.buf, exp.buf);
		CHECK(!"golden mismatch");
	} else {
		CHECK(1);
	}
out:
	fdiff_free_array(fd, n);
	sb_release(&path);
	sb_release(&in);
	sb_release(&exp);
	sb_release(&got);
	sb_release(&err);
}

static void test_golden(void)
{
	DIR *d = opendir(data_dir());
	struct dirent *e;
	int count = 0;

	CHECK(d != NULL);
	if (!d)
		return;
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);

		if (l > 5 && !strcmp(e->d_name + l - 5, ".diff")) {
			golden(e->d_name);
			count++;
		}
	}
	closedir(d);
	CHECK(count >= 20);
}

static void test_bad_input(void)
{
	static const char *bad[] = {
		"diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1,2 +1,2 @@\n a\n",
		"diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n+c\n",
		"diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n*a\n",
		"diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -x +1 @@\n",
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		struct strbuf err = STRBUF_INIT;
		struct fdiff *fd = NULL;
		size_t n = 0;

		CHECK_INT(diff_parse(bad[i], strlen(bad[i]), &fd, &n, &err),
			  -1);
		CHECK(err.len > 0);
		sb_release(&err);
	}
}

static void test_quote(void)
{
	struct strbuf sb = STRBUF_INIT;
	char *s;

	{
		const char *q = "\"a\\tb\\\"c\\\\d\\303\\251\"";

		s = unquote_path(q, strlen(q));
	}
	CHECK_STR(s, "a\tb\"c\\d\303\251");
	quote_path(&sb, s);
	CHECK_STR(sb.buf, "\"a\\tb\\\"c\\\\d\303\251\"");
	free(s);
	sb_reset(&sb);
	quote_path(&sb, "plain name é");
	CHECK_STR(sb.buf, "plain name é");
	sb_release(&sb);
	s = unquote_path("noquote", 7);
	CHECK_STR(s, "noquote");
	free(s);
}

static struct fdiff *parse1(const char *text)
{
	struct fdiff *fd = NULL;
	size_t n = 0;

	if (diff_parse(text, strlen(text), &fd, &n, NULL) || n != 1)
		return NULL;
	return fd;
}

static const char sel_diff[] = "diff --git a/f b/f\n--- a/f\n+++ b/f\n"
			       "@@ -1,6 +1,6 @@\n"
			       " c1\n"	/* 1 1 */
			       "-o2\n"	/* 2 - */
			       "-o3\n"	/* 3 - */
			       "+n2\n"	/* - 2 */
			       "+n3\n"	/* - 3 */
			       " c4\n"	/* 4 4 */
			       "-o5\n"	/* 5 - */
			       "+n5\n"	/* - 5 */
			       " c6\n"; /* 6 6 */

static void sel_str(const struct fdiff *fd, char *buf)
{
	size_t j;

	for (j = 0; j < fd->hunks[0].nlines; j++)
		buf[j] = fd->hunks[0].lines[j].sel ? 'x' : '.';
	buf[j] = '\0';
}

static void check_span(const char *a, const char *b, const char *ma,
		       const char *mb)
{
	size_t s, ae, be;
	bool ok = change_span(a, strlen(a), b, strlen(b), &s, &ae, &be);

	CHECK(ok == (ma != NULL));
	if (ok && ma) {
		char x[64], y[64];

		snprintf(x, sizeof(x), "%.*s", (int)(ae - s), a + s);
		snprintf(y, sizeof(y), "%.*s", (int)(be - s), b + s);
		CHECK_STR(x, ma);
		CHECK_STR(y, mb);
	}
}

static void test_intraline(void)
{
	struct strbuf err = STRBUF_INIT;
	static const char in[] =
		"diff --git a/f b/f\n--- a/f\n+++ b/f\n@@ -1,5 +1,6 @@\n"
		" x\n-a\n-b\n+A\n+B\n+C\n y\n-c\n+D\n";
	struct fdiff *fd = NULL;
	size_t n = 0;

	CHECK_INT(diff_parse(in, strlen(in), &fd, &n, &err), 0);
	if (n == 1) {
		const struct hunk *h = &fd->hunks[0];

		/* x -a -b +A +B +C y -c +D */
		CHECK_INT((int)dline_partner(h, 0), -1);
		CHECK_INT((int)dline_partner(h, 1), 3);
		CHECK_INT((int)dline_partner(h, 2), 4);
		CHECK_INT((int)dline_partner(h, 3), 1);
		CHECK_INT((int)dline_partner(h, 4), 2);
		CHECK_INT((int)dline_partner(h, 5), -1);
		CHECK_INT((int)dline_partner(h, 7), 8);
		CHECK_INT((int)dline_partner(h, 8), 7);
	}
	fdiff_free_array(fd, n);
	sb_release(&err);
	check_span("return foo(bar);", "return foo(baz);", "bar", "baz");
	check_span("int x;", "long x;", "int", "long");
	check_span("\tfoo(a, b);", "\tfoo(a, b, c);", "", ", c");
	check_span("abc", "xyz", NULL, NULL);
	check_span("same", "same", NULL, NULL);
}

static void test_select(void)
{
	struct fdiff *fd = parse1(sel_diff);
	struct strbuf err = STRBUF_INIT;
	char buf[32];

	CHECK(fd != NULL);
	if (!fd)
		return;
	CHECK_INT(sel_apply(fd, "n2", &err), 1);
	sel_str(fd, buf);
	CHECK_STR(buf, "...x.....");
	sel_clear(fd);
	CHECK_INT(sel_apply(fd, "o3", &err), 1);
	sel_str(fd, buf);
	CHECK_STR(buf, "..x......");
	sel_clear(fd);
	/* a new-side range includes removed lines shown between */
	CHECK_INT(sel_apply(fd, "n2-5", &err), 6);
	sel_str(fd, buf);
	CHECK_STR(buf, ".xxxx.xx.");
	sel_clear(fd);
	CHECK_INT(sel_apply(fd, "n1-6", &err), 6);
	sel_clear(fd);
	CHECK_INT(sel_apply(fd, "o2,n5", &err), 2);
	sel_str(fd, buf);
	CHECK_STR(buf, ".x.....x.");
	CHECK_INT(sel_count(fd), 2);
	sel_clear(fd);
	CHECK_INT(sel_apply(fd, "n4", &err), -1); /* context */
	sb_reset(&err);
	CHECK_INT(sel_apply(fd, "n9", &err), -1); /* out of range */
	sb_reset(&err);
	CHECK_INT(sel_apply(fd, "X3", &err), -1);
	sb_reset(&err);
	/* long forms */
	sel_clear(fd);
	CHECK_INT(sel_apply(fd, "old:2,new:5", &err), 2);
	sel_str(fd, buf);
	CHECK_STR(buf, ".x.....x.");
	sel_clear(fd);
	CHECK_INT(sel_apply(fd, "new:2-5", &err), 6);
	sel_clear(fd);
	/* the old syntax is refused with a hint */
	sb_reset(&err);
	CHECK_INT(sel_apply(fd, "L2", &err), -1);
	CHECK(strstr(sb_str(&err), "n<N>") != NULL);
	sb_reset(&err);
	CHECK_INT(sel_apply(fd, "-L2", &err), -1);
	CHECK(strstr(sb_str(&err), "o<N>") != NULL);
	sb_reset(&err);
	CHECK_INT(sel_apply(fd, "n", &err), -1);
	sb_reset(&err);
	CHECK_INT(sel_apply(fd, "n2,", &err), 1);
	sel_clear(fd);
	sb_reset(&err);
	CHECK_INT(sel_apply(fd, "x2", &err), -1);
	sb_reset(&err);
	CHECK_INT(sel_apply(fd, "n5-3", &err), -1);
	fdiff_free(fd);
	free(fd);
	sb_release(&err);
}

/*
 * a -U0 diff of lines 1 to 10, with 2 removed, A B added after 5, and 8
 * replaced by X Y
 */
static const char follow_diff[] = "diff --git a/f b/f\n"
				  "index 1111111..2222222 100644\n"
				  "--- a/f\n"
				  "+++ b/f\n"
				  "@@ -2 +1,0 @@\n"
				  "-2\n"
				  "@@ -5,0 +5,2 @@\n"
				  "+A\n"
				  "+B\n"
				  "@@ -8 +9,2 @@\n"
				  "-8\n"
				  "+X\n"
				  "+Y\n";

static void test_follow(void)
{
	struct fdiff *fd = parse1(follow_diff);

	CHECK(fd != NULL);
	if (!fd)
		return;
	CHECK_INT(fdiff_follow_line(fd, 1), 1);
	CHECK_INT(fdiff_follow_line(fd, 2), 1); /* removed, the line before */
	CHECK_INT(fdiff_follow_line(fd, 3), 2);
	CHECK_INT(fdiff_follow_line(fd, 5), 4);
	CHECK_INT(fdiff_follow_line(fd, 6), 7); /* after the insertion */
	CHECK_INT(fdiff_follow_line(fd, 7), 8);
	CHECK_INT(fdiff_follow_line(fd, 8), 9); /* changed, its replacement */
	CHECK_INT(fdiff_follow_line(fd, 9), 11);
	CHECK_INT(fdiff_follow_line(fd, 10), 12);
	CHECK_INT(fdiff_follow_line(NULL, 5), 5);
	fdiff_free(fd);
	free(fd);
}

/* carve.skipDiff patterns, a name anywhere or a path from the top. */
static void test_skip_patterns(void)
{
	char *pats[] = { "tags", "cscope.*.out", "/gen/*.h", "build/out" };
	char *none[] = { "" };

	CHECK(path_matches("tags", pats, 4));
	CHECK(path_matches("drivers/net/tags", pats, 4));
	CHECK(!path_matches("tags.c", pats, 4));
	CHECK(!path_matches("tags/x.c", pats, 4));
	CHECK(path_matches("cscope.in.out", pats, 4));
	CHECK(path_matches("gen/regs.h", pats, 4));
	/* a slash pattern is anchored and * stops at a slash */
	CHECK(!path_matches("sub/gen/regs.h", pats, 4));
	CHECK(!path_matches("gen/sub/regs.h", pats, 4));
	CHECK(path_matches("build/out", pats, 4));
	CHECK(!path_matches("tags", none, 1));
	CHECK(!path_matches("tags", NULL, 0));
}

/* fdiff_dup of the second file of a parse, kept after the parse is freed */
static void test_dup(void)
{
	struct fdiff *fds = NULL, copy;
	static const char in[] =
		"diff --git a/a.c b/a.c\n--- a/a.c\n+++ b/a.c\n"
		"@@ -1 +1 @@\n-x\n+y\n"
		"diff --git a/b.c b/b.c\n--- a/b.c\n+++ b/b.c\n"
		"@@ -1,2 +1,2 @@ fn\n a\n-b\n+B\n\\ No newline at end of file\n";
	size_t n = 0;

	CHECK_INT(diff_parse(in, sizeof(in) - 1, &fds, &n, NULL), 0);
	CHECK_INT(n, 2);
	if (n != 2)
		return;
	fdiff_dup(&fds[1], &copy);
	fdiff_free_array(fds, n);
	CHECK_STR(copy.new_path, "b.c");
	CHECK_INT(copy.nhunks, 1);
	CHECK_STR(copy.hunks[0].section, "fn");
	CHECK_INT(copy.hunks[0].nlines, 3);
	CHECK(copy.hunks[0].lines[2].len == 1 &&
	      !memcmp(copy.hunks[0].lines[2].text, "B", 1));
	CHECK(copy.hunks[0].lines[2].no_eol);
	fdiff_free(&copy);
}

int main(void)
{
	RUN(test_golden);
	RUN(test_dup);
	RUN(test_skip_patterns);
	RUN(test_bad_input);
	RUN(test_quote);
	RUN(test_select);
	RUN(test_intraline);
	RUN(test_follow);
	return unit_done("diff");
}
