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

/*
 * The sections of the help, each action once, and the rest goes to Other.
 * The actions that change something come first, moving around last.
 */
static const struct {
	const char *title;
	enum action acts[24];
} help_sections[] = {
	{ "Staging and editing",
	  { ACT_STAGE, ACT_STAGE_FILE, ACT_EDIT, ACT_DISCARD, ACT_REMOVE,
	    ACT_IGNORE, ACT_OURS, ACT_THEIRS, ACT_UNDO } },
	{ "Selecting lines",
	  { ACT_RANGE, ACT_MARK, ACT_MARK_PAIR, ACT_SELECT_HUNK, ACT_CLEAR } },
	{ "Committing",
	  { ACT_COMMIT, ACT_AMEND, ACT_FIXUP_COMMIT, ACT_AMEND_COMMIT,
	    ACT_SQUASH, ACT_REWORD, ACT_SIGNOFF, ACT_MESSAGE_PANE } },
	{ "Splitting and moving changes",
	  { ACT_SPLIT, ACT_FIXUP, ACT_ABSORB, ACT_FINISH, ACT_ABORT } },
	{ "The Series pane",
	  { ACT_NEXT_COMMIT, ACT_PREV_COMMIT, ACT_SERIES_PANE } },
	{ "Going through the changes",
	  { ACT_NEXT_HUNK, ACT_PREV_HUNK, ACT_NEXT_CHANGE, ACT_PREV_CHANGE,
	    ACT_NEXT_FILE, ACT_PREV_FILE, ACT_SEARCH, ACT_SEARCH_NEXT,
	    ACT_SEARCH_PREV, ACT_SEARCH_LIST } },
	{ "Panes and the view",
	  { ACT_NEXT_PANE, ACT_PREV_PANE, ACT_ZOOM, ACT_RESIZE,
	    ACT_MORE_CONTEXT, ACT_LESS_CONTEXT, ACT_FUNC_CONTEXT, ACT_IGNORE_WS,
	    ACT_SHOW_COMMIT, ACT_LOAD_DIFF, ACT_REFRESH, ACT_REPAINT } },
	{ "Moving", { ACT_DOWN,		 ACT_UP,
		      ACT_PAGE_DOWN,	 ACT_PAGE_UP,
		      ACT_HALF_DOWN,	 ACT_HALF_UP,
		      ACT_TOP,		 ACT_BOTTOM,
		      ACT_SCREEN_TOP,	 ACT_SCREEN_MIDDLE,
		      ACT_SCREEN_BOTTOM, ACT_LINE_DOWN,
		      ACT_LINE_UP,	 ACT_SCROLL_CENTER,
		      ACT_SCROLL_TOP,	 ACT_SCROLL_BOTTOM,
		      ACT_SCROLL_LEFT,	 ACT_SCROLL_RIGHT,
		      ACT_LINE_START,	 ACT_LINE_END,
		      ACT_GOTO } },
	{ "General", { ACT_HELP, ACT_QUIT } },
};

