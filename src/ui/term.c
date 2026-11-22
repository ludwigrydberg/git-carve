// SPDX-License-Identifier: MIT
/* Terminal handling, prompts and overlays. */
#include "term.h"
#include "ui_int.h"
#include "../undo.h"
#include "actions.h"
#include "draw.h"
#include "state.h"
#include "../absorb.h"
#include "../rebase.h"

static void draw_painted(int y, int x, int maxw, char *const *lines, size_t n,
			 size_t k, line_paint_fn paint);

/* ---- prompts and helpers ---------------------------------------------- */

/* Read a line at the bottom of the screen. Returns false on Esc. */
bool prompt(const char *question, char *buf, size_t sz)
{
	buf[0] = '\0';
	return prompt_live(question, buf, sz, NULL);
}

/*
 * A line read at the bottom, starting from what buf holds. changed(buf) is
 * called on every change and the screen is drawn again under the line, for
 * a filter as it is typed.
 */
bool prompt_live(const char *question, char *buf, size_t sz,
		 void (*changed)(const char *text))
{
	struct strbuf q = STRBUF_INIT;
	size_t n = strlen(buf);
	const char *label;
	bool ok = false;
	int c;

	keys_subst(&ui.keys, question, &q, false);
	label = sb_str(&q);
	for (;;) {
		if (changed)
			draw();
		move(LINES - 1, 0);
		clrtoeol();
		puts_at(LINES - 1, 0, COLS, label);
		puts_at(LINES - 1, (int)strlen(label), COLS, buf);
		curs_set(1);
		refresh();
		c = user_key();
		curs_set(0);
		if (c == '\n' || c == '\r' || c == KEY_ENTER) {
			ok = true;
			break;
		}
		if (c == 27)
			break;
		if (c == KEY_RESIZE)
			continue;
		if (c == KEY_BACKSPACE || c == 127 || c == 8) {
			if (!n)
				break;
			buf[--n] = '\0';
			if (changed)
				changed(buf);
			continue;
		}
		if (c >= 0x20 && c < 0x100 && n + 1 < sz) {
			buf[n++] = (char)c;
			buf[n] = '\0';
			if (changed)
				changed(buf);
		}
	}
	sb_release(&q);
	return ok;
}

bool confirm(const char *question)
{
	struct strbuf q = STRBUF_INIT;
	int c;

	keys_subst(&ui.keys, question, &q, false);
	move(LINES - 1, 0);
	clrtoeol();
	attron(A_BOLD);
	puts_at(LINES - 1, 0, COLS, sb_str(&q));
	sb_release(&q);
	attroff(A_BOLD);
	refresh();
	c = user_key();
	msg_clear();
	return c == 'y' || c == 'Y';
}

/* Show the first pending warning (e.g. "already pushed"). */
void show_warnings(void)
{
	struct strbuf *w = &ui.r->warn;
	struct strbuf t = STRBUF_INIT;

	if (!w->len)
		return;
	keys_subst(&ui.keys, w->buf, &t, false);
	set_warn("%s", t.buf);
	sb_release(&t);
	sb_reset(w);
}

/* Adopt the real terminal size if it differs from what curses thinks. */
static void sync_size(void)
{
	struct winsize ws;

	if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_col &&
	    (ws.ws_row != LINES || ws.ws_col != COLS))
		resizeterm(ws.ws_row, ws.ws_col);
}

/*
 * Ctrl-Z is handled here rather than by ncurses. Its handler restarts the
 * blocked read, and after fg we could not check the terminal size again,
 * since a stopped process gets no SIGWINCH. Ours only sets a flag and,
 * without SA_RESTART, makes getch() return. next_key() then suspends
 * properly.
 */
volatile sig_atomic_t want_stop;

static void on_sigtstp(int sig)
{
	(void)sig;
	want_stop = 1;
}

void set_tstp(bool ours)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = ours ? on_sigtstp : SIG_DFL;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTSTP, &sa, NULL);
}

/* Leave curses for a child that needs the terminal. */
void suspend_curses(void)
{
	/* the shell or the child may look at the undo history */
	if (flush_undo())
		repo_warn(ui.r, UNDO_FAILED_MSG, sb_str(&ui.r->err));
	set_tstp(false); /* Ctrl-Z in the child stops us all */
	def_prog_mode();
	endwin();
}

void resume_curses(void)
{
	reset_prog_mode();
	set_tstp(true);
	/* the terminal may have been resized while the child had it */
	sync_size();
	keypad(stdscr, TRUE);
	curs_set(0);
	clearok(stdscr, TRUE);
	refresh();
}

/* The action of key c in an overlay. A prefix (g of gg) reads its sequence. */
static enum action overlay_action(int c)
{
	enum action act = key_action(&ui.keys, c);

	if (act == ACT_PREFIX)
		act = key_seq_action(&ui.keys, c, user_key());
	return act;
}

enum ov_mode { OV_PICK, OV_VIEW, OV_CONFIRM };

/*
 * A key of a picker's own, such as b in the commit picker. fn gets the
 * cursor and returns the new one, or -1 when it stays, with a line for the
 * footer in note.
 */
