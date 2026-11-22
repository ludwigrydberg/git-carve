// SPDX-License-Identifier: MIT
/* Key names, default bindings and user bindings from git config. */
#include "keys.h"
#include "ui_int.h"

#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const struct action_info actions[NACTIONS] = {
	[ACT_NONE] = { "", "", "", "" },
	[ACT_QUIT] = { "quit", "q", "quit", "quit" },
	[ACT_HELP] = { "help", "?", "the keys of this pane, again for all keys",
		       "this help" },
	[ACT_NEXT_PANE] = { "next-pane", "Tab", "next pane",
			    "next pane: Files, Unstaged, Staged" },
	[ACT_PREV_PANE] = { "prev-pane", "Shift-Tab", "previous pane",
			    "previous pane" },
	[ACT_DOWN] = { "down", "j Down", "down", "move down" },
	[ACT_UP] = { "up", "k Up", "up", "move up" },
	[ACT_PAGE_DOWN] = { "page-down", "PgDn Ctrl-F", "page down",
			    "page down" },
	[ACT_PAGE_UP] = { "page-up", "PgUp Ctrl-B", "page up", "page up" },
	[ACT_TOP] = { "top", "gg Home", "to the top",
		      "go to the top (in Files: the first file)" },
	[ACT_BOTTOM] = { "bottom", "G End", "to the bottom",
			 "go to the bottom" },
	[ACT_SCROLL_LEFT] = { "scroll-left", "h Left",
			      "scroll left, back to the start of long lines",
			      "show more of the left: the start of long "
			      "lines, paths, subjects (a mark at a row's "
			      "edge: more there)" },
	[ACT_SCROLL_RIGHT] = { "scroll-right", "l Right",
			       "scroll right, to the end of long lines",
			       "show more of the right: the end of long "
			       "lines, paths, subjects" },
	[ACT_HALF_DOWN] = { "half-page-down", "Ctrl-D", "half a page down",
			    "half a page down" },
	[ACT_HALF_UP] = { "half-page-up", "Ctrl-U", "half a page up",
			  "half a page up" },
	[ACT_LINE_DOWN] = { "scroll-down", "Ctrl-E", "scroll down a line",
			    "scroll the view down a line (the cursor stays "
			    "if it can)" },
	[ACT_LINE_UP] = { "scroll-up", "Ctrl-Y", "scroll up a line",
			  "scroll the view up a line" },
	[ACT_SCROLL_CENTER] = { "scroll-center", "zz",
				"the cursor line to the middle",
				"scroll the cursor line to the middle" },
	[ACT_SCROLL_TOP] = { "scroll-cursor-top", "zt",
			     "the cursor line to the top",
			     "scroll the cursor line to the top" },
	[ACT_SCROLL_BOTTOM] = { "scroll-cursor-bottom", "zb",
				"the cursor line to the bottom",
				"scroll the cursor line to the bottom" },
	[ACT_SCREEN_TOP] = { "screen-top", "H",
			     "the cursor to the top of the view",
			     "cursor to the top line of the view" },
	[ACT_SCREEN_MIDDLE] = { "screen-middle", "M",
				"the cursor to the middle of the view",
				"cursor to the middle of the view" },
	[ACT_SCREEN_BOTTOM] = { "screen-bottom", "L",
				"the cursor to the bottom of the view",
				"cursor to the bottom line of the view" },
	[ACT_LINE_START] = { "line-start", "0",
			     "back to the start of the lines",
			     "scroll the diff text back to the start of the "
			     "lines" },
	[ACT_LINE_END] = { "line-end", "$", "to the end of the cursor line",
			   "scroll the diff text to the end of the cursor "
			   "line" },
	[ACT_NEXT_HUNK] = { "next-hunk", "]", "next hunk",
			    "next hunk (or conflict)" },
	[ACT_PREV_HUNK] = { "prev-hunk", "[", "previous hunk",
			    "previous hunk (or conflict)" },
	[ACT_NEXT_CHANGE] = { "next-change", ")", "next change",
			      "next block of changed lines" },
	[ACT_PREV_CHANGE] = { "prev-change", "(", "previous change",
			      "previous block of changed lines" },
	[ACT_NEXT_FILE] = { "next-file", "}", "next file",
			    "next file with changes in this pane" },
	[ACT_PREV_FILE] = { "prev-file", "{", "previous file",
			    "previous file with changes in this pane" },
	[ACT_MORE_CONTEXT] = { "more-context", "+ =", "more context lines",
			       "more context lines in the diffs" },
	[ACT_LESS_CONTEXT] = { "less-context", "-", "fewer context lines",
			       "fewer context lines (down to 0)" },
	[ACT_FUNC_CONTEXT] = { "function-context", "W",
			       "whole functions on or off",
			       "show whole functions around changes (git diff "
			       "-W) on/off" },
	[ACT_IGNORE_WS] = { "ignore-whitespace", "w",
			    "ignore whitespace on or off, a view only",
			    "ignore whitespace changes on/off; a view only: "
			    "line operations are off meanwhile" },
	[ACT_GOTO] = { "goto-line", ":",
		       "go to a line, or replace with :s/old/new/",
		       "go to line: {goto-line}N or {goto-line}nN new side, "
		       "{goto-line}oN old side; replace: "
		       "{goto-line}s/pattern/replacement/[gi] on the selected "
		       "lines, {goto-line}%s/.../ on the whole file (the work "
		       "tree)" },
	[ACT_SEARCH] = { "search", "/",
			 "search the diffs, in Files filter the list",
			 "search the diffs of all files (smart case); in the "
			 "Files pane: filter the list by path as you type" },
	[ACT_SEARCH_NEXT] = { "search-next", "n", "next search hit",
			      "next search hit, on into the next files" },
	[ACT_SEARCH_PREV] = { "search-prev", "N", "previous search hit",
			      "previous search hit" },
	[ACT_SEARCH_LIST] = { "search-list", "g/", "list every search hit",
			      "list every search hit in all files, to jump to" },
	[ACT_STAGE] = { "stage", "Space",
			"stage or unstage the line, range or hunk",
			"stage (Unstaged) / unstage (Staged) the line, range, "
			"marked lines, or on a hunk header the hunk" },
	[ACT_STAGE_FILE] = { "stage-file", "Enter",
			     "stage or unstage the whole file",
			     "Files pane: stage/unstage the whole file; on a "
			     "section's row (Staged, Unstaged) all its files, "
			     "those the filter shows" },
	[ACT_RANGE] = { "range", "v", "start or end a range",
			"start/stop a range selection (in Files: of files, "
			"for {stage-file})" },
	[ACT_MARK] = { "mark", "m", "mark or unmark a line",
		       "mark/unmark a line. Stage, fixup and edit on a "
		       "commit's lines then act on the marked lines" },
	[ACT_SELECT_HUNK] = { "select-hunk", "a", "select the whole hunk",
			      "select the whole hunk" },
	[ACT_CLEAR] = { "clear", "Esc",
			"clear the range, the marks or the filter",
			"clear the range and the marks (Files pane: the "
			"filter)" },
	[ACT_EDIT] = { "edit", "e", "edit the file at the line, or the message",
		       "edit what the cursor is on: the file at the line, a "
		       "commit's lines as the commit has them, a commit's "
		       "message in the Series pane" },
	[ACT_DISCARD] = { "discard", "d", "throw the unstaged lines away",
			  "throw the selected unstaged lines (Files pane: the "
			  "file's unstaged changes) away, after asking; "
			  "{undo} undoes it" },
	[ACT_OURS] = { "take-ours", "O", "take our side of the conflict",
		       "conflicts: take our side of the one under the cursor "
		       "(Files pane: of the whole file)" },
	[ACT_THEIRS] = { "take-theirs", "T", "take their side of the conflict",
			 "conflicts: take their side, likewise" },
	[ACT_COMMIT] = { "commit", "c", "commit what is staged",
			 "commit what is staged" },
	[ACT_AMEND] = { "amend", "A", "amend HEAD with what is staged",
			"amend HEAD" },
	[ACT_SIGNOFF] = { "signoff", "Ctrl-S", "sign off commits on or off",
			  "sign off commits on/off (Signed-off-by, no "
			  "duplicates)" },
	[ACT_SPLIT] = { "split", "s", "split HEAD, or the commit shown",
			"split HEAD, or the older commit shown (the commits "
			"above are replayed when it is finished); "
			"{finish} finishes it" },
	[ACT_FIXUP] = { "fixup-to", "t",
			"move the lines into an older commit now",
			"move the selection, the staged changes or the file "
			"into an older commit (b in the list: the commit that "
			"last touched the lines); on a commit's lines: into "
			"another commit, or a new one in a gap" },
	[ACT_ABSORB] = { "absorb", "ga",
			 "move every change into the commit it belongs to",
			 "move every unstaged change block into the commit of "
			 "the series that last touched its lines (a plan "
			 "first)" },
	[ACT_NEXT_COMMIT] = { "next-commit", "J", "show the next older commit",
			      "the next (older) commit of the Series pane, "
			      "below" },
	[ACT_PREV_COMMIT] = { "prev-commit", "K", "show the next newer commit",
			      "the previous (newer) commit of the Series pane, "
			      "above, up to the work tree" },
	[ACT_SHOW_COMMIT] = { "show-commit", "D",
			      "the whole commit: message, stat and full diff",
			      "show the commit shown in the Commit section, "
			      "else HEAD (message, stat and diff)" },
	[ACT_FINISH] = { "finish", "ZZ", "finish the split or move in progress",
			 "finish what is in progress: a split, a move or "
			 "fixup-to after its conflict was resolved" },
	[ACT_REMOVE] = { "remove", "R", "delete the file and stage it (git rm)",
			 "Files pane: delete the file and stage its deletion "
			 "(git rm), after asking; {undo} undoes it" },
	[ACT_MARK_PAIR] = { "mark-pair", "gm",
			    "mark a changed line and its pair",
			    "mark a changed line and its pair on the other "
			    "side (-a +A), or unmark both" },
	[ACT_ABORT] = { "abort", "X", "give up the operation in progress",
			"give up what is in progress: a split, a move, "
			"fixup-to; everything as it was" },
	[ACT_UNDO] = { "undo", "u", "undo the last change to the index or HEAD",
		       "undo the last change to the index or HEAD (the work "
		       "tree is not touched)" },
	[ACT_REFRESH] = { "refresh", "F5",
			  "read the status and the diffs again",
			  "read the status, the diffs and the configuration "
			  "again ({repaint} does too)" },
	[ACT_REWORD] = { "reword", "r", "a new message for the commit shown",
			 "a new message for the commit shown (none shown: "
			 "HEAD's), in the editor; the commits above it are "
			 "replayed" },
	[ACT_ZOOM] = { "zoom", "o", "this pane on the whole screen, or back",
		       "the active pane on the whole screen (Files, Series "
		       "or a diff), or back; {next-pane} goes on zoomed" },
	[ACT_RESIZE] = { "resize", "Ctrl-W", "resize the panes",
			 "resize the panes: {scroll-left} / {scroll-right} "
			 "move the divider between the lists and the diffs, "
			 "{down} / {up} the active pane's divider, = puts the "
			 "layout back, Enter ends it" },
	[ACT_SERIES_PANE] = { "series-pane", "gS",
			      "hide or show the Series pane",
			      "hide the Series pane, or show it again "
			      "(carve.seriesPane = false: hidden from the "
			      "start; J / K still move through the commits)" },
	[ACT_MESSAGE_PANE] = { "message-pane", "gc",
			       "hide or show the message pane",
			       "hide the message pane, or show it again "
			       "(carve.messagePane = false: hidden from the "
			       "start); in it {edit} writes the "
			       "message, or renames the commit shown, and "
			       "{commit} commits with it" },
	[ACT_REPAINT] = { "repaint", "Ctrl-L", "repaint the screen",
			  "repaint the screen and read the status and the diffs "
			  "again" },
	[ACT_LOAD_DIFF] = { "load-diff", "gd",
			    "load a diff that is big or skipped",
			    "load the diff of the file although it is big or "
			    "carve.skipDiff names it (a key stops the loading)" },
	[ACT_IGNORE] = { "ignore", "gi", "never list this untracked file again",
			 "an untracked file: add it to .git/info/exclude, so "
			 "it is never listed again" },
	[ACT_FIXUP_COMMIT] = { "fixup", "f",
			       "a fixup! commit for an older commit",
			       "a fixup! commit of the selected lines (or what "
			       "is staged) for a commit chosen from a list (b: "
			       "the commit that last touched the lines), to be "
			       "squashed later ({squash}); nothing is "
			       "rewritten" },
	[ACT_AMEND_COMMIT] = { "amend-fixup", "F",
			       "an amend! commit, a new message for a commit",
			       "an amend! commit: a new message for a commit "
			       "chosen from a list (in the editor), with what "
			       "is staged; squashed later ({squash})" },
	[ACT_SQUASH] = { "squash", "S",
			 "squash the fixup! commits into their commits",
			 "squash the fixup!, amend! and squash! commits of the "
			 "series into their commits, after asking (the "
			 "commits above them are replayed; {undo} undoes it)" },
	[ACT_PREFIX] = { "", "", "", "" },
};

