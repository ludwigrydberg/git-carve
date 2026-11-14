// SPDX-License-Identifier: MIT
#include <stdlib.h>

#include "patch.h"
#include "unit.h"

#define HDR "diff --git a/f b/f\n--- a/f\n+++ b/f\n"

struct pcase {
	const char *name;
	const char *diff;
	const char *sel;
	enum patch_dir dir;
	const char *expect; /* NULL for no hunks */
};

static const char mixed[] = HDR "@@ -1,5 +1,5 @@\n"
				" a\n"
				"-b\n"
				"+B\n"
				" c\n"
				"-d\n"
				"+D\n"
				" e\n";

static const struct pcase cases[] = {
	/*
	 * staging keeps the selected lines, drops an unselected + and
	 * makes an unselected - context
	 */
	{ "stage selected pair", mixed, "o2,n2", PATCH_STAGE,
	  HDR "@@ -1,5 +1,5 @@\n a\n-b\n+B\n c\n d\n e\n" },
	{ "stage only '+'", mixed, "n2", PATCH_STAGE,
	  HDR "@@ -1,5 +1,6 @@\n a\n b\n+B\n c\n d\n e\n" },
	{ "stage only '-'", mixed, "o4", PATCH_STAGE,
	  HDR "@@ -1,5 +1,4 @@\n a\n b\n c\n-d\n e\n" },
	/* unstaging makes an unselected + context and drops an unselected - */
	{ "unstage selected pair", mixed, "o2,n2", PATCH_UNSTAGE,
	  HDR "@@ -1,5 +1,5 @@\n a\n-b\n+B\n c\n D\n e\n" },
	{ "unstage only '+'", mixed, "n4", PATCH_UNSTAGE,
	  HDR "@@ -1,4 +1,5 @@\n a\n B\n c\n+D\n e\n" },
	{ "unstage only '-'", mixed, "o2", PATCH_UNSTAGE,
	  HDR "@@ -1,6 +1,5 @@\n a\n-b\n B\n c\n D\n e\n" },
	{ "nothing selected", mixed, NULL, PATCH_STAGE, NULL },

	/* hunks without a selection are left out, and later starts shift */
	{ "second hunk shifts",
	  HDR "@@ -1,2 +1,3 @@\n a\n+x\n b\n"
	      "@@ -10,2 +11,3 @@\n j\n+y\n k\n",
	  "n1-3,n12", PATCH_STAGE,
	  HDR "@@ -1,2 +1,3 @@\n a\n+x\n b\n"
	      "@@ -10,2 +11,3 @@\n j\n+y\n k\n" },
	{ "first hunk skipped",
	  HDR "@@ -1,2 +1,3 @@\n a\n+x\n b\n"
	      "@@ -10,2 +11,3 @@\n j\n+y\n k\n",
	  "n12", PATCH_STAGE, HDR "@@ -10,2 +10,3 @@\n j\n+y\n k\n" },
	{ "unstage first hunk skipped",
	  HDR "@@ -1,2 +1,3 @@\n a\n+x\n b\n"
	      "@@ -10,2 +11,3 @@\n j\n+y\n k\n",
	  "n12", PATCH_UNSTAGE, HDR "@@ -11,2 +11,3 @@\n j\n+y\n k\n" },

	/* new file (i-t-a): -0,0 stays */
	{ "new file one line",
	  "diff --git a/f b/f\nnew file mode 100644\n"
	  "--- /dev/null\n+++ b/f\n@@ -0,0 +1,3 @@\n+a\n+b\n+c\n",
	  "n2", PATCH_STAGE, HDR "@@ -0,0 +1,1 @@\n+b\n" },
	{ "unstage from new file",
	  "diff --git a/f b/f\nnew file mode 100644\n"
	  "--- /dev/null\n+++ b/f\n@@ -0,0 +1,3 @@\n+a\n+b\n+c\n",
	  "n2", PATCH_UNSTAGE, HDR "@@ -1,2 +1,3 @@\n a\n+b\n c\n" },
	{ "unstage whole new file",
	  "diff --git a/f b/f\nnew file mode 100644\n"
	  "--- /dev/null\n+++ b/f\n@@ -0,0 +1,2 @@\n+a\n+b\n",
	  "n1-2", PATCH_UNSTAGE, HDR "@@ -0,0 +1,2 @@\n+a\n+b\n" },

	/* no newline at end of file */
	{ "noeol old, stage '+' only",
	  HDR "@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+b\n",
	  "n2", PATCH_STAGE,
	  HDR "@@ -1,2 +1,3 @@\n a\n-b\n\\ No newline at end of file\n"
	      "+b\n+b\n" },
	{ "noeol old, stage '-' only",
	  HDR "@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+b\n",
	  "o2", PATCH_STAGE,
	  HDR "@@ -1,2 +1,1 @@\n a\n-b\n\\ No newline at end of file\n" },
	{ "noeol new, stage '+' only",
	  HDR "@@ -1,2 +1,2 @@\n a\n-b\n+b\n\\ No newline at end of file\n",
	  "n2", PATCH_STAGE,
	  HDR "@@ -1,2 +1,3 @@\n a\n b\n+b\n\\ No newline at end of file\n" },
	{ "noeol new, stage '-' only",
	  HDR "@@ -1,2 +1,2 @@\n a\n-b\n+b\n\\ No newline at end of file\n",
	  "o2", PATCH_STAGE, HDR "@@ -1,2 +1,1 @@\n a\n-b\n" },
	{ "noeol both, stage '+' only",
	  HDR "@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n"
	      "+c\n\\ No newline at end of file\n",
	  "n2", PATCH_STAGE,
	  HDR "@@ -1,2 +1,3 @@\n a\n-b\n\\ No newline at end of file\n"
	      "+b\n+c\n\\ No newline at end of file\n" },
	{ "noeol context, change above",
	  HDR "@@ -1,2 +1,2 @@\n-a\n+A\n b\n\\ No newline at end of file\n",
	  "n1", PATCH_STAGE,
	  HDR "@@ -1,2 +1,3 @@\n a\n+A\n b\n\\ No newline at end of file\n" },
	{ "unstage noeol old '-' with '+' kept",
	  HDR "@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+c\n",
	  "o2", PATCH_UNSTAGE, HDR "@@ -1,3 +1,2 @@\n a\n-b\n c\n" },
	{ "unstage noeol new '+' only",
	  HDR "@@ -1,2 +1,2 @@\n a\n-b\n+c\n\\ No newline at end of file\n",
	  "n2", PATCH_UNSTAGE,
	  HDR "@@ -1,1 +1,2 @@\n a\n+c\n\\ No newline at end of file\n" },
	{ "unstage noeol both, '-' only",
	  HDR "@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n"
	      "+c\n\\ No newline at end of file\n",
	  "o2", PATCH_UNSTAGE,
	  HDR "@@ -1,3 +1,2 @@\n a\n-b\n c\n"
	      "\\ No newline at end of file\n" },

	/* change blocks pair up, and taking one pair keeps its place */
	{ "stage one pair of a block",
	  HDR "@@ -1,3 +1,3 @@\n-a\n-b\n+A\n+B\n c\n", "o1,n1", PATCH_STAGE,
	  HDR "@@ -1,3 +1,3 @@\n-a\n+A\n b\n c\n" },
	{ "stage second pair of a block",
	  HDR "@@ -1,3 +1,3 @@\n-a\n-b\n+A\n+B\n c\n", "o2,n2", PATCH_STAGE,
	  HDR "@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n" },
	{ "unstage one pair of a block",
	  HDR "@@ -1,3 +1,3 @@\n-a\n-b\n+A\n+B\n c\n", "o1,n1", PATCH_UNSTAGE,
	  HDR "@@ -1,3 +1,3 @@\n-a\n+A\n B\n c\n" },
	{ "unequal block: leftovers follow the pairs",
	  HDR "@@ -1,4 +1,2 @@\n-a\n-b\n-c\n+A\n d\n", "o1,n1", PATCH_STAGE,
	  HDR "@@ -1,4 +1,4 @@\n-a\n+A\n b\n c\n d\n" },

	/* paths needing care */
	{ "path with space",
	  "diff --git a/x y b/x y\n--- a/x y\t\n"
	  "+++ b/x y\t\n@@ -1 +1 @@\n-a\n+b\n",
	  "n1", PATCH_STAGE,
	  "diff --git a/x y b/x y\n--- a/x y\t\n+++ b/x y\t\n"
	  "@@ -1,1 +1,2 @@\n a\n+b\n" },
};

