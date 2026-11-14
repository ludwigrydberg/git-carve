// SPDX-License-Identifier: MIT
#include <curses.h>

#include "ui/keys.h"
#include "unit.h"

static void test_names(void)
{
	int c[4];

	CHECK_INT(key_parse("x", c), 1);
	CHECK_INT(c[0], 'x');
	CHECK_INT(key_parse("Space", c), 1);
	CHECK_INT(c[0], ' ');
	CHECK_INT(key_parse("enter", c), 3);
	CHECK_INT(key_parse("Ctrl-L", c), 1);
	CHECK_INT(c[0], 12);
	CHECK_INT(key_parse("ctrl-d", c), 1);
	CHECK_INT(c[0], 4);
	CHECK_INT(key_parse("F5", c), 1);
	CHECK_INT(c[0], KEY_F(5));
	CHECK_INT(key_parse("PgDn", c), 1);
	CHECK_INT(c[0], KEY_NPAGE);
	CHECK_INT(key_parse("Shift-Tab", c), 1);
	CHECK_INT(c[0], KEY_BTAB);
	CHECK_INT(key_parse("F13", c), 0);
	CHECK_INT(key_parse("Ctrl-", c), 0);
	CHECK_INT(key_parse("Bogus", c), 0);
	CHECK_INT(key_parse("", c), 0);
	CHECK_STR(key_label(' '), "Space");
	CHECK_STR(key_label('\r'), "Enter");
	CHECK_STR(key_label(12), "Ctrl-L");
	CHECK_STR(key_label(KEY_F(12)), "F12");
	CHECK_STR(key_label(']'), "]");
}

static void test_defaults(void)
{
	struct strbuf sb = STRBUF_INIT;
	struct keymap km;
	int a;

	keymap_defaults(&km);
	CHECK_INT(key_action(&km, 'j'), ACT_DOWN);
	CHECK_INT(key_action(&km, KEY_DOWN), ACT_DOWN);
	CHECK_INT(key_action(&km, ' '), ACT_STAGE);
	CHECK_INT(key_action(&km, '\r'), ACT_STAGE_FILE);
	CHECK_INT(key_action(&km, 12), ACT_REPAINT);
	CHECK_INT(key_action(&km, 'Q'), ACT_NONE);
	/* C is free, ZZ finishes what is in progress */
	CHECK_INT(key_action(&km, 'C'), ACT_NONE);
	CHECK_INT(key_action(&km, 'Z'), ACT_PREFIX);
	CHECK_INT(key_seq_action(&km, 'Z', 'Z'), ACT_FINISH);
	keymap_describe(&km, ACT_DOWN, &sb);
	CHECK_STR(sb_str(&sb), "j, Down");
	/* as in vim, g and z start sequences, and Ctrl-F and Ctrl-B page */
	CHECK_INT(key_action(&km, 'g'), ACT_PREFIX);
	CHECK_INT(key_seq_action(&km, 'g', 'g'), ACT_TOP);
	CHECK_INT(key_seq_action(&km, 'g', '/'), ACT_SEARCH_LIST);
	CHECK_INT(key_seq_action(&km, 'g', 'q'), ACT_NONE);
	/* gc is the message pane, and g with s, r or x is no sequence */
	CHECK_INT(key_seq_action(&km, 'g', 'c'), ACT_MESSAGE_PANE);
	CHECK_INT(key_seq_action(&km, 'g', 's'), ACT_NONE);
	CHECK_INT(key_seq_action(&km, 'g', 'r'), ACT_NONE);
	CHECK_INT(key_seq_action(&km, 'g', 'x'), ACT_NONE);
	CHECK_INT(key_action(&km, 'z'), ACT_PREFIX);
	CHECK_INT(key_seq_action(&km, 'z', 'z'), ACT_SCROLL_CENTER);
	CHECK_INT(key_action(&km, 6), ACT_PAGE_DOWN); /* Ctrl-F */
	CHECK_INT(key_action(&km, 2), ACT_PAGE_UP);   /* Ctrl-B */
	CHECK_INT(key_action(&km, 'L'), ACT_SCREEN_BOTTOM);
	CHECK_INT(key_action(&km, '$'), ACT_LINE_END);
	sb_reset(&sb);
	keymap_describe(&km, ACT_TOP, &sb);
	CHECK_STR(sb_str(&sb), "Home, gg");
	sb_reset(&sb);
	/*
	 * every action has a name, help and a default key (not the
	 * internal prefix marker)
	 */
	for (a = 1; a < NACTIONS; a++) {
		if (a == ACT_PREFIX)
			continue;
		CHECK(actions[a].name && *actions[a].name);
		CHECK(actions[a].help && *actions[a].help);
		sb_reset(&sb);
		keymap_describe(&km, (enum action)a, &sb);
		CHECK(strcmp(sb_str(&sb), "(none)") != 0);
	}
	sb_release(&sb);
}