static const struct {
	const char *name;
	int code;
} named[] = {
	{ "Space", ' ' },
	{ "Tab", '\t' },
	{ "Shift-Tab", KEY_BTAB },
	{ "BTab", KEY_BTAB },
	{ "Esc", 27 },
	{ "Escape", 27 },
	{ "Up", KEY_UP },
	{ "Down", KEY_DOWN },
	{ "Left", KEY_LEFT },
	{ "Right", KEY_RIGHT },
	{ "PgUp", KEY_PPAGE },
	{ "PageUp", KEY_PPAGE },
	{ "PgDn", KEY_NPAGE },
	{ "PageDown", KEY_NPAGE },
	{ "Home", KEY_HOME },
	{ "End", KEY_END },
	{ "Ins", KEY_IC },
	{ "Insert", KEY_IC },
	{ "Del", KEY_DC },
	{ "Delete", KEY_DC },
};

int key_parse(const char *s, int codes[4])
{
	size_t i;
	int n;

	if (!s || !*s)
		return 0;
	if (!s[1] && s[0] > ' ' && s[0] < 0x7f) {
		codes[0] = (unsigned char)s[0];
		return 1;
	}
	if (!strcasecmp(s, "Enter") || !strcasecmp(s, "Return")) {
		codes[0] = '\n';
		codes[1] = '\r';
		codes[2] = KEY_ENTER;
		return 3;
	}
	if (!strcasecmp(s, "Backspace")) {
		codes[0] = KEY_BACKSPACE;
		codes[1] = 127;
		codes[2] = 8;
		return 3;
	}
	for (i = 0; i < ARRAY_SIZE(named); i++) {
		if (!strcasecmp(s, named[i].name)) {
			codes[0] = named[i].code;
			return 1;
		}
	}
	if (!strncasecmp(s, "Ctrl-", 5) && s[5] && !s[6]) {
		char c = s[5];

		if (c >= 'a' && c <= 'z')
			c -= 'a' - 'A';
		if (c >= 'A' && c <= 'Z') {
			codes[0] = c - 'A' + 1;
			return 1;
		}
		return 0;
	}
	if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9' &&
	    !parse_int(s + 1, NULL, &n) && n >= 1 && n <= 12 &&
	    strlen(s) <= 3) {
		codes[0] = KEY_F(n);
		return 1;
	}
	return 0;
}