/* One action, its keys, then its description wrapped at 52 columns. */
static void help_action(char ***v, size_t *n, size_t *alloc, enum action a)
{
	struct strbuf keys = STRBUF_INIT, line = STRBUF_INIT,
		      help = STRBUF_INIT;
	bool first = true;
	const char *d;

	/* other keys a description names, as bound */
	keys_subst(&ui.keys, actions[a].help, &help, false);
	d = sb_str(&help);
	keymap_describe(&ui.keys, a, &keys);
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
		ALLOC_GROW(*v, *n + 1, *alloc);
		(*v)[(*n)++] = sb_detach(&line);
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
 * The help text, built from the key map to show the keys in effect. A
 * section per kind of work has its title at the left, drawn bold, and its
 * actions indented. Returns the number of lines. Free them with
 * help_free().
 */
static size_t help_build(char ***out)
{
	bool listed[NACTIONS] = { false };
	size_t n = 0, alloc = 0, s, i;
	bool other = false;
	char **v = NULL;
	int a;

#define HELP_ADD(s)                          \
	do {                                 \
		ALLOC_GROW(v, n + 1, alloc); \
		v[n++] = (s);                \
	} while (0)

	HELP_ADD(xstrdup("Keys (bind with git config carve.keys.<action>; "
			 "git carve keys lists them)"));
	for (s = 0; s < ARRAY_SIZE(help_sections); s++) {
		HELP_ADD(xstrdup(""));
		HELP_ADD(xstrdup(help_sections[s].title));
		for (i = 0; i < ARRAY_SIZE(help_sections[s].acts) &&
			    help_sections[s].acts[i];
		     i++) {
			help_action(&v, &n, &alloc, help_sections[s].acts[i]);
			listed[help_sections[s].acts[i]] = true;
		}
	}
	for (a = 1; a < NACTIONS; a++) {
		if (listed[a] || !*actions[a].name)
			continue; /* shown, or ACT_PREFIX */
		if (!other) {
			HELP_ADD(xstrdup(""));
			HELP_ADD(xstrdup("Other"));
			other = true;
		}
		help_action(&v, &n, &alloc, (enum action)a);
	}
	HELP_ADD(xstrdup(""));
	HELP_ADD(xstrdup("After a conflict in fixup-to"));
	{
		struct strbuf t = STRBUF_INIT;

		keys_subst(&ui.keys,
			   "  resolve ({edit}), stage the file, "
			   "then {finish} continues and",
			   &t, false);
		HELP_ADD(sb_detach(&t));
		keys_subst(&ui.keys, "  {abort} gives up.", &t, false);
		HELP_ADD(sb_detach(&t));
	}
#undef HELP_ADD
	*out = v;
	return n;
}

static void help_free(char **v, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

/* The help overlay. It scrolls when the terminal is too short for all of it. */
void act_help(void)
{
	struct strbuf dn = STRBUF_INIT, up = STRBUF_INIT;
	char **lines;
	int n = (int)help_build(&lines), top = 0, c;

	/* the first key of each for the footer */
	keymap_describe(&ui.keys, ACT_DOWN, &dn);
	keymap_describe(&ui.keys, ACT_UP, &up);
	dn.len = strcspn(dn.buf, ",");
	dn.buf[dn.len] = '\0';
	up.len = strcspn(up.buf, ",");
	up.buf[up.len] = '\0';

	for (;;) {
		int w = COLS < 78 ? COLS : 78,
		    h = LINES < n + 3 ? LINES : n + 3;
		int rows = h - 3, y0, x0, i;
		enum action act;
		char foot[120];

		if (rows < 1 || w < 20)
			break;
		if (top > n - rows)
			top = n - rows;
		if (top < 0)
			top = 0;
		draw_under();
		y0 = (LINES - h) / 2;
		x0 = (COLS - w) / 2;
		for (i = 0; i < h; i++)
			fill_line(y0 + i, x0, w);
		/* section titles are the lines that start at the left */
		for (i = 0; i < rows && top + i < n; i++) {
			bool title = lines[top + i][0] &&
				     lines[top + i][0] != ' ';

			if (title)
				attron(A_BOLD);
			puts_at(y0 + 1 + i, x0 + 2, w - 4, lines[top + i]);
			if (title)
				attroff(A_BOLD);
		}
		if (rows < n)
			snprintf(foot, sizeof(foot),
				 "%s/%s: scroll (%d-%d of %d)"
				 "  other keys: close",
				 dn.buf, up.buf, top + 1, top + rows, n);
		else
			snprintf(foot, sizeof(foot), "Press any key to close.");
		attron(A_BOLD);
		puts_at(y0 + h - 2, x0 + 2, w - 4, foot);
		draw_frame(y0, x0, h, w);
		attroff(A_BOLD);
		refresh();
		c = user_key();
		if (c == KEY_RESIZE)
			continue;
		act = overlay_action(c);
		if (rows < n && (act == ACT_DOWN || c == KEY_DOWN))
			top++;
		else if (rows < n && (act == ACT_UP || c == KEY_UP))
			top--;
		else if (rows < n &&
			 (act == ACT_PAGE_DOWN || c == ' ' || c == KEY_NPAGE))
			top += rows;
		else if (rows < n && (act == ACT_PAGE_UP || c == KEY_PPAGE))
			top -= rows;
		else if (rows < n && (act == ACT_TOP || c == KEY_HOME))
			top = 0;
		else if (rows < n && (act == ACT_BOTTOM || c == KEY_END))
			top = n;
		else
			break;
	}
	help_free(lines, (size_t)n);
	sb_release(&dn);
	sb_release(&up);
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
