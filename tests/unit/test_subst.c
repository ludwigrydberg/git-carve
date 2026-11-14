// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <string.h>

#include "subst.h"
#include "unit.h"

/* expr on line, the result or "(error)" or "(none)". */
static char *run(const char *expr, const char *line)
{
	struct strbuf err = STRBUF_INIT, out = STRBUF_INIT;
	struct subst s;
	char *res;

	if (subst_parse(expr, &s, &err)) {
		sb_release(&err);
		return xstrdup("(error)");
	}
	if (!subst_line(&s, line, strlen(line), &out))
		res = xstrdup("(none)");
	else
		res = xstrdup(sb_str(&out));
	subst_free(&s);
	sb_release(&out);
	return res;
}

#define CHECK_SUB(expr, line, want)          \
	do {                                 \
		char *got = run(expr, line); \
		CHECK_STR(got, want);        \
		free(got);                   \
	} while (0)

static void test_line(void)
{
	CHECK_SUB("s/100/500/", "\tfdrv_set_timeout(fd, 100);",
		  "\tfdrv_set_timeout(fd, 500);");
	/* the first match only, unless g */
	CHECK_SUB("s/a/b/", "aaa", "baa");
	CHECK_SUB("s/a/b/g", "aaa", "bbb");
	/* the last delimiter may go, another delimiter, an escaped one */
	CHECK_SUB("s/foo/bar", "foo()", "bar()");
	CHECK_SUB("s#a/b#c#", "x a/b", "x c");
	CHECK_SUB("s/a\\/b/c/", "a/b", "c");
	/* extended regex, & and groups */
	CHECK_SUB("s/(bar)\\(dev, FLAG_X\\)/\\1(dev)/",
		  "return bar(dev, FLAG_X);", "return bar(dev);");
	CHECK_SUB("s/[0-9]+/<&>/g", "a 1 b 22", "a <1> b <22>");
	CHECK_SUB("s/x/\\&/", "x", "&");
	CHECK_SUB("s/FOO/bar/i", "foo", "bar");
	CHECK_SUB("s/zzz/y/", "abc", "(none)");
	/* an empty match does not loop */
	CHECK_SUB("s/q*/-/g", "ab", "-a-b-");
	CHECK_SUB("s/^/# /", "line", "# line");
	/* errors */
	CHECK_SUB("x/a/b/", "a", "(error)");
	CHECK_SUB("s//b/", "a", "(error)");
	CHECK_SUB("s/a/b/q", "a", "(error)");
	CHECK_SUB("s/(/b/", "a", "(error)");
}

static void test_ranges(void)
{
	struct strbuf err = STRBUF_INIT;
	struct line_range *rg;
	size_t n;

	CHECK_INT(subst_ranges("n12,3-5,new:7", &rg, &n, &err), 0);
	CHECK_INT((int)n, 3);
	CHECK_INT(rg[0].from, 12);
	CHECK_INT(rg[1].from, 3);
	CHECK_INT(rg[1].to, 5);
	CHECK_INT(rg[2].to, 7);
	free(rg);
	/* old-side lines are not in the work tree */
	CHECK_INT(subst_ranges("o12", &rg, &n, &err), -1);
	CHECK(strstr(err.buf, "old-side") != NULL);
	sb_reset(&err);
	CHECK_INT(subst_ranges("n5-3", &rg, &n, &err), -1);
	CHECK_INT(subst_ranges("", &rg, &n, &err), -1);
	CHECK_INT(subst_ranges("n1,x", &rg, &n, &err), -1);
	sb_release(&err);
}

int main(void)
{
	RUN(test_line);
	RUN(test_ranges);
	return unit_done("subst");
}