const char *key_label(int code)
{
	static char buf[16];
	size_t i;

	if (code == '\n' || code == '\r' || code == KEY_ENTER)
		return "Enter";
	if (code == KEY_BACKSPACE || code == 127 || code == 8)
		return "Backspace";
	for (i = 0; i < ARRAY_SIZE(named); i++)
		if (named[i].code == code)
			return named[i].name;
	if (code > ' ' && code < 0x7f) {
		buf[0] = (char)code;
		buf[1] = '\0';
		return buf;
	}
	if (code >= 1 && code <= 26) {
		snprintf(buf, sizeof(buf), "Ctrl-%c", 'A' + code - 1);
		return buf;
	}
	if (code >= KEY_F(1) && code <= KEY_F(12)) {
		snprintf(buf, sizeof(buf), "F%d", code - KEY_F0);
		return buf;
	}
	snprintf(buf, sizeof(buf), "#%d", code);
	return buf;
}

/* "gg": a sequence of two printable keys (tried when not a key name). */
static bool seq_parse(const char *s, int *first, int *second)
{
	if (strlen(s) != 2 || s[0] <= ' ' || s[0] >= 0x7f || s[1] <= ' ' ||
	    s[1] >= 0x7f)
		return false;
	*first = (unsigned char)s[0];
	*second = (unsigned char)s[1];
	return true;
}