struct ov_hook {
	int key;
	const char *foot; /* what the footer says it does */
	long (*fn)(void *ctx, size_t cur, char *note, size_t sz);
	void *ctx;
};

static long list_overlay(const char *title, char *const *lines, size_t n,
			 size_t cur, enum ov_mode mode, const char *verb,
			 line_paint_fn paint, const struct ov_hook *hook);

long pick_list(const char *title, char *const *lines, size_t n, size_t cur,
	       line_paint_fn paint)
{
	return list_overlay(title, lines, n, cur, OV_PICK, NULL, paint, NULL);
}

/*
 * Text to read, each line painted by paint or plain for NULL. The movement
 * keys scroll and any other key closes.
 */
void view_lines_paint(const char *title, char *const *lines, size_t n,
		      line_paint_fn paint)
{
	list_overlay(title, lines, n, 0, OV_VIEW, NULL, paint, NULL);
}

/*
 * A question over lines to read. Enter answers yes, as the footer says
 * with "Enter: verb", and Esc no. The movement keys scroll.
 */
bool confirm_list(const char *title, char *const *lines, size_t n,
		  const char *verb, line_paint_fn paint)
{
	return list_overlay(title, lines, n, 0, OV_CONFIRM, verb, paint,
			    NULL) == 0;
}

/* The frame of an overlay (the help's): lines, or ASCII where there are none. */
void draw_frame(int y0, int x0, int h, int w)
{
	int i;

	mvaddch(y0, x0, line_ch(ACS_ULCORNER, '+'));
	mvaddch(y0, x0 + w - 1, line_ch(ACS_URCORNER, '+'));
	mvaddch(y0 + h - 1, x0, line_ch(ACS_LLCORNER, '+'));
	mvaddch(y0 + h - 1, x0 + w - 1, line_ch(ACS_LRCORNER, '+'));
	for (i = 1; i < w - 1; i++) {
		mvaddch(y0, x0 + i, line_ch(ACS_HLINE, '-'));
		mvaddch(y0 + h - 1, x0 + i, line_ch(ACS_HLINE, '-'));
	}
	for (i = 1; i < h - 1; i++) {
		mvaddch(y0 + i, x0, line_ch(ACS_VLINE, '|'));
		mvaddch(y0 + i, x0 + w - 1, line_ch(ACS_VLINE, '|'));
	}
}

/*
 * A list overlay, moved through with the movement keys, where Enter
 * chooses. Returns the chosen index, or -1 when cancelled (Esc, quit) or
 * the screen is too small.
 */
