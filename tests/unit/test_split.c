// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <string.h>

#include "split.h"
#include "message.h"
#include "unit.h"

static const char msg[] = "drv: do things\n"
			  "\n"
			  "Body.\n"
			  "\n"
			  "Change-Id: Iabc\n"
			  "Signed-off-by: T <t@t>\n";

static void test_change_id(void)
{
	char *id = change_id_of(msg);

	CHECK_STR(id, "Iabc");
	free(id);
	CHECK(change_id_of("subject\n\nno trailer\n") == NULL);
}

static void test_strip(void)
{
	struct strbuf sb = STRBUF_INIT;

	strip_change_id(msg, &sb);
	CHECK_STR(sb.buf,
		  "drv: do things\n\nBody.\n\nSigned-off-by: T <t@t>\n");
	sb_reset(&sb);
	strip_change_id("s\n\nChange-Id: I1\n", &sb);
	CHECK_STR(sb.buf, "s\n");
	sb_release(&sb);
}

static void test_template(void)
{
	struct strbuf sb = STRBUF_INIT;

	split_template(msg, true, true, '#', NULL, &sb);
	CHECK_STR(sb.buf, msg);
	sb_reset(&sb);
	split_template(msg, true, false, '#', NULL, &sb);
	CHECK_STR(sb.buf,
		  "drv: do things\n\nBody.\n\nSigned-off-by: T <t@t>\n");
	sb_reset(&sb);
	/*
	 * later parts are signed off as the original was, with the
	 * reference below a scissors line, which leaves only it out of the
	 * message
	 */
	split_template(msg, false, false, ';', NULL, &sb);
	CHECK_STR(sb.buf,
		  "\n\nSigned-off-by: T <t@t>\n\n"
		  "; ------------------------ >8 ------------------------\n"
		  "; Do not modify or remove the line above.\n"
		  "; Everything below it is left out.\n"
		  "; Original message of the commit being split:\n"
		  ";\n; drv: do things\n;\n; Body.\n;\n"
		  "; Signed-off-by: T <t@t>\n");
	sb_reset(&sb);
	split_template(msg, false, true, '#', NULL, &sb);
	CHECK(starts_with(sb.buf,
			  "\n\nChange-Id: Iabc\nSigned-off-by: T <t@t>\n\n"
			  "# " SCISSORS_LINE));
	sb_reset(&sb);
	/* one's own sign-off after the original's */
	split_template(msg, false, true, '#', "Signed-off-by: U <u@u>", &sb);
	CHECK(starts_with(sb.buf,
			  "\n\nChange-Id: Iabc\nSigned-off-by: T <t@t>\n"
			  "Signed-off-by: U <u@u>\n\n# " SCISSORS_LINE));
	sb_reset(&sb);
	/* ... not twice */
	split_template(msg, false, false, '#', "Signed-off-by: T <t@t>", &sb);
	CHECK(starts_with(sb.buf,
			  "\n\nSigned-off-by: T <t@t>\n\n# " SCISSORS_LINE));
	sb_release(&sb);
}

static const char tagged[] = "drv: do things\n"
			     "\n"
			     "Body.\n"
			     "\n"
			     "Change-Id: Iabc\n"
			     "Signed-off-by: A <a@a>\n"
			     "Reviewed-by: R <r@r>\n"
			     "acked-by: K <k@k>\n"
			     "Reviewed-by: S <s@s>\n"
			     "Signed-off-by: M <m@m>\n";

static void test_review_tags(void)
{
	struct strbuf sb = STRBUF_INIT, dropped = STRBUF_INIT;

	strip_review_tags(tagged, &sb, &dropped);
	CHECK_STR(sb.buf, "drv: do things\n\nBody.\n\nChange-Id: Iabc\n"
			  "Signed-off-by: A <a@a>\nSigned-off-by: M <m@m>\n");
	CHECK_STR(dropped.buf, "Reviewed-by, Acked-by");
	sb_reset(&sb);
	signoffs_of(tagged, &sb);
	CHECK_STR(sb.buf, "Signed-off-by: A <a@a>\nSigned-off-by: M <m@m>\n");
	sb_reset(&sb);
	/* no part keeps the tags, not even the first ... */
	split_template(tagged, true, true, '#', NULL, &sb);
	CHECK(!strstr(sb.buf, "Reviewed-by") && !strstr(sb.buf, "cked-by"));
	CHECK(strstr(sb.buf, "Change-Id: Iabc\n") != NULL);
	sb_reset(&sb);
	/* ... nor the later ones, which get every Signed-off-by */
	split_template(tagged, false, false, '#', NULL, &sb);
	CHECK(starts_with(sb.buf,
			  "\n\nSigned-off-by: A <a@a>\n"
			  "Signed-off-by: M <m@m>\n\n# " SCISSORS_LINE));
	CHECK(!strstr(sb.buf, "Reviewed-by"));
	sb_release(&sb);
	sb_release(&dropped);
}

int main(void)
{
	RUN(test_change_id);
	RUN(test_strip);
	RUN(test_template);
	RUN(test_review_tags);
	return unit_done("split");
}