/*
 * Forget the sequences that start with first, which becomes a plain key
 * again.
 */
static void drop_prefix(struct keymap *km, int first)
{
	memset(km->seq[first], 0, sizeof(km->seq[first]));
	km->act[first] = ACT_NONE;
}

/*
 * Bind every key in a space separated list. Returns false on a bad name.
 * locked and slocked are the keys and sequences the user bound up to now,
 * which a later binding may not take, or NULL for the defaults.
 */
static bool bind_list(struct keymap *km, enum action a, const char *list,
		      bool *locked, bool *slocked, struct strbuf *warn)
{
	char *copy = xstrdup(list), *tok, *save = NULL;
	bool ok = true;

	for (tok = strtok_r(copy, " \t", &save); tok;
	     tok = strtok_r(NULL, " \t", &save)) {
		int codes[4], n = key_parse(tok, codes), i, s1, s2;

		if (!n && seq_parse(tok, &s1, &s2)) {
			enum action was = km->seq[s1][s2];

			if (locked &&
			    ((locked[s1] && km->act[s1] != ACT_PREFIX) ||
			     (slocked[s1 * KEYSEQ_SIZE + s2] && was != a))) {
				if (warn)
					sb_addf(warn,
						"carve.keys.%s: '%s' is "
						"already bound to %s\n",
						actions[a].name, tok,
						actions[was ? was : km->act[s1]]
							.name);
				ok = false;
				continue;
			}
			/*
			 * a key of its own until now, which starts
			 * sequences from here on
			 */
			km->act[s1] = ACT_PREFIX;
			km->seq[s1][s2] = (unsigned char)a;
			if (slocked)
				slocked[s1 * KEYSEQ_SIZE + s2] = true;
			continue;
		}
		if (!n) {
			if (warn)
				sb_addf(warn,
					"carve.keys.%s: unknown key '%s'\n",
					actions[a].name, tok);
			ok = false;
			continue;
		}
		for (i = 0; i < n; i++) {
			if (locked && locked[codes[i]] &&
			    km->act[codes[i]] != a) {
				if (warn)
					sb_addf(warn,
						"carve.keys.%s: '%s' is "
						"already bound to %s\n",
						actions[a].name, tok,
						actions[km->act[codes[i]]].name);
				ok = false;
				continue;
			}
			if (codes[i] < KEYSEQ_SIZE && slocked &&
			    km->act[codes[i]] == ACT_PREFIX &&
			    memchr(slocked + (size_t)codes[i] * KEYSEQ_SIZE, 1,
				   KEYSEQ_SIZE)) {
				if (warn)
					sb_addf(warn,
						"carve.keys.%s: '%s' starts "
						"a sequence bound in "
						"carve.keys\n",
						actions[a].name, tok);
				ok = false;
				continue;
			}
			/* a key that started sequences, which end here */
			if (codes[i] < KEYSEQ_SIZE &&
			    km->act[codes[i]] == ACT_PREFIX)
				drop_prefix(km, codes[i]);
			km->act[codes[i]] = (unsigned char)a;
			if (locked)
				locked[codes[i]] = true;
		}
	}
	free(copy);
	return ok;
}

