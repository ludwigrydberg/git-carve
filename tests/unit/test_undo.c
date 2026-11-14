// SPDX-License-Identifier: MIT
#include "undo.h"
#include "unit.h"

#define T1 "1111111111111111111111111111111111111111"
#define T2 "2222222222222222222222222222222222222222"
#define H1 "3333333333333333333333333333333333333333"
#define H2 "4444444444444444444444444444444444444444"
#define B1 "5555555555555555555555555555555555555555"
#define B2 "6666666666666666666666666666666666666666"

static void test_roundtrip(void)
{
	struct undo_state before = { .head = (char *)H1, .index = (char *)T1 };
	struct undo_state after = { .head = (char *)H2, .index = (char *)T2 };
	struct strbuf obj = STRBUF_INIT;
	struct undo_entry e;

	sb_adds(&obj,
		"tree " T1 "\nparent " H1 "\n"
		"author git-carve <git-carve@localhost> 1577836800 +0000\n"
		"committer git-carve <git-carve@localhost> 1577836801 "
		"+0000\n\n");
	undo_format("stage", "f.c o1,n1", &before, &after, -1, &obj);
	CHECK_INT(undo_parse(obj.buf, obj.len, &e), 0);
	CHECK_STR(e.op, "stage");
	CHECK_STR(e.what, "f.c o1,n1");
	CHECK_STR(e.before.index, T1);
	CHECK_STR(e.before.head, H1);
	CHECK_STR(e.after.head, H2);
	CHECK_STR(e.after.index, T2);
	CHECK_INT(e.pending_seq, -1);
	CHECK_INT((int)e.before.nfiles, 0);
	CHECK_INT((int)e.time, 1577836801);
	undo_entry_release(&e);
	sb_release(&obj);
}

static void test_unborn_pending(void)
{
	/*
	 * a root commit's split, without a parent, pending, with a
	 * conflicted index
	 */
	struct undo_state before = { .head = (char *)"-", .index = (char *)T1 };
	struct undo_state after = { .head = (char *)"-" };
	struct strbuf obj = STRBUF_INIT;
	struct undo_entry e;

	sb_adds(&obj, "tree " T1 "\n"
		      "committer git-carve <git-carve@localhost> 5 +0000\n\n");
	undo_format("split", "", &before, &after, 7, &obj);
	CHECK(strstr(obj.buf, "carve undo: split\n") != NULL);
	CHECK_INT(undo_parse(obj.buf, obj.len, &e), 0);
	CHECK_STR(e.before.head, "-");
	CHECK_STR(e.after.head, "-");
	CHECK(e.after.index == NULL);
	CHECK_STR(e.what, "");
	CHECK_INT(e.pending_seq, 7);
	undo_entry_release(&e);
	sb_release(&obj);

	/* not an undo entry */
	CHECK_INT(undo_parse("tree " T1 "\n\nhello\n",
			     strlen("tree " T1 "\n\nhello\n"), &e),
		  -1);
}

static void test_files(void)
{
	struct strbuf obj = STRBUF_INIT;
	/*
	 * a discard on an unborn HEAD, where the saved files are the only
	 * parent
	 */
	struct undo_file bf[2] = {
		{ (char *)"dir/a b.c", (char *)B1, (char *)"100755" },
		{ (char *)"new.c", (char *)B2, (char *)"100644" },
	};
	struct undo_state before = { .head = (char *)"-",
				     .index = (char *)T1,
				     .files = bf,
				     .nfiles = 2 };
	struct undo_file af[2] = {
		{ (char *)"dir/a b.c", (char *)B2, (char *)"100644" },
		{ (char *)"new.c", (char *)"-", (char *)"-" },
	};
	struct undo_state after = { .head = (char *)"-",
				    .index = (char *)T1,
				    .stamp = (char *)"abcd-100",
				    .files = af,
				    .nfiles = 2 };
	struct undo_entry e;

	sb_adds(&obj, "tree " T1 "\nparent " H2 "\n"
		      "committer git-carve <git-carve@localhost> 5 +0000\n\n");
	undo_format("discard", "dir/a b.c n3", &before, &after, -1, &obj);
	CHECK_INT(undo_parse(obj.buf, obj.len, &e), 0);
	/* before-head wins over the parent, which holds the files */
	CHECK_STR(e.before.head, "-");
	CHECK_INT((int)e.before.nfiles, 2);
	CHECK_INT((int)e.after.nfiles, 2);
	if (e.before.nfiles == 2 && e.after.nfiles == 2) {
		CHECK_STR(e.before.files[0].path, "dir/a b.c");
		CHECK_STR(e.before.files[0].blob, B1);
		CHECK_STR(e.before.files[0].mode, "100755");
		CHECK_STR(e.after.files[0].mode, "100644");
		CHECK_STR(e.after.files[1].blob, "-");
		CHECK(undo_state_equal(&e.after, &after));
		CHECK_STR(e.after.stamp, "abcd-100");
		CHECK(!undo_state_equal(&e.before, &after));
	}
	undo_entry_release(&e);
	sb_release(&obj);
}

static void test_equal(void)
{
	struct undo_state a = { .head = (char *)H1, .index = (char *)T1 },
			  b = { .head = (char *)H1, .index = (char *)T1 },
			  c = { .head = (char *)H1 };

	CHECK(undo_state_equal(&a, &b));
	CHECK(!undo_state_equal(&a, &c));
	CHECK(!undo_state_equal(&c, &c));
}

int main(void)
{
	RUN(test_roundtrip);
	RUN(test_unborn_pending);
	RUN(test_files);
	RUN(test_equal);
	return unit_done("undo");
}
