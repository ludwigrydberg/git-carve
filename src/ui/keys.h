/* SPDX-License-Identifier: MIT */
/*
 * TUI key bindings, the named actions, their default keys, and user
 * overrides from git config ([carve "keys"] <action> = <key> [<key>...]).
 * There are no curses calls, only its key codes, and this can be unit
 * tested.
 */
#ifndef CARVE_UI_KEYS_H
#define CARVE_UI_KEYS_H

#include <stdbool.h>

#include "../util.h"

enum action {
	ACT_NONE = 0,
	ACT_QUIT,
	ACT_HELP,
	ACT_NEXT_PANE,
	ACT_PREV_PANE,
	ACT_DOWN,
	ACT_UP,
	ACT_PAGE_DOWN,
	ACT_PAGE_UP,
	ACT_TOP,
	ACT_BOTTOM,
	ACT_SCROLL_LEFT,
	ACT_SCROLL_RIGHT,
	ACT_HALF_DOWN,
	ACT_HALF_UP,
	ACT_LINE_DOWN,
	ACT_LINE_UP,
	ACT_SCROLL_CENTER,
	ACT_SCROLL_TOP,
	ACT_SCROLL_BOTTOM,
	ACT_SCREEN_TOP,
	ACT_SCREEN_MIDDLE,
	ACT_SCREEN_BOTTOM,
	ACT_LINE_START,
	ACT_LINE_END,
	ACT_NEXT_HUNK,
	ACT_PREV_HUNK,
	ACT_NEXT_CHANGE,
	ACT_PREV_CHANGE,
	ACT_NEXT_FILE,
	ACT_PREV_FILE,
	ACT_MORE_CONTEXT,
	ACT_LESS_CONTEXT,
	ACT_FUNC_CONTEXT,
	ACT_IGNORE_WS,
	ACT_GOTO,
	ACT_SEARCH,
	ACT_SEARCH_NEXT,
	ACT_SEARCH_PREV,
	ACT_SEARCH_LIST,
	ACT_STAGE,
	ACT_STAGE_FILE,
	ACT_RANGE,
	ACT_MARK,
	ACT_SELECT_HUNK,
	ACT_CLEAR,
	ACT_EDIT,
	ACT_DISCARD,
	ACT_OURS,
	ACT_THEIRS,
	ACT_COMMIT,
	ACT_AMEND,
	ACT_SIGNOFF,
	ACT_SPLIT,
	ACT_FIXUP,
	ACT_ABSORB,
	ACT_NEXT_COMMIT,
	ACT_PREV_COMMIT,
	ACT_SHOW_COMMIT,
	ACT_FINISH,
	ACT_MARK_PAIR,
	ACT_REMOVE,
	ACT_ABORT,
	ACT_UNDO,
	ACT_REFRESH,
	ACT_ZOOM,
	ACT_SERIES_PANE,
	ACT_MESSAGE_PANE,
	ACT_REPAINT,
	ACT_LOAD_DIFF,
	ACT_IGNORE,
	ACT_REWORD,
	ACT_FIXUP_COMMIT,
	ACT_AMEND_COMMIT,
	ACT_SQUASH,
	ACT_RESIZE,
	/* internal, the first key of a sequence such as gg, not an action */
	ACT_PREFIX,
	NACTIONS
};

struct action_info {
	const char *name; /* for git config, carve.keys.<name> */
	const char *keys; /* default keys, space separated */
	const char *help;
};

extern const struct action_info actions[NACTIONS];

/* Key codes cover ASCII and curses KEY_* values. */
#define KEYMAP_SIZE 1024

/* Sequences are two printable ASCII keys (gg, zz): seq[first][second]. */
#define KEYSEQ_SIZE 128

struct keymap {
	unsigned char act[KEYMAP_SIZE]; /* ACT_PREFIX starts sequences */
	unsigned char seq[KEYSEQ_SIZE][KEYSEQ_SIZE];
};

/*
 * Parse "x", "Space", "Ctrl-X", "F5", "PgDn", ... into codes (Enter maps
 * to several). Returns the number of codes, 0 if unknown.
 */
int key_parse(const char *s, int codes[4]);
/* Canonical name of a code (static buffer). */
const char *key_label(int code);

void keymap_defaults(struct keymap *km);
/*
 * Apply user bindings given as "carve.keys.<action> <keys>" lines, the
 * output of git config --get-regexp. A configured action loses its default
 * keys, and "none" leaves it unbound. Problems are added to warn, one line
 * each, and the default is kept.
 */
void keymap_apply(struct keymap *km, const char *config, struct strbuf *warn);
/* Keys bound to an action, e.g. "j, Down", or "(none)". */
void keymap_describe(const struct keymap *km, enum action a,
		     struct strbuf *out);

/*
 * What can follow the prefix key first, as "g: a absorb  c message-pane
 * ...", for the status line while the second key is awaited.
 */
void keymap_seq_hint(const struct keymap *km, int first, struct strbuf *out);

/* Lowest key code bound to an action (its "main" key), -1 if none. */
int keymap_first(const struct keymap *km, enum action a);
/*
 * The main key of action a as written ("s", "gc", "Ctrl-S") in buf, or
 * NULL when nothing is bound to it.
 */
const char *keymap_key(const struct keymap *km, enum action a, char *buf,
		       size_t size);
/*
 * in with every "{action-name}" replaced by the key bound to it, or
 * "<action-name>" when none is. Messages then name keys as the user bound
 * them. With fmt the result is a printf format, with "%" in a key doubled.
 */
void keys_subst(const struct keymap *km, const char *in, struct strbuf *out,
		bool fmt);

static inline enum action key_action(const struct keymap *km, int code)
{
	return code >= 0 && code < KEYMAP_SIZE ? (enum action)km->act[code] :
						 ACT_NONE;
}

/*
 * The action of the sequence first, second, where first is an ACT_PREFIX
 * key.
 */
static inline enum action key_seq_action(const struct keymap *km, int first,
					 int second)
{
	if (first <= 0 || first >= KEYSEQ_SIZE || second <= 0 ||
	    second >= KEYSEQ_SIZE)
		return ACT_NONE;
	/*
	 * Both indexes are checked above. clang-tidy 22 still reports the
	 * keys read from the terminal as an out of bounds access here.
	 */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	return (enum action)km->seq[first][second];
}

#endif