void keymap_defaults(struct keymap *km)
{
	int a;

	memset(km, 0, sizeof(*km));
	for (a = 1; a < NACTIONS; a++)
		bind_list(km, (enum action)a, actions[a].keys, NULL, NULL,
			  NULL);
}

static enum action action_by_name(const char *name, size_t len)
{
	int a;

	for (a = 1; a < NACTIONS; a++)
		if (strlen(actions[a].name) == len &&
		    !strncasecmp(actions[a].name, name, len))
			return (enum action)a;
	return ACT_NONE;
}

static void unbind(struct keymap *km, enum action a)
{
	int k, s;

	for (k = 0; k < KEYMAP_SIZE; k++)
		if (km->act[k] == a)
			km->act[k] = ACT_NONE;
	for (k = 0; k < KEYSEQ_SIZE; k++) {
		bool any = false;

		if (km->act[k] != ACT_PREFIX)
			continue;
		for (s = 0; s < KEYSEQ_SIZE; s++) {
			if (km->seq[k][s] == a)
				km->seq[k][s] = ACT_NONE;
			any |= km->seq[k][s] != ACT_NONE;
		}
		/* with no sequence left it is a plain key again */
		if (!any)
			km->act[k] = ACT_NONE;
	}
}

void keymap_apply(struct keymap *km, const char *config, struct strbuf *warn)
{
	static const char prefix[] = "carve.keys.";
	const char *end = config + strlen(config);
	bool slocked[KEYSEQ_SIZE * KEYSEQ_SIZE];
	bool configured[NACTIONS] = { false };
	bool locked[KEYMAP_SIZE] = { false };
	const char *pos = config;
	const char *l;
	size_t len;
	int a;

	memset(slocked, 0, sizeof(slocked));
	while ((l = next_line(&pos, end, &len))) {
		const char *sp = memchr(l, ' ', len);
		struct keymap before = *km;
		enum action act;
		char *value;
		size_t nl;

		nl = sp ? (size_t)(sp - l) : len;

		if (len < sizeof(prefix) - 1 ||
		    strncasecmp(l, prefix, sizeof(prefix) - 1))
			continue;
		act = action_by_name(l + sizeof(prefix) - 1,
				     nl - (sizeof(prefix) - 1));
		if (act == ACT_NONE) {
			sb_addf(warn,
				"unknown action '%.*s' in carve.keys "
				"(see git carve keys)\n",
				(int)(nl - (sizeof(prefix) - 1)),
				l + sizeof(prefix) - 1);
			continue;
		}
		configured[act] = true;
		value = sp ? xstrndup(sp + 1, len - nl - 1) : xstrdup("");
		unbind(km, act);
		if (strcasecmp(value, "none") &&
		    (!*value ||
		     !bind_list(km, act, value, locked, slocked, warn))) {
			if (!*value)
				sb_addf(warn, "carve.keys.%s: no key given\n",
					actions[act].name);
			/* keep the defaults of this action */
			*km = before;
		}
		free(value);
	}
	/* an action that lost all its keys to others is worth a note */
	for (a = 1; a < NACTIONS; a++) {
		bool any = false;
		int k;

		for (k = 0; k < KEYMAP_SIZE && !any; k++)
			any = km->act[k] == a;
		for (k = 0; k < KEYSEQ_SIZE * KEYSEQ_SIZE && !any; k++)
			any = km->seq[k / KEYSEQ_SIZE][k % KEYSEQ_SIZE] == a;
		if (!any && *actions[a].keys && !configured[a])
			sb_addf(warn, "action %s has no key any more\n",
				actions[a].name);
	}
}