static struct fdiff *parse1(const char *text)
{
	struct strbuf err = STRBUF_INIT;
	struct fdiff *fd = NULL;
	size_t n = 0;

	if (diff_parse(text, strlen(text), &fd, &n, &err) || n != 1) {
		fprintf(stderr, "parse: %s\n", err.buf ? err.buf : "n != 1");
		sb_release(&err);
		return NULL;
	}
	sb_release(&err);
	return fd;
}

static void run_case(const struct pcase *c)
{
	struct strbuf out = STRBUF_INIT, err = STRBUF_INIT;
	struct fdiff *fd = parse1(c->diff);
	int nh;

	CHECK(fd != NULL);
	if (!fd)
		return;
	if (c->sel && sel_apply(fd, c->sel, &err) < 0) {
		fprintf(stderr, "%s: bad selection: %s\n", c->name, err.buf);
		CHECK(0);
		goto out;
	}
	nh = build_patch(fd, c->dir, &out, &err);
	if (!c->expect) {
		CHECK_INT(nh, 0);
	} else {
		if (strcmp(sb_str(&out), c->expect))
			fprintf(stderr, "case: %s\n", c->name);
		CHECK_STR(sb_str(&out), c->expect);
	}
out:
	fdiff_free(fd);
	free(fd);
	sb_release(&out);
	sb_release(&err);
}

static void test_cases(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		run_case(&cases[i]);
}

static void test_refuse(void)
{
	static const char bin[] = "diff --git a/b b/b\nindex 1..2 100644\n"
				  "Binary files a/b and b/b differ\n";
	struct strbuf out = STRBUF_INIT, err = STRBUF_INIT;
	struct fdiff *fd = parse1(bin);

	CHECK(fd != NULL);
	if (!fd)
		return;
	CHECK_INT(build_patch(fd, PATCH_STAGE, &out, &err), -1);
	CHECK(strstr(sb_str(&err), "binary") != NULL);
	fdiff_free(fd);
	free(fd);
	sb_release(&out);
	sb_release(&err);
}

int main(void)
{
	RUN(test_cases);
	RUN(test_refuse);
	return unit_done("patch");
}