/* No two actions share a default key. */
static void test_distinct(void)
{
	static char seen[512][16];
	size_t n = 0, i;
	int a;

	for (a = 1; a < NACTIONS; a++) {
		char buf[128], *tok, *save = NULL;

		snprintf(buf, sizeof(buf), "%s", actions[a].keys);
		for (tok = strtok_r(buf, " ", &save); tok;
		     tok = strtok_r(NULL, " ", &save)) {
			for (i = 0; i < n; i++)
				if (!strcmp(seen[i], tok))
					fprintf(stderr, "key %s twice (%s)\n",
						tok, actions[a].name);
			for (i = 0; i < n; i++)
				CHECK(strcmp(seen[i], tok) != 0);
			if (n < sizeof(seen) / sizeof(*seen))
				snprintf(seen[n++], sizeof(*seen), "%s", tok);
		}
	}
	CHECK(n > 50);
}

/* After a prefix key the status line lists what can follow it. */
static void test_seq_hint(void)
{
	struct strbuf sb = STRBUF_INIT;
	struct keymap km;

	keymap_defaults(&km);
	keymap_seq_hint(&km, 'g', &sb);
	CHECK(!strncmp(sb_str(&sb), "g:", 2));
	CHECK(strstr(sb_str(&sb), "  g top") != NULL);
	CHECK(strstr(sb_str(&sb), "  c message-pane") != NULL);
	sb_reset(&sb);
	keymap_seq_hint(&km, 'Z', &sb);
	CHECK(strstr(sb_str(&sb), "  Z finish") != NULL);
	sb_release(&sb);
}

static void test_apply(void)
{
	struct strbuf warn = STRBUF_INIT;
	struct keymap km;

	/* rebinding moves the key, and takes it from the default owner */
	keymap_defaults(&km);
	keymap_apply(&km, "carve.keys.stage y Space\ncarve.keys.quit Ctrl-Q\n",
		     &warn);
	CHECK_STR(sb_str(&warn), "");
	CHECK_INT(key_action(&km, 'y'), ACT_STAGE);
	CHECK_INT(key_action(&km, ' '), ACT_STAGE);
	CHECK_INT(key_action(&km, 'q'), ACT_NONE);
	CHECK_INT(key_action(&km, 17), ACT_QUIT);

	/* taking another action's only key leaves a note */
	keymap_defaults(&km);
	sb_reset(&warn);
	keymap_apply(&km, "carve.keys.commit a\n", &warn);
	CHECK_INT(key_action(&km, 'a'), ACT_COMMIT);
	CHECK(strstr(sb_str(&warn), "select-hunk has no key") != NULL);

	/* none unbinds, and config names ignore case */
	keymap_defaults(&km);
	sb_reset(&warn);
	keymap_apply(&km, "carve.keys.Zoom none\n", &warn);
	CHECK_INT(key_action(&km, 'o'), ACT_NONE);
	CHECK_STR(sb_str(&warn), "");

	/* problems keep the defaults and are reported */
	keymap_defaults(&km);
	sb_reset(&warn);
	keymap_apply(&km,
		     "carve.keys.nosuch y\ncarve.keys.commit Bogus\n"
		     "carve.keys.amend\n",
		     &warn);
	CHECK(strstr(sb_str(&warn), "unknown action 'nosuch'") != NULL);
	CHECK(strstr(sb_str(&warn), "unknown key 'Bogus'") != NULL);
	CHECK(strstr(sb_str(&warn), "carve.keys.amend: no key given") != NULL);
	CHECK_INT(key_action(&km, 'c'), ACT_COMMIT);
	CHECK_INT(key_action(&km, 'A'), ACT_AMEND);

	/* of two user bindings for one key the first wins, with a warning */
	keymap_defaults(&km);
	sb_reset(&warn);
	keymap_apply(&km, "carve.keys.commit y\ncarve.keys.amend y\n", &warn);
	CHECK_INT(key_action(&km, 'y'), ACT_COMMIT);
	CHECK_INT(key_action(&km, 'A'), ACT_AMEND);
	CHECK(strstr(sb_str(&warn), "'y' is already bound to commit") != NULL);
	sb_release(&warn);
}

static void test_subst(void)
{
	struct strbuf sb = STRBUF_INIT, warn = STRBUF_INIT;
	struct keymap km;

	keymap_defaults(&km);
	keys_subst(&km, "{split}: finish  {next-commit}: next  {abort}", &sb,
		   false);
	CHECK_STR(sb_str(&sb), "s: finish  J: next  X");
	/* not an action, left as it is */
	sb_reset(&sb);
	keys_subst(&km, "a {b} {split", &sb, false);
	CHECK_STR(sb_str(&sb), "a {b} {split");
	/* rebound, unbound, and a % in a key doubled for a format */
	keymap_apply(&km,
		     "carve.keys.split F6\ncarve.keys.zoom none\n"
		     "carve.keys.undo %\n",
		     &warn);
	sb_reset(&sb);
	keys_subst(&km, "{split} {zoom} {undo}", &sb, false);
	CHECK_STR(sb_str(&sb), "F6 <zoom> %");
	sb_reset(&sb);
	keys_subst(&km, "{undo} undoes it", &sb, true);
	CHECK_STR(sb_str(&sb), "%% undoes it");
	sb_release(&sb);
	sb_release(&warn);
}

int main(void)
{
	RUN(test_names);
	RUN(test_defaults);
	RUN(test_distinct);
	RUN(test_seq_hint);
	RUN(test_apply);
	RUN(test_subst);
	return unit_done("keys");
}