static long list_overlay(const char *title, char *const *lines, size_t n,
			 size_t cur, enum ov_mode mode, const char *verb,
			 line_paint_fn paint, const struct ov_hook *hook)
{
	bool viewer = mode != OV_PICK; /* without a cursor the lines scroll */
	char foot[200], note[160] = "", zoom_key[32];
	int h, w, y0, x0, c;
	size_t top = 0, i;

	if (!n)
		return -1;
	{
		struct strbuf zk = STRBUF_INIT;

		/* the zoom key as bound ("o") */
		keys_subst(&ui.keys, "{zoom}", &zk, false);
		snprintf(zoom_key, sizeof(zoom_key), "%s", sb_str(&zk));
		sb_release(&zk);
	}
	if (cur >= n)
		cur = n - 1;
	for (;;) {
		enum action act;
		int rows;

		/* frame, title, the lines, footer, frame */
		w = COLS - 8 > 100 ? 100 : COLS - 8;
		h = LINES - 4;
		/* o takes the whole screen, for long lines and long commits */
		if (ui.lay.ov_zoom) {
			w = COLS;
			h = LINES;
		}
		if ((int)n + 4 < h)
			h = (int)n + 4;
		if (w < 20 || h < 5)
			return -1;
		rows = h - 4;
		y0 = (LINES - h) / 2;
		x0 = (COLS - w) / 2;
		/* a viewer has no cursor, and cur is the first row shown */
		if (viewer) {
			if (n > (size_t)rows && cur > n - (size_t)rows)
				cur = n - (size_t)rows;
			if (n <= (size_t)rows)
				cur = 0;
			top = cur;
		}
		if (cur < top)
			top = cur;
		if (cur >= top + (size_t)rows)
			top = cur - (size_t)rows + 1;
		draw_under();
		for (i = 0; i < (size_t)h; i++)
			fill_line(y0 + (int)i, x0, w);
		attron(A_BOLD);
		puts_keys(y0 + 1, x0 + 2, w - 4, title);
		attroff(A_BOLD);
		for (i = 0; i < (size_t)rows; i++) {
			int y = y0 + 2 + (int)i;
			size_t k = top + i;

			if (k >= n)
				continue;
			if (k == cur && !viewer) {
				attron(A_REVERSE);
				fill_line(y, x0 + 1, w - 2);
			}
			mvaddch(y, x0 + 1, k == cur && !viewer ? '>' : ' ');
			draw_painted(y, x0 + 2, w - 4, lines, n, k, paint);
			attroff(A_REVERSE);
		}
		attron(A_BOLD);
		if (mode == OV_CONFIRM && n > (size_t)rows)
			snprintf(foot, sizeof(foot),
				 "Enter: %s  Esc: cancel  "
				 "up/down: scroll (lines %zu-%zu of %zu)",
				 verb, top + 1, top + (size_t)rows, n);
		else if (mode == OV_CONFIRM)
			snprintf(foot, sizeof(foot), "Enter: %s  Esc: cancel",
				 verb);
		else if (mode == OV_VIEW && n > (size_t)rows)
			snprintf(foot, sizeof(foot),
				 "PgUp/PgDn: scroll (lines "
				 "%zu-%zu of %zu)  %s: %s  other keys: close",
				 top + 1, top + (size_t)rows, n, zoom_key,
				 ui.lay.ov_zoom ? "smaller" : "full screen");
		else if (mode == OV_VIEW)
			snprintf(foot, sizeof(foot),
				 "%s: %s  any other key: close", zoom_key,
				 ui.lay.ov_zoom ? "smaller" : "full screen");
		else if (*note)
			snprintf(foot, sizeof(foot), "%s", note);
		else
			snprintf(foot, sizeof(foot),
				 "move: up/down keys  Enter: "
				 "choose  %s%sEsc: cancel",
				 hook ? hook->foot : "", hook ? "  " : "");
		puts_at(y0 + h - 2, x0 + 2, w - 4, foot);
		draw_frame(y0, x0, h, w);
		attroff(A_BOLD);
		refresh();
		c = user_key();
		if (c == KEY_RESIZE)
			continue;
		act = overlay_action(c);
		*note = '\0';
		if (act == ACT_ZOOM) {
			ui.lay.ov_zoom = !ui.lay.ov_zoom;
		} else if (act == ACT_DOWN || c == KEY_DOWN) {
			if (cur + 1 < n)
				cur++;
		} else if (act == ACT_UP || c == KEY_UP) {
			if (cur)
				cur--;
		} else if (act == ACT_PAGE_DOWN || c == KEY_NPAGE) {
			cur = cur + (size_t)rows < n ? cur + (size_t)rows :
						       n - 1;
		} else if (act == ACT_PAGE_UP || c == KEY_PPAGE) {
			cur = cur > (size_t)rows ? cur - (size_t)rows : 0;
		} else if (act == ACT_TOP) {
			cur = 0;
		} else if (act == ACT_BOTTOM) {
			cur = n - 1;
		} else if (mode == OV_VIEW || c == 27 || act == ACT_QUIT) {
			/* a text closes on any other key, a list on Esc */
			return -1;
		} else if (hook && c == hook->key) {
			long k = hook->fn(hook->ctx, cur, note, sizeof(note));

			if (k >= 0 && (size_t)k < n)
				cur = (size_t)k;
		} else if (c == '\n' || c == '\r' || c == KEY_ENTER) {
			return mode == OV_CONFIRM ? 0 : (long)cur;
		}
	}
}

struct blame_pick {
	char **ids;
	size_t n;
};

/*
 * b moves the cursor to the commit of the list that last touched most of
 * the lines of the change (blame_suggest), and puts what it found in the
 * footer.
 */
static long blame_key(void *ctx, size_t cur, char *note, size_t sz)
{
	const struct blame_pick *bp = ctx;
	struct blame_count *v;
	size_t n, i, k;
	long res = -1;
	int total;

	if (blame_suggest(&v, &n, &total)) {
		struct strbuf t = STRBUF_INIT;

		keys_subst(&ui.keys,
			   ui.r->err.len ? ui.r->err.buf : "git blame failed",
			   &t, false);
		snprintf(note, sz, "b: %s", t.buf);
		sb_release(&t);
		sb_reset(&ui.r->err);
		return -1;
	}
	for (i = 0; i < n && res < 0; i++)
		for (k = 0; k < bp->n; k++)
			if (!strcmp(bp->ids[k], v[i].id)) {
				res = (long)k;
				break;
			}
	if (res < 0)
		snprintf(note, sz,
			 "b: the lines come from %.7s, not in this "
			 "list",
			 n ? v[0].id : "?");
	else if (n == 1)
		snprintf(note, sz, "b: %s from this commit",
			 total == 1 ? "the line comes" : "the lines come");
	else
		snprintf(note, sz,
			 "b: %d of %d lines from this commit (they "
			 "come from %zu commits)",
			 v[i - 1].n, total, n);
	blame_counts_free(v, n);
	return res;
}

/*
 * A commit of rev's history chosen from a list, the cursor on sel (a full
 * id) when it is listed. With blame, b finds the commit of the lines of
 * the change (blame_suggest). Returns its id, or NULL on Esc.
 */