void keymap_seq_hint(const struct keymap *km, int first, struct strbuf *out)
{
	int k;

	sb_adds(out, key_label(first));
	sb_adds(out, ":");
	if (first <= 0 || first >= KEYSEQ_SIZE)
		return;
	for (k = 1; k < KEYSEQ_SIZE; k++) {
		enum action a = (enum action)km->seq[first][k];

		if (a != ACT_NONE)
			sb_addf(out, "  %s %s", key_label(k), actions[a].name);
	}
}

void keymap_describe(const struct keymap *km, enum action a, struct strbuf *out)
{
	bool seen_enter = false, seen_bs = false;
	size_t start = out->len;
	int k;

	/* defaults first, in their documented order, then the rest */
	for (k = 0; k < KEYMAP_SIZE; k++) {
		const char *n;

		if (km->act[k] != a)
			continue;
		n = key_label(k);
		if (!strcmp(n, "Enter")) {
			if (seen_enter)
				continue;
			seen_enter = true;
		}
		if (!strcmp(n, "Backspace")) {
			if (seen_bs)
				continue;
			seen_bs = true;
		}
		if (out->len > start)
			sb_adds(out, ", ");
		sb_adds(out, n);
	}
	for (k = 0; k < KEYSEQ_SIZE * KEYSEQ_SIZE; k++) {
		if (km->seq[k / KEYSEQ_SIZE][k % KEYSEQ_SIZE] != a)
			continue;
		if (out->len > start)
			sb_adds(out, ", ");
		sb_addf(out, "%c%c", k / KEYSEQ_SIZE, k % KEYSEQ_SIZE);
	}
	if (out->len == start)
		sb_adds(out, "(none)");
}

