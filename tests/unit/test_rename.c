// SPDX-License-Identifier: MIT
#include "rename.h"
#include "unit.h"

static void test_parse(void)
{
	static const char in[] = "M\0a.c\0R087\0old.c\0new dir/new.c\0"
				 "C100\0x.c\0y.c\0D\0gone.c\0R100\0p\0q\0";
	struct rename *v;
	size_t n = parse_renames(in, sizeof(in) - 1, true, &v);

	CHECK_INT((int)n, 2);
	if (n == 2) {
		CHECK_STR(v[0].from, "old.c");
		CHECK_STR(v[0].to, "new dir/new.c");
		CHECK_INT(v[0].score, 87);
		CHECK(v[0].staged);
		CHECK_STR(v[1].from, "p");
		CHECK_INT(v[1].score, 100);
	}
	free_renames(v, n);
}

static void test_apply(void)
{
	/* staged a to b, work tree c to d, e deleted in both index and tree */
	static const char st[] = "D  a\0A  b\0MD c\0?? d\0 D e\0";
	struct file_status *fs;
	struct rename rv[] = {
		{ (char *)"a", (char *)"b", true, 100 },
		{ (char *)"c", (char *)"d", false, 90 },
		{ (char *)"x", (char *)"y", false, 90 }, /* not listed */
	};
	size_t n;

	n = parse_status(st, sizeof(st) - 1, &fs);

	n = apply_renames(fs, n, rv, 3);
	CHECK_INT((int)n, 4);
	if (n == 4) {
		/* a is gone, and c keeps its staged change */
		CHECK_STR(fs[0].path, "b");
		CHECK_INT(fs[0].x, 'R');
		CHECK_STR(fs[0].from_x, "a");
		CHECK_STR(fs[1].path, "c");
		CHECK_INT(fs[1].x, 'M');
		CHECK_INT(fs[1].y, ' ');
		CHECK_STR(fs[2].path, "d");
		CHECK_INT(fs[2].y, 'R');
		CHECK_STR(fs[2].from_y, "c");
		CHECK(fs[2].from_x == NULL);
		CHECK_STR(fs[3].path, "e");
	}
	free_status(fs, n);
}

int main(void)
{
	RUN(test_parse);
	RUN(test_apply);
	return unit_done("rename");
}