char *pick_commit_sel(const char *title, const char *rev, const char *sel,
		      bool blame)
{
	char **ids = NULL, **lines = NULL, *res = NULL;
	size_t n = 0, alloc = 0, alloc2 = 0, len, i;
	struct strbuf out = STRBUF_INIT;
	const char *pos, *end, *l;
	struct blame_pick bp;
	struct ov_hook hook = { 'b', "b: blame", blame_key, &bp };
	long k;

	if (repo_recent_commits(ui.r, rev, SERIES_MAX, &out) || !out.len) {
		msg_repo_err();
		sb_release(&out);
		return NULL;
	}
	pos = sb_str(&out);
	end = sb_str(&out) + out.len;
	while ((l = next_line(&pos, end, &len))) {
		const char *sp = memchr(l, ' ', len);

		if (!sp)
			continue;
		ALLOC_GROW(ids, n + 1, alloc);
		ALLOC_GROW(lines, n + 1, alloc2);
		ids[n] = xstrndup(l, (size_t)(sp - l));
		lines[n] = xstrndup(sp + 1, len - (size_t)(sp + 1 - l));
		n++;
	}
	sb_release(&out);
	for (i = 0, k = 0; sel && i < n; i++)
		if (!strcmp(ids[i], sel))
			k = (long)i;
	bp.ids = ids;
	bp.n = n;
	k = list_overlay(title, lines, n, (size_t)k, OV_PICK, NULL, NULL,
			 blame ? &hook : NULL);
	if (k >= 0 && (size_t)k < n)
		res = xstrdup(ids[k]);
	for (i = 0; i < n; i++) {
		free(ids[i]);
		free(lines[i]);
	}
	free(ids);
	free(lines);
	return res;
}

/* Where the help of a pane lists an action. */
enum {
	H_FILES = 1 << 0,
	H_DIFF = 1 << 1,
	H_SERIES = 1 << 2,
	H_MSG = 1 << 3,
	H_ANY = H_FILES | H_DIFF | H_SERIES | H_MSG,
	H_OP = 1 << 4,	     /* only while a split or a move is in progress */
	H_CONFLICT = 1 << 5, /* only while a file has conflicts */
};

struct help_item {
	enum action a;
	unsigned where; /* 0 for the list of all keys only */
};

/*
 * The sections of the help, each action once, and the rest goes to Other.
 * The actions that change something come first, moving around last.
 */
static const struct {
	const char *title;
	struct help_item items[24];
} help_sections[] = {
	{ "Staging and editing",
	  { { ACT_STAGE, H_FILES | H_DIFF },
	    { ACT_STAGE_FILE, H_FILES },
	    { ACT_EDIT, H_ANY },
	    { ACT_DISCARD, H_FILES | H_DIFF },
	    { ACT_REMOVE, H_FILES },
	    { ACT_IGNORE, H_FILES },
	    { ACT_OURS, H_FILES | H_DIFF | H_CONFLICT },
	    { ACT_THEIRS, H_FILES | H_DIFF | H_CONFLICT },
	    { ACT_UNDO, H_ANY } } },
	{ "Selecting lines",
	  { { ACT_RANGE, H_FILES | H_DIFF },
	    { ACT_MARK, H_DIFF },
	    { ACT_MARK_PAIR, H_DIFF },
	    { ACT_SELECT_HUNK, H_DIFF },
	    { ACT_CLEAR, H_FILES | H_DIFF } } },
	{ "Committing",
	  { { ACT_COMMIT, H_FILES | H_DIFF | H_MSG },
	    { ACT_AMEND, H_FILES | H_DIFF | H_MSG },
	    { ACT_FIXUP_COMMIT, H_FILES | H_DIFF | H_SERIES },
	    { ACT_AMEND_COMMIT, H_FILES | H_DIFF | H_SERIES },
	    { ACT_SQUASH, H_FILES | H_SERIES },
	    { ACT_REWORD, H_SERIES | H_MSG },
	    { ACT_SIGNOFF, 0 },
	    { ACT_MESSAGE_PANE, H_MSG } } },
	{ "Splitting and moving changes",
	  { { ACT_SPLIT, H_FILES | H_SERIES },
	    { ACT_FIXUP, H_FILES | H_DIFF },
	    { ACT_ABSORB, H_FILES | H_DIFF },
	    { ACT_FINISH, H_ANY | H_OP },
	    { ACT_ABORT, H_ANY | H_OP } } },
	{ "The Series pane",
	  { { ACT_NEXT_COMMIT, H_ANY },
	    { ACT_PREV_COMMIT, H_ANY },
	    { ACT_SERIES_PANE, H_SERIES } } },
	{ "Going through the changes",
	  { { ACT_NEXT_HUNK, H_DIFF },
	    { ACT_PREV_HUNK, H_DIFF },
	    { ACT_NEXT_CHANGE, H_DIFF },
	    { ACT_PREV_CHANGE, H_DIFF },
	    { ACT_NEXT_FILE, H_DIFF },
	    { ACT_PREV_FILE, H_DIFF },
	    { ACT_SEARCH, H_FILES | H_DIFF },
	    { ACT_SEARCH_NEXT, H_DIFF },
	    { ACT_SEARCH_PREV, H_DIFF },
	    { ACT_SEARCH_LIST, H_DIFF } } },
	{ "Panes and the view",
	  { { ACT_SHOW_COMMIT, H_ANY },
	    { ACT_NEXT_PANE, H_ANY },
	    { ACT_PREV_PANE, H_ANY },
	    { ACT_ZOOM, H_ANY },
	    { ACT_RESIZE, 0 },
	    { ACT_MORE_CONTEXT, H_DIFF },
	    { ACT_LESS_CONTEXT, H_DIFF },
	    { ACT_FUNC_CONTEXT, H_DIFF },
	    { ACT_IGNORE_WS, H_DIFF },
	    { ACT_LOAD_DIFF, H_DIFF },
	    { ACT_REFRESH, H_ANY },
	    { ACT_REPAINT, 0 } } },
	{ "Moving",
	  { { ACT_DOWN, H_ANY },
	    { ACT_UP, H_ANY },
	    { ACT_PAGE_DOWN, H_ANY },
	    { ACT_PAGE_UP, H_ANY },
	    { ACT_HALF_DOWN, 0 },
	    { ACT_HALF_UP, 0 },
	    { ACT_TOP, H_ANY },
	    { ACT_BOTTOM, H_ANY },
	    { ACT_SCREEN_TOP, 0 },
	    { ACT_SCREEN_MIDDLE, 0 },
	    { ACT_SCREEN_BOTTOM, 0 },
	    { ACT_LINE_DOWN, 0 },
	    { ACT_LINE_UP, 0 },
	    { ACT_SCROLL_CENTER, 0 },
	    { ACT_SCROLL_TOP, 0 },
	    { ACT_SCROLL_BOTTOM, 0 },
	    { ACT_SCROLL_LEFT, H_FILES | H_DIFF | H_SERIES },
	    { ACT_SCROLL_RIGHT, H_FILES | H_DIFF | H_SERIES },
	    { ACT_LINE_START, 0 },
	    { ACT_LINE_END, 0 },
	    { ACT_GOTO, H_DIFF } } },
	{ "General", { { ACT_HELP, H_ANY }, { ACT_QUIT, H_ANY } } },
};

