// SPDX-License-Identifier: MIT
#include "conflict.h"
#include "unit.h"

static const char merge[] = "a\n"
			    "<<<<<<< HEAD\n"
			    "ours 1\n"
			    "ours 2\n"
			    "=======\n"
			    "theirs\n"
			    ">>>>>>> 1234567 (C3: touch f.c)\n"
			    "b\n"
			    "<<<<<<< HEAD\r\n"
			    "x\r\n"
			    "||||||| parent of 1234567\r\n"
			    "base\r\n"
			    "=======\r\n"
			    ">>>>>>> 1234567\r\n"
			    "c\n";

static size_t parse(const char *s, struct conflict **c)
{
	return conflicts_parse(s, strlen(s), c);
}

static void test_parse(void)
{
	struct conflict *c;
	size_t n;

	n = conflicts_parse(merge, sizeof(merge) - 1, &c);

	CHECK_INT((int)n, 2);
	if (n != 2)
		return;
	CHECK_INT(c[0].line, 2);
	CHECK_STR(c[0].ours_label, "HEAD");
	CHECK_STR(c[0].theirs_label, "1234567 (C3: touch f.c)");
	CHECK(!strncmp(merge + c[0].ours, "ours 1\nours 2\n",
		       c[0].ours_end - c[0].ours));
	CHECK_INT((int)(c[0].ours_end - c[0].ours), 14);
	CHECK_INT((int)(c[0].base_end - c[0].base), 0);
	CHECK(!strncmp(merge + c[0].theirs, "theirs\n", 7));
	CHECK_INT((int)(c[0].theirs_end - c[0].theirs), 7);
	CHECK(!strncmp(merge + c[0].start, "<<<<<<<", 7));
	CHECK(!strncmp(merge + c[0].end, "b\n", 2));
	/* diff3 style, CRLF, an empty side */
	CHECK_INT(c[1].line, 9);
	CHECK(!strncmp(merge + c[1].base, "base\r\n",
		       c[1].base_end - c[1].base));
	CHECK_INT((int)(c[1].theirs_end - c[1].theirs), 0);
	CHECK(!strncmp(merge + c[1].end, "c\n", 2));
	conflicts_free(c, n);

	/*
	 * not markers, such as longer runs, a label on the separator, or
	 * unbalanced ones
	 */
	n = parse("<<<<<<<< x\n=======\n>>>>>>>\n", &c);
	CHECK_INT((int)n, 0);
	conflicts_free(c, n);
	n = parse("<<<<<<<\na\n======= x\nb\n>>>>>>>\n", &c);
	CHECK_INT((int)n, 0);
	conflicts_free(c, n);
	n = parse("<<<<<<< a\nx\n<<<<<<< b\ny\n=======\n>>>>>>> c\n", &c);
	CHECK_INT((int)n, 1);
	if (n == 1)
		CHECK_STR(c[0].ours_label, "b");
	conflicts_free(c, n);
}

int main(void)
{
	RUN(test_parse);
	return unit_done("conflict");
}