int keymap_first(const struct keymap *km, enum action a)
{
	int k;

	for (k = 0; k < KEYMAP_SIZE; k++)
		if (km->act[k] == a)
			return k;
	return -1;
}

const char *keymap_key(const struct keymap *km, enum action a, char *buf,
		       size_t size)
{
	int k = keymap_first(km, a), x, y;

	if (k >= 0) {
		snprintf(buf, size, "%s", key_label(k));
		return buf;
	}
	/* a sequence such as gg, zz or gc */
	for (x = 1; x < KEYSEQ_SIZE; x++) {
		for (y = 1; y < KEYSEQ_SIZE; y++) {
			char first[16];

			if (km->seq[x][y] != a)
				continue;
			snprintf(first, sizeof(first), "%s", key_label(x));
			snprintf(buf, size, "%s%s", first, key_label(y));
			return buf;
		}
	}
	return NULL;
}

void keys_subst(const struct keymap *km, const char *in, struct strbuf *out,
		bool fmt)
{
	const char *p = in;

	while (*p) {
		const char *close = *p == '{' ? strchr(p, '}') : NULL;
		size_t a, n = close ? (size_t)(close - p - 1) : 0;

		for (a = 1; close && a < NACTIONS; a++)
			if (strlen(actions[a].name) == n && n &&
			    !strncmp(p + 1, actions[a].name, n))
				break;
		if (!close || a == NACTIONS) {
			sb_add(out, p, 1);
			p++;
			continue;
		}
		{
			struct strbuf lab = STRBUF_INIT;
			char buf[32];
			const char *k = keymap_key(km, (enum action)a, buf,
						   sizeof(buf));
			size_t i;

			/*
			 * an unbound action shows its name, which tells
			 * how to bind it
			 */
			if (k)
				sb_adds(&lab, k);
			else
				sb_addf(&lab, "<%s>", actions[a].name);
			for (i = 0; i < lab.len; i++) {
				if (fmt && lab.buf[i] == '%')
					sb_addch(out, '%');
				sb_addch(out, lab.buf[i]);
			}
			sb_release(&lab);
		}
		p = close + 1;
	}
}