/* A row of the help, with the action Enter runs on it (or ACT_NONE). */
struct help_row {
	char *text;
	enum action act;
	bool title;
};

struct help_list {
	struct help_row *v;
	size_t n, alloc;
};

static void help_add(struct help_list *l, char *text, enum action act,
		     bool title)
{
	ALLOC_GROW(l->v, l->n + 1, l->alloc);
	l->v[l->n++] = (struct help_row){ text, act, title };
}

static void help_free(struct help_list *l)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		free(l->v[i].text);
	free(l->v);
	memset(l, 0, sizeof(*l));
}

static bool any_conflict(void)
{
	size_t i;

	for (i = 0; i < ui.nfs; i++)
		if (ui.fs[i].unmerged)
			return true;
	return false;
}

/* Whether the help of the active pane lists an action used there. */
static bool help_here(unsigned where)
{
	unsigned pane = ui.active == P_FILES   ? H_FILES :
			ui.active == P_SERIES  ? H_SERIES :
			ui.active == P_MESSAGE ? H_MSG :
						 H_DIFF;

	if ((where & H_OP) && !ui.split.active && ui.series.op == OP_NONE)
		return false;
	if ((where & H_CONFLICT) && !any_conflict())
		return false;
	return where & pane;
}

/* Whether an action's keys, name or short description have the filter. */
static bool help_matches(enum action a, const char *filter)
{
	struct strbuf t = STRBUF_INIT;
	bool yes;

	keymap_describe(&ui.keys, a, &t);
	sb_addf(&t, " %s %s", actions[a].name, actions[a].brief);
	yes = smart_match(t.buf, t.len, filter);
	sb_release(&t);
	return yes;
}

/*
 * One action, its keys, then its description. The short one takes one
 * line, and the long one is wrapped at 52 columns.
 */
static void help_action(struct help_list *l, enum action a, bool details)
{
	struct strbuf keys = STRBUF_INIT, line = STRBUF_INIT,
		      help = STRBUF_INIT;
	bool first = true;
	const char *d;

	keymap_describe(&ui.keys, a, &keys);
	if (!details) {
		sb_addf(&line, "  %-14s %s", keys.buf, actions[a].brief);
		help_add(l, sb_detach(&line), a, false);
		sb_release(&keys);
		return;
	}
	/* other keys a description names, as bound */
	keys_subst(&ui.keys, actions[a].help, &help, false);
	d = sb_str(&help);
	while (*d || first) {
		size_t take = strlen(d);

		if (take > 52) {
			take = 52;
			while (take && d[take] != ' ')
				take--;
			if (!take)
				take = 52;
		}
		sb_reset(&line);
		sb_addf(&line, "  %-18s %.*s", first ? keys.buf : "", (int)take,
			d);
		help_add(l, sb_detach(&line), first ? a : ACT_NONE, false);
		d += take;
		while (*d == ' ')
			d++;
		first = false;
	}
	sb_release(&keys);
	sb_release(&line);
	sb_release(&help);
}

/*
 * The rows of the help, built from the key map to show the keys in
 * effect. With a filter they are the actions that match it, with all the
 * actions with their long descriptions, and else the actions of the
 * active pane. A section's title is a row of its own.
 */
static void help_build(struct help_list *l, bool all, const char *filter)
{
	bool listed[NACTIONS] = { false };
	size_t s, i;
	int a;

	if (all)
		help_add(
			l,
			xstrdup("Bind keys with git config carve.keys.<action>; "
				"git carve keys lists them."),
			ACT_NONE, false);
	for (s = 0; s < ARRAY_SIZE(help_sections); s++) {
		bool first = true;

		for (i = 0; i < ARRAY_SIZE(help_sections[s].items); i++) {
			const struct help_item *it = &help_sections[s].items[i];

			if (!it->a)
				break;
			listed[it->a] = true;
			if (*filter ? !help_matches(it->a, filter) :
			    !all    ? !help_here(it->where) :
				      false)
				continue;
			if (first) {
				if (l->n)
					help_add(l, xstrdup(""), ACT_NONE,
						 false);
				help_add(l, xstrdup(help_sections[s].title),
					 ACT_NONE, true);
				first = false;
			}
			help_action(l, it->a, all);
		}
	}
	for (a = 1; a < NACTIONS; a++) {
		if (listed[a] || !*actions[a].name || !(all || *filter))
			continue; /* shown, ACT_PREFIX, or not in a pane's help */
		if (*filter && !help_matches((enum action)a, filter))
			continue;
		help_add(l, xstrdup(""), ACT_NONE, false);
		help_add(l, xstrdup("Other"), ACT_NONE, true);
		help_action(l, (enum action)a, all);
	}
	if (all && !*filter) {
		struct strbuf t = STRBUF_INIT;

		help_add(l, xstrdup(""), ACT_NONE, false);
		help_add(l, xstrdup("After a conflict in fixup-to"), ACT_NONE,
			 true);
		keys_subst(&ui.keys,
			   "  resolve ({edit}), stage the file, "
			   "then {finish} continues and",
			   &t, false);
		help_add(l, sb_detach(&t), ACT_NONE, false);
		keys_subst(&ui.keys, "  {abort} gives up.", &t, false);
		help_add(l, sb_detach(&t), ACT_NONE, false);
	}
}

/* The next row from k in direction dir that runs an action, or k. */
static size_t help_step(const struct help_list *l, size_t k, int dir)
{
	size_t i = k;

	while (dir > 0 ? i + 1 < l->n : i > 0) {
		i = dir > 0 ? i + 1 : i - 1;
		if (l->v[i].act)
			return i;
	}
	return k;
}

static const char *help_pane_name(void)
{
	switch (ui.active) {
	case P_FILES:
		return "Keys in the Files pane";
	case P_SERIES:
		return "Keys in the Series pane";
	case P_MESSAGE:
		return "Keys in the message pane";
	default:
		return "Keys in the diff";
	}
}

/* The help as it is shown. */
struct help_view {
	struct help_list l;
	char filter[64];
	bool all;     /* all keys with their long descriptions */
	bool typing;  /* the filter is being typed */
	bool rebuild; /* the rows change with the filter or all */
	size_t cur, top;
	int rows; /* rows shown, set when drawn */
};

static void help_rebuild(struct help_view *v)
{
	help_free(&v->l);
	help_build(&v->l, v->all, v->filter);
	v->cur = v->l.n && v->l.v[0].act ? 0 : help_step(&v->l, 0, 1);
	v->top = 0;
	v->rebuild = false;
}

static void help_footer(const struct help_view *v, struct strbuf *foot)
{
	size_t n = v->l.n, rows = (size_t)v->rows;

	if (v->typing) {
		sb_addf(foot, "/%s_  Enter: run  Esc: stop searching",
			v->filter);
	} else {
		keys_subst(&ui.keys,
			   "Enter: run  {search}: search  {help}: ", foot,
			   false);
		sb_addf(foot, "%s  Esc: close",
			v->all ? "this pane" : "all keys");
	}
	if (n > rows)
		sb_addf(foot, "  (%zu-%zu of %zu)", v->top + 1,
			v->top + rows < n ? v->top + rows : n, n);
}

/* Draw the help over the panes. Returns false when the screen is too small. */
static bool help_draw(struct help_view *v)
{
	int w = COLS < 80 ? COLS : 80, h, y0, x0, i;
	const struct help_list *l = &v->l;
	struct strbuf foot = STRBUF_INIT;
	char head[128];

	h = LINES < (int)l->n + 4 ? LINES : (int)l->n + 4;
	if (h < 6)
		h = LINES < 6 ? LINES : 6;
	v->rows = h - 4;
	if (v->rows < 1 || w < 20)
		return false;
	/* the last page is full, and the cursor in view */
	if (v->top + (size_t)v->rows > l->n)
		v->top = l->n > (size_t)v->rows ? l->n - (size_t)v->rows : 0;
	if (v->cur < v->top)
		v->top = v->cur;
	if (v->cur >= v->top + (size_t)v->rows)
		v->top = v->cur - (size_t)v->rows + 1;
	draw_under();
	y0 = (LINES - h) / 2;
	x0 = (COLS - w) / 2;
	for (i = 0; i < h; i++)
		fill_line(y0 + i, x0, w);
	if (*v->filter)
		snprintf(head, sizeof(head), "Keys that match /%s", v->filter);
	else
		snprintf(head, sizeof(head), "%s",
			 v->all ? "All keys" : help_pane_name());
	attron(A_BOLD);
	puts_at(y0 + 1, x0 + 2, w - 4, head);
	attroff(A_BOLD);
	for (i = 0; i < v->rows && v->top + (size_t)i < l->n; i++) {
		size_t k = v->top + (size_t)i;
		int y = y0 + 2 + i;

		if (k == v->cur && l->v[k].act) {
			attron(A_REVERSE);
			fill_line(y, x0 + 1, w - 2);
			mvaddch(y, x0 + 1, '>');
		}
		if (l->v[k].title)
			attron(A_BOLD);
		puts_at(y, x0 + 2, w - 4, l->v[k].text);
		attroff(A_BOLD | A_REVERSE);
	}
	if (!l->n)
		puts_at(y0 + 2, x0 + 2, w - 4, "  No key matches.");
	help_footer(v, &foot);
	attron(A_BOLD);
	puts_at(y0 + h - 2, x0 + 2, w - 4, foot.buf);
	draw_frame(y0, x0, h, w);
	attroff(A_BOLD);
	refresh();
	sb_release(&foot);
	return true;
}

/* A key while the filter is typed. */
static void help_type(struct help_view *v, int c)
{
	size_t len = strlen(v->filter);

	if (c == 27) {
		v->typing = false;
		v->filter[0] = '\0';
	} else if (c == KEY_BACKSPACE || c == 127 || c == 8) {
		if (len)
			v->filter[len - 1] = '\0';
		else
			v->typing = false;
	} else if (c >= 0x20 && c < 0x7f && len + 1 < sizeof(v->filter)) {
		v->filter[len] = (char)c;
		v->filter[len + 1] = '\0';
	} else {
		return;
	}
	v->rebuild = true;
}

/* A movement key. Returns false when c is none. */
static bool help_move(struct help_view *v, int c, enum action act)
{
	const struct help_list *l = &v->l;
	int i;

	if (act == ACT_DOWN || act == ACT_UP) {
		v->cur = help_step(l, v->cur, act == ACT_DOWN ? 1 : -1);
	} else if (act == ACT_PAGE_DOWN || c == KEY_NPAGE) {
		for (i = 0; i < v->rows; i++)
			v->cur = help_step(l, v->cur, 1);
	} else if (act == ACT_PAGE_UP || c == KEY_PPAGE) {
		for (i = 0; i < v->rows; i++)
			v->cur = help_step(l, v->cur, -1);
	} else if (act == ACT_TOP || c == KEY_HOME) {
		v->cur = help_step(l, 0, 1);
		v->top = 0;
	} else if (act == ACT_BOTTOM || c == KEY_END) {
		v->cur = l->n && l->v[l->n - 1].act ?
				 l->n - 1 :
				 help_step(l, l->n ? l->n - 1 : 0, -1);
		v->top = l->n;
	} else {
		return false;
	}
	return true;
}

/*
 * The help. It lists the keys of the active pane, ? lists all of them with
 * their long descriptions, and / searches all of them. Enter runs the
 * action of the row with the cursor. Returns that action, or ACT_NONE
 * when the help was closed.
 */
enum action help_pick(void)
{
	struct help_view v = { .rebuild = true };
	enum action res = ACT_NONE, act;
	int c;

	for (;;) {
		if (v.rebuild)
			help_rebuild(&v);
		if (!help_draw(&v))
			break;
		c = user_key();
		if (c == KEY_RESIZE)
			continue;
		if (c == '\n' || c == '\r' || c == KEY_ENTER) {
			if (v.cur < v.l.n)
				res = v.l.v[v.cur].act;
			break;
		}
		if (c == KEY_DOWN || c == KEY_UP) {
			help_move(&v, c, c == KEY_DOWN ? ACT_DOWN : ACT_UP);
			continue;
		}
		if (v.typing) {
			help_type(&v, c);
			continue;
		}
		act = overlay_action(c);
		if (help_move(&v, c, act))
			continue;
		if (act == ACT_HELP) {
			v.all = !v.all;
			v.rebuild = true;
		} else if (act == ACT_SEARCH) {
			v.typing = true;
		} else {
			break;
		}
	}
	help_free(&v.l);
	return res;
}

/* ---- colours of text windows -------------------------------------------- */

/*
 * Line k of lines at (y, x) in at most maxw columns, the parts paint
 * returns in their attributes and the rest plain.
 */
static void draw_painted(int y, int x, int maxw, char *const *lines, size_t n,
			 size_t k, line_paint_fn paint)
{
	const char *l = lines[k];
	struct span sp[16];
	size_t ns = paint ? paint(lines, n, k, sp, ARRAY_SIZE(sp)) : 0;
	size_t len = strlen(l), pos = 0, j;

	for (j = 0; j <= ns && maxw > 0; j++) {
		size_t from = j < ns ? sp[j].from : len;
		int used;

		if (from > len)
			from = len;
		if (from > pos) {
			used = put_text_from(y, x, maxw, l + pos, from - pos,
					     0);
			x += used;
			maxw -= used;
			pos = from;
		}
		if (j == ns || maxw <= 0)
			break;
		if (sp[j].to > len)
			sp[j].to = len;
		if (sp[j].to <= pos)
			continue;
		attron(sp[j].a);
		used = put_text_from(y, x, maxw, l + pos, sp[j].to - pos, 0);
		attroff(sp[j].a);
		x += used;
		maxw -= used;
		pos = sp[j].to;
	}
}

/* One part [from, to) in a. Returns the number of parts up to now. */
static size_t span_add(struct span *sp, size_t ns, size_t max, size_t from,
		       size_t to, attr_t a)
{
	if (ns < max && to > from && a) {
		sp[ns].from = from;
		sp[ns].to = to;
		sp[ns].a = a;
		ns++;
	}
	return ns;
}

/*
 * A line of a diff, with bold headers and coloured removed, added and @@
 * lines.
 */
static attr_t diff_attr(const char *l)
{
	if (!strncmp(l, "+++ ", 4) || !strncmp(l, "--- ", 4) ||
	    !strncmp(l, "diff --git ", 11))
		return A_BOLD;
	if (*l == '+')
		return role_attr(C_ADD);
	if (*l == '-')
		return role_attr(C_DEL);
	if (!strncmp(l, "@@", 2))
		return role_attr(C_HUNK);
	return 0;
}

static size_t paint_diff(char *const *lines, size_t n, size_t i,
			 struct span *sp, size_t max)
{
	return span_add(sp, 0, max, 0, strlen(lines[i]), diff_attr(lines[i]));
}

/* A "Name-Of: value" trailer line of a message, indented by 4. */
static bool trailer_line(const char *l)
{
	const char *p = l + 4;

	if (strncmp(l, "    ", 4) || !isupper((unsigned char)*p))
		return false;
	while (isalnum((unsigned char)*p) || *p == '-')
		p++;
	return p[0] == ':' && p[1] == ' ';
}

/*
 * git show as D shows it, coloured as git colours it. The commit id, the
 * author dim, the subject bold, trailers, the stat's + and -, then the
 * diff.
 */
size_t paint_show(char *const *lines, size_t n, size_t i, struct span *sp,
		  size_t max)
{
	const char *l = lines[i];
	size_t len = strlen(l);
	size_t ns = 0;
	size_t j;

	if (!strncmp(l, "commit ", 7))
		return span_add(sp, 0, max, 7, len, role_attr(C_ID));
	if (!strncmp(l, "Author: ", 8) || !strncmp(l, "Date: ", 6))
		return span_add(sp, 0, max, 0, len, role_attr(C_DIM));
	if (!strncmp(l, "    ", 4)) {
		/* the first line of the message follows the header */
		j = i;
		while (j > 0 && !*lines[j - 1])
			j--;
		if (j > 0 && (!strncmp(lines[j - 1], "Author: ", 8) ||
			      !strncmp(lines[j - 1], "Date: ", 6)))
			return span_add(sp, 0, max, 4, len, A_BOLD);
		if (trailer_line(l))
			return span_add(sp, 0, max, 4, len, role_attr(C_HUNK));
		return 0;
	}
	/* the + and - of a stat line such as " path | 12 +++--" */
	if (*l == ' ' && strstr(l, " | ")) {
		const char *bar = strstr(l, " | ") + 3, *p;

		for (p = bar; *p; p++) {
			size_t k = (size_t)(p - l), e = k;

			if (*p != '+' && *p != '-')
				continue;
			while (l[e] == *p)
				e++;
			ns = span_add(sp, ns, max, k, e,
				      role_attr(*p == '+' ? C_ADD : C_DEL));
			p = l + e - 1;
		}
		return ns;
	}
	if (*l == ' ' && strstr(l, " changed"))
		return span_add(sp, 0, max, 0, len, role_attr(C_DIM));
	return paint_diff(lines, n, i, sp, max);
}

/*
 * A search hit, "path:n12: +text" or c3 for a conflict. The place is
 * coloured as git grep colours a file name, the text as a diff line.
 */
size_t paint_hit(char *const *lines, size_t n, size_t i, struct span *sp,
		 size_t max)
{
	const char *l = lines[i], *p, *q = NULL;
	size_t ns;

	/* the last ":<n|o|c><digits>: " is the place (a path has colons) */
	for (p = l + strlen(l); p-- > l;) {
		if (p[0] != ':' || !strchr("noc", p[1]))
			continue;
		for (q = p + 2; isdigit((unsigned char)*q); q++)
			;
		if (q > p + 2 && q[0] == ':' && q[1] == ' ')
			break;
	}
	if (p < l || !q)
		return 0;
	/* the text after ": " */
	p = q + 2;
	ns = span_add(sp, 0, max, 0, (size_t)(p - l), role_attr(C_HUNK));
	if (*p == '+' || *p == '-')
		ns = span_add(sp, ns, max, (size_t)(p - l), strlen(l),
			      role_attr(*p == '+' ? C_ADD : C_DEL));
	return ns;
}

/* absorb's plan, where "path:12 -> commit" goes and "... stays: why" stays. */
size_t paint_absorb(char *const *lines, size_t n, size_t i, struct span *sp,
		    size_t max)
{
	const char *l = lines[i], *p;

	if ((p = strstr(l, " -> ")))
		return span_add(sp, 0, max, (size_t)(p - l) + 4, strlen(l),
				role_attr(C_ADD));
	if ((p = strstr(l, " stays: ")))
		return span_add(sp, 0, max, (size_t)(p - l) + 1, strlen(l),
				role_attr(C_DIM));
	return 0;
}
