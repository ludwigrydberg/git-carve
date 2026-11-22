// SPDX-License-Identifier: MIT
/* Drawing, layout and colours. */
#include "draw.h"
#include "ui_int.h"
#include "../message.h"
#include "actions.h"
#include "state.h"
#include "sview.h"
#include "term.h"

/* draw_under(): the screen drawn but not sent, an overlay goes on top */
static bool draw_held;

static bool in_frange(size_t k);
static int series_width(void);
static void draw_series(void);
static void draw_msg_box(void);
static void diff_area(void);
static size_t series_len(void);

/*
 * The attribute for a role. That is its colour pair, or without colours an
 * attribute that keeps the role visible, and markers carry the rest.
 */
attr_t role_attr(int role)
{
	if (ui.cfg.color)
		return COLOR_PAIR(role);
	switch (role) {
	case C_WS:
	case C_TITLE:
	case C_STATUS:
		return A_REVERSE;
	case C_TITLE_ACT:
		return A_REVERSE | A_BOLD;
	case C_HUNK:
	case C_FRAME_ACT:
	case C_SEL:
	case C_ERR:
	case C_ID:
		return A_BOLD;
	default:
		return 0;
	}
}

/* Line drawing with ACS, where curses falls back itself, or ASCII if asked. */
chtype line_ch(chtype acs, char ascii)
{
	return ui.cfg.ascii_lines ? (chtype)ascii : acs;
}

/* ---- drawing ---------------------------------------------------------- */

/*
 * Draw s at (y, x) in at most maxw columns, leaving out its first skip
 * display columns for horizontal scrolling. Tabs expand from the start of
 * s, and control characters show as ^X. Returns the columns drawn.
 */
int put_text_from(int y, int x, int maxw, const char *s, size_t len, int skip)
{
	mbstate_t st;
	size_t i = 0;
	int vcol;
	vcol = 0; /* column in the whole line */

#define COL (vcol - skip)
#define PUT(ch)                                    \
	do {                                       \
		if (vcol >= skip)                  \
			mvaddch(y, x + COL, (ch)); \
		vcol++;                            \
	} while (0)

	memset(&st, 0, sizeof(st));

	while (i < len && COL < maxw) {
		unsigned char c = (unsigned char)s[i];
		wchar_t wc;
		size_t k;
		int w;

		if (c == '\t') {
			int n = ui.cfg.tabw - vcol % ui.cfg.tabw;

			while (n-- && COL < maxw)
				PUT(' ');
			i++;
			continue;
		}
		if (c < 0x20 || c == 0x7f) {
			if (COL + 2 > maxw)
				break;
			PUT('^');
			PUT(c == 0x7f ? '?' : c + 0x40);
			i++;
			continue;
		}
		if (c < 0x80) {
			PUT(c);
			i++;
			continue;
		}
		/* our own em dashes, in a terminal that cannot show them */
		if (!ui.cfg.utf8 && len - i >= 3 &&
		    !memcmp(s + i, "\xe2\x80\x94", 3)) {
			PUT('-');
			i += 3;
			continue;
		}
		k = mbrtowc(&wc, s + i, len - i, &st);
		if (k == (size_t)-1 || k == (size_t)-2 || k == 0) {
			memset(&st, 0, sizeof(st));
			PUT('?');
			i++;
			continue;
		}
		w = wcwidth(wc);
		if (w < 0)
			w = 1;
		if (COL + w > maxw)
			break;
		if (vcol >= skip) {
			mvaddnstr(y, x + COL, s + i, (int)k);
			vcol += w;
		} else {
			/* a wide character cut by the left edge */
			while (w--)
				PUT(' ');
		}
		i += k;
	}
#undef PUT
#undef COL
	return vcol > skip ? vcol - skip : 0;
}

static int put_text(int y, int x, int maxw, const char *s, size_t len)
{
	return put_text_from(y, x, maxw, s, len, 0);
}

int puts_at(int y, int x, int maxw, const char *s)
{
	return put_text(y, x, maxw, s, strlen(s));
}

void fill_line(int y, int x, int w)
{
	int i;

	for (i = 0; i < w; i++)
		mvaddch(y, x + i, ' ');
}

/* Display column where byte n of s starts, measured as put_text_from does. */
static int text_col(const char *s, size_t len, size_t n)
{
	mbstate_t st;
	size_t i = 0;
	int col = 0;

	memset(&st, 0, sizeof(st));
	while (i < n && i < len) {
		unsigned char c = (unsigned char)s[i];
		wchar_t wc;
		size_t k;
		int w;

		if (c == '\t') {
			col += ui.cfg.tabw - col % ui.cfg.tabw;
			i++;
		} else if (c < 0x20 || c == 0x7f) {
			col += 2;
			i++;
		} else if (c < 0x80) {
			col++;
			i++;
		} else {
			k = mbrtowc(&wc, s + i, len - i, &st);
			if (k == (size_t)-1 || k == (size_t)-2 || k == 0) {
				memset(&st, 0, sizeof(st));
				col++;
				i++;
				continue;
			}
			w = wcwidth(wc);
			col += w < 0 ? 1 : w;
			i += k;
		}
	}
	return col;
}

static void ws_mark(int y, int x, int maxw, const struct dline *d, size_t a,
		    size_t b, int skip)
{
	int c0 = text_col(d->text, d->len, a) - skip;
	int c1 = text_col(d->text, d->len, b) - skip;

	if (c0 < 0)
		c0 = 0;
	if (c1 > maxw)
		c1 = maxw;
	if (c0 >= c1)
		return;
	attron(role_attr(C_WS));
	fill_line(y, x + c0, c1 - c0);
	attroff(role_attr(C_WS));
}

/*
 * Whitespace errors on an added line, as git diff shows them per
 * core.whitespace. That is trailing whitespace (blank-at-eol, a CR
 * included) and spaces before a tab in the indent (space-before-tab).
 */
static void draw_ws_errors(int y, int x, int maxw, const struct dline *d,
			   int skip)
{
	size_t end = d->len, i;

	if (ui.cfg.ws_trailing) {
		while (end > 0 &&
		       (d->text[end - 1] == ' ' || d->text[end - 1] == '\t' ||
			d->text[end - 1] == '\r'))
			end--;
		if (end < d->len)
			ws_mark(y, x, maxw, d, end, d->len, skip);
	}
	if (!ui.cfg.ws_space_tab)
		return;
	for (i = 0; i < d->len && (d->text[i] == ' ' || d->text[i] == '\t');
	     i++) {
		size_t j = i;

		if (d->text[i] != ' ')
			continue;
		while (j < d->len && d->text[j] == ' ')
			j++;
		if (j < d->len && d->text[j] == '\t')
			ws_mark(y, x, maxw, d, i, j, skip);
		i = j - 1;
	}
}

/*
 * Where text is cut off, a mark says on which side it goes on, and with
 * that which of h and l brings it into view.
 */
static const char *more_left(void)
{
	return ui.cfg.utf8 ? "\xe2\x80\xb9" : "<";
}

static const char *more_right(void)
{
	return ui.cfg.utf8 ? "\xe2\x80\xba" : ">";
}

static int max_int(int a, int b)
{
	return a > b ? a : b;
}

/* Mark the last column of a row whose text goes on past it. */
static void mark_more_right(int y, int x, int w, int text_w)
{
	if (w > 1 && text_w > w) {
		attron(A_BOLD);
		puts_at(y, x + w - 1, 1, more_right());
		attroff(A_BOLD);
	}
}

/*
 * Draw the columns of s from start on in at most maxw columns, with a mark
 * at either side where text is cut off. start is kept within what the row
 * needs, at most the end of s in view, and a row that fits never moves.
 * Returns how far this row can scroll, the start that shows its end.
 */
static int puts_window(int y, int x, int maxw, const char *s, int start)
{
	size_t len = strlen(s);
	int total;
	int last;

	total = text_col(s, len, len);

	if (maxw <= 0)
		return 0;
	if (total <= maxw || maxw < 3) {
		put_text(y, x, maxw, s, len);
		return 0;
	}
	last = total - (maxw - 1);
	if (start > last)
		start = last;
	if (start <= 0) {
		put_text(y, x, maxw - 1, s, len);
	} else {
		puts_at(y, x, 1, more_left());
		put_text_from(y, x + 1, start == last ? maxw - 1 : maxw - 2, s,
			      len, start);
	}
	if (start < last)
		puts_at(y, x + maxw - 1, 1, more_right());
	return last;
}

/* Past a UTF-8 continuation byte, towards the start with back. */
static size_t char_start(const char *s, size_t i, bool back)
{
	while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80)
		i += back ? (size_t)-1 : 1;
	return i;
}

/*
 * A path cut in the middle to fit maxw columns, as file browsers do
 * (Finder, VS Code). Its first directory and its end, the file name, stay,
 * and an ellipsis stands for the rest, as in
 * "Documentation/…/Requirements/". A first directory longer than half the
 * room is cut too.
 */
static void middle_cut(const char *s, int maxw, struct strbuf *out)
{
	const char *ell = ui.cfg.utf8 ? "\xe2\x80\xa6" : "~";
	size_t len = strlen(s), head, tail;
	const char *slash = strchr(s, '/');
	int room = maxw - 1;

	head = slash && slash[1] ? (size_t)(slash - s) + 1 : 0;
	if (!head || (int)head > room / 2)
		head = char_start(s, (size_t)(room / 2), true);
	tail = char_start(s, len - (size_t)(room - (int)head), false);
	for (;;) {
		sb_reset(out);
		sb_add(out, s, head);
		sb_adds(out, ell);
		sb_add(out, s + tail, len - tail);
		if (text_col(out->buf, out->len, out->len) <= maxw ||
		    tail >= len)
			return;
		tail = char_start(s, tail + 1, false);
	}
}

/*
 * A path in at most maxw columns, cut in the middle (middle_cut), or with
 * off columns scrolled, its end in view and more of its start with each (h
 * in the Files pane). Returns how far off can go for this row.
 */
static int puts_scrolled(int y, int x, int maxw, const char *s, int off)
{
	size_t len = strlen(s);
	int last;

	last = maxw >= 3 ? text_col(s, len, len) - (maxw - 1) : 0;

	if (last < 0)
		last = 0;
	if (!off && text_col(s, len, len) > maxw && maxw >= 8) {
		struct strbuf m = STRBUF_INIT;

		middle_cut(s, maxw, &m);
		put_text(y, x, maxw, m.buf, m.len);
		sb_release(&m);
		return last;
	}
	puts_window(y, x, maxw, s, last - off);
	return last;
}

/*
 * The changed words of a line paired with one on the other side (-a +A):
 * reverse video (bold and underlined on the cursor row or without
 * colours) over what changed.
 */
static void draw_intraline(int y, int x, int maxw, const struct hunk *h,
			   size_t j, int skip, bool cursor)
{
	const struct dline *d = &h->lines[j], *p;
	long k = dline_partner(h, j);
	size_t s, ae, be, e;
	int c0, c1;
	short pair;
	attr_t a;

	if (k < 0)
		return;
	p = &h->lines[k];
	if (!change_span(d->text, d->len, p->text, p->len, &s, &ae, &be))
		return;
	e = ae;
	c0 = text_col(d->text, d->len, s) - skip;
	c1 = text_col(d->text, d->len, e) - skip;
	if (c0 < 0)
		c0 = 0;
	if (c1 > maxw)
		c1 = maxw;
	if (c0 >= c1)
		return;
	if (ui.cfg.color && !cursor) {
		a = A_REVERSE;
		pair = d->op == '+' ? C_ADD : C_DEL;
	} else {
		a = A_BOLD | A_UNDERLINE;
		pair = ui.cfg.color ? (d->op == '+' ? C_ADD : C_DEL) : 0;
	}
	mvchgat(y, x + c0, c1 - c0, a, pair, NULL);
}

/*
 * The program's line, without a background to set it apart from the panes'
 * titles. "git-carve <branch> <id> <subject>" (ui.head), with the name in
 * bold, the branch green and the id yellow as git has them.
 */
static void draw_header(int w)
{
	const char *h = ui.head, *b = strstr(h, "  "), *id;
	int x = 1;

	if (w < 1)
		return;
	if (!b) {
		attron(A_BOLD);
		puts_at(0, x, w, h);
		attroff(A_BOLD);
		return;
	}
	attron(A_BOLD);
	x += put_text(0, x, w, h, (size_t)(b - h));
	attroff(A_BOLD);
	b += 2;
	id = strstr(b, "  ");
	x += put_text(0, x, w - x + 1, "  ", 2);
	attron(role_attr(C_ADD) | A_BOLD);
	x += put_text(0, x, w - x + 1, b, id ? (size_t)(id - b) : strlen(b));
	attroff(role_attr(C_ADD) | A_BOLD);
	if (!id)
		return;
	id += 2;
	x += put_text(0, x, w - x + 1, "  ", 2);
	attron(role_attr(C_ID));
	x += put_text(0, x, w - x + 1, id, strcspn(id, " "));
	attroff(role_attr(C_ID));
	id += strcspn(id, " ");
	if (x <= w)
		puts_at(0, x, w - x + 1, id);
}

/*
 * What the panes show, "work tree" or the commit as the Series pane names
 * it ("HEAD", "~2 below HEAD").
 */
static const char *shown_label(void)
{
	static char buf[48];
	char l[32];

	if (!ui.ccommit)
		return "work tree";
	sv_label(ui.cpos, l, sizeof(l));
	snprintf(buf, sizeof(buf), "%s%s", l,
		 ui.cpos && ui.sv.own ? " below HEAD" : "");
	return buf;
}

/* The upper diff pane's title when it shows a commit, such as "Commit ~2". */
static const char *commit_title(void)
{
	static char buf[48];
	char l[32];

	sv_label(ui.cpos, l, sizeof(l));
	snprintf(buf, sizeof(buf), "Commit %s", l);
	return buf;
}

/* Columns of s, counted in code points, which is enough for these lines. */
static size_t text_cols(const char *s)
{
	size_t n = 0;

	for (; *s; s++)
		if (((unsigned char)*s & 0xC0) != 0x80)
			n++;
	return n;
}

enum { F_UL, F_UR, F_LL, F_LR, F_H, F_V };

/*
 * One character of a frame. Heavy lines need a UTF-8 terminal, and
 * elsewhere the active pane has thin lines in bold like the others.
 */
static void frame_ch(int y, int x, int part, bool heavy)
{
	static const char thin[] = "++++-|", thick[] = "####=#";
	const cchar_t *wc;

	if (ui.cfg.ascii_lines) {
		mvaddch(y, x, (chtype)(heavy ? thick : thin)[part]);
		return;
	}
	heavy = heavy && ui.cfg.utf8;
	switch (part) {
	case F_UL:
		wc = heavy ? WACS_T_ULCORNER : WACS_ULCORNER;
		break;
	case F_UR:
		wc = heavy ? WACS_T_URCORNER : WACS_URCORNER;
		break;
	case F_LL:
		wc = heavy ? WACS_T_LLCORNER : WACS_LLCORNER;
		break;
	case F_LR:
		wc = heavy ? WACS_T_LRCORNER : WACS_LRCORNER;
		break;
	case F_H:
		wc = heavy ? WACS_T_HLINE : WACS_HLINE;
		break;
	default:
		wc = heavy ? WACS_T_VLINE : WACS_VLINE;
		break;
	}
	mvadd_wch(y, x, wc);
}

/* What a pane is called where Tab is about to go. */
static const char *pane_name(int pane)
{
	switch (pane) {
	case P_SERIES:
		return "Series";
	case P_FILES:
		return "Files";
	case P_MESSAGE:
		return "Message";
	case P_UNSTAGED:
		return ui.dp[0].commit ? "Commit" :
		       ui.lay.one      ? "Diff" :
					 "Unstaged";
	default:
		return ui.lay.one ? "Diff" : "Staged";
	}
}

/*
 * The frame of a pane with its title in the top line. The active pane's
 * frame is heavy and says where Tab goes next.
 */
static void draw_pane_frame(int pane, const char *label, const char *path)
{
	const struct box *b = &ui.lay.box[pane];
	bool active = ui.active == pane;
	attr_t line = role_attr(active ? C_FRAME_ACT : C_FRAME) |
		      (active ? A_BOLD : 0);
	char title[1024], next[64] = "";
	int i, room, tw, nw = 0, k;

	if (b->h < 2 || b->w < 4)
		return;
	attron(line);
	frame_ch(b->y, b->x, F_UL, active);
	frame_ch(b->y, b->x + b->w - 1, F_UR, active);
	frame_ch(b->y + b->h - 1, b->x, F_LL, active);
	frame_ch(b->y + b->h - 1, b->x + b->w - 1, F_LR, active);
	for (i = 1; i < b->w - 1; i++) {
		frame_ch(b->y, b->x + i, F_H, active);
		frame_ch(b->y + b->h - 1, b->x + i, F_H, active);
	}
	for (i = 1; i < b->h - 1; i++) {
		frame_ch(b->y + i, b->x, F_V, active);
		frame_ch(b->y + i, b->x + b->w - 1, F_V, active);
	}
	k = keymap_first(&ui.keys, ACT_NEXT_PANE);
	if (active && k >= 0 && pane_after(pane, 1) != pane)
		snprintf(next, sizeof(next), " %s %s %s ", key_label(k),
			 ui.cfg.utf8 ? "\xe2\x86\x92" : "->",
			 pane_name(pane_after(pane, 1)));
	snprintf(title, sizeof(title), " %s%s%s ", label, path ? ": " : "",
		 path ? path : "");
	room = b->w - 4;
	tw = (int)text_cols(title);
	if (*next)
		nw = (int)text_cols(next);
	/* the title first, and where Tab goes only when both fit */
	if (tw + nw + 1 > room)
		nw = 0;
	if (nw)
		puts_at(b->y, b->x + b->w - 2 - nw, nw, next);
	attroff(line);
	attron(active ? role_attr(C_TITLE_ACT) | A_BOLD : role_attr(C_TITLE));
	puts_at(b->y, b->x + 2, room - (nw ? nw + 1 : 0), title);
	attroff(role_attr(C_TITLE_ACT) | role_attr(C_TITLE) | A_BOLD);
}

/*
 * The message pane. It shows the commit message being written as it is,
 * with comment lines dim, or what e and c do without one.
 */
static void draw_message(void)
{
	int x = ui.dp[0].x, w = ui.dp[0].w, y = ui.lay.msg_y, i;
	const struct strbuf *m = pane_message();
	const char *p = sb_str(m), *end = p + m->len, *l;
	static const char *const empty[] = {
		"No message yet.",
		"Here {edit} writes it in your editor, {commit} commits with it.",
		"{message-pane} hides this pane.",
	};
	char cc = commit_comment_char(ui.r);
	char title[48], lab[32];
	size_t len, k = 0;

	/* the message of the commit shown ("Message of ~2") */
	snprintf(title, sizeof(title), "Message");
	if (ui.ccommit) {
		sv_label(ui.cpos, lab, sizeof(lab));
		snprintf(title, sizeof(title), "Message of %s", lab);
	}
	draw_pane_frame(P_MESSAGE, title, NULL);
	for (i = 0; i < ui.lay.msg_h; i++)
		fill_line(y + i, x, w);
	if (!m->len) {
		attron(A_DIM);
		for (i = 0; i < 3 && i < ui.lay.msg_h; i++)
			puts_keys(y + i, x + 2, w - 2, empty[i]);
		attroff(A_DIM);
		return;
	}
	i = 0;
	while ((l = next_line(&p, end, &len)) && i < ui.lay.msg_h) {
		char buf[1024];

		if (k++ < ui.draft_top)
			continue;
		snprintf(buf, sizeof(buf), "%.*s", (int)len, l);
		/* the subject bold, comments (notes, git's help) dim */
		if (len && l[0] == cc)
			attron(A_DIM);
		else if (k == 1)
			attron(A_BOLD);
		puts_at(y + i, x + 2, w - 2, buf);
		attroff(A_DIM | A_BOLD);
		i++;
	}
}

static void status_text(int w, struct strbuf *out)
{
	const struct status_line *s = &ui.st;
	size_t fixed, room, subj, k;
	bool keys = true;
	const char *p;

	sb_adds(out, s->head);
	/*
	 * the keys as bound for {action}, left out when the whole subject
	 * would not fit with them, since the subject is what the pane cuts
	 */
	if (*s->keys && *s->id && s->optional_keys) {
		struct strbuf t = STRBUF_INIT;

		keys_subst(&ui.keys, s->keys, &t, false);
		if (*s->info)
			keys_subst(&ui.keys, s->info, &t, false);
		if (text_cols(s->head) + 2 + text_cols(sb_str(&t)) + 3 + 3 +
			    strlen(s->id) + 3 + text_cols(s->subject) >
		    (size_t)w)
			keys = false;
		sb_release(&t);
	}
	if (*s->keys && keys) {
		sb_adds(out, "  ");
		keys_subst(&ui.keys, s->keys, out, false);
	}
	if (*s->info) {
		sb_adds(out, " \xe2\x80\x94 ");
		keys_subst(&ui.keys, s->info, out, false);
	}
	if (!*s->id)
		return;
	/* the dash, the id and the quotes around the subject */
	fixed = text_cols(sb_str(out)) + 3 + strlen(s->id) + 3;
	room = w > 0 && (size_t)w > fixed ? (size_t)w - fixed : 0;
	subj = text_cols(s->subject);
	if (subj <= room) {
		sb_addf(out, " \xe2\x80\x94 %s \"%s\"", s->id, s->subject);
		return;
	}
	if (room < 8) {
		/* no room for a useful part of the subject */
		if (text_cols(sb_str(out)) + 3 + strlen(s->id) <= (size_t)w)
			sb_addf(out, " \xe2\x80\x94 %s", s->id);
		return;
	}
	/* room minus one columns of the subject, then an ellipsis */
	for (p = s->subject, k = 0; *p; p++) {
		if (((unsigned char)*p & 0xC0) != 0x80 && ++k > room - 1)
			break;
	}
	sb_addf(out, " \xe2\x80\x94 %s \"%.*s\xe2\x80\xa6\"", s->id,
		(int)(p - s->subject), s->subject);
}

/* puts_at() of s with {action} as the key bound to it. */
void puts_keys(int y, int x, int w, const char *s)
{
	struct strbuf t = STRBUF_INIT;

	keys_subst(&ui.keys, s, &t, false);
	puts_at(y, x, w, sb_str(&t));
	sb_release(&t);
}

/*
 * What s splits where nothing is to stage, as a hint with {split} for the
 * key ("`s` to split HEAD"); false where s splits nothing (an operation
 * in progress, a branch that is not checked out).
 */
static bool split_hint(char *buf, size_t size)
{
	char l[32];

	if (ui.split.active)
		return false;
	if (ui.series.op != OP_NONE && !ui.rebase_stop)
		return false;
	if (ui.rebase_stop || !ui.ccommit || ui.cpos >= ui.sv.n) {
		snprintf(buf, size, "`{split}` to split HEAD");
		return true;
	}
	if (!ui.sv.own)
		return false;
	/* browsing, the commit shown */
	sv_label(ui.cpos, l, sizeof(l));
	snprintf(buf, size, "`{split}` to split %s", l);
	return true;
}

/* No changes in the work tree. Say what to do instead, from row y on. */
static void files_hint(int y, int w)
{
	int x = ui.lay.list_x + 1;
	char hint[64];

	if (ui.split.active && ui.split.done) {
		puts_at(y, x, w - 1, "Split complete \xe2\x80\x94");
		puts_keys(y + 1, x, w - 1, "`{finish}` to finish");
	} else if (split_hint(hint, sizeof(hint))) {
		puts_at(y, x, w - 1, "Nothing to stage \xe2\x80\x94");
		puts_keys(y + 1, x, w - 1, hint);
	} else {
		puts_at(y, x, w - 1, "Nothing to stage");
	}
}

/* Whether the Files pane lists work-tree changes (not just a commit). */
static bool worktree_items(void)
{
	size_t k;

	for (k = 0; k < ui.nitems; k++)
		if (!ui.items[k].header && ui.items[k].sec != SEC_COMMIT)
			return true;
	/* untracked files folded, and files the filter hides */
	return ui.nuntracked > 0 || ui.filter;
}

/*
 * A section's colour, as git status has it, unstaged red, staged green,
 * the commit shown cyan. Without colours there is none, and the rule and
 * bold do the work.
 */
static attr_t sec_attr(enum sec sec)
{
	if (!ui.cfg.color)
		return 0;
	return role_attr(sec == SEC_STAGED ? C_ADD :
			 sec == SEC_COMMIT ? C_HUNK :
					     C_DEL);
}

/* The Files title, with the filter's pattern and where the cursor is. */
static void files_title(void)
{
	size_t m, nth = 0, total = 0;
	char t[340], pos[48] = "";

	for (m = 0; m < ui.nitems; m++) {
		if (ui.items[m].header)
			continue;
		total++;
		if (m <= ui.fcur)
			nth = total;
	}
	/* "3/14914" */
	if (total > 1 && ui.fcur < ui.nitems && !ui.items[ui.fcur].header)
		snprintf(pos, sizeof(pos), "%zu/%zu", nth, total);
	if (ui.filter)
		snprintf(t, sizeof(t), "/%.280s%s%s", ui.filter,
			 *pos ? "  " : "", pos);
	else
		snprintf(t, sizeof(t), "%s", pos);
	draw_pane_frame(P_FILES, "Files", *t ? t : NULL);
}

/* The first row shown, which keeps the cursor in view with its rows. */
static void files_scroll(int h)
{
	if (ui.fcur < ui.ftop || (ui.fcur == ui.ftop && ui.fcur != ui.fdrawn)) {
		/*
		 * moved onto the top row, show its section's and
		 * directory's rows too, but not when the view was scrolled
		 * there with Ctrl-E
		 */
		ui.ftop = ui.fcur;
		while (ui.ftop > 0 && ui.items[ui.ftop - 1].header &&
		       ui.fcur - (ui.ftop - 1) < (size_t)h)
			ui.ftop--;
	} else if (ui.freveal) {
		/* the list changed, show the top file with its section's row */
		while (ui.ftop > 0 && ui.items[ui.ftop - 1].header &&
		       ui.fcur - (ui.ftop - 1) < (size_t)h)
			ui.ftop--;
	}
	ui.freveal = false;
	ui.fdrawn = ui.fcur;
	if (ui.fcur >= ui.ftop + (size_t)h)
		ui.ftop = ui.fcur - (size_t)h + 1;
}

/* A directory's row, dimmed and its start cut off to fit. Returns its width. */
static int files_dir_row(int y, size_t k, int w, bool mine)
{
	char d[1024];
	int need;

	snprintf(d, sizeof(d), "%.*s", (int)ui.items[k].dirlen,
		 item_path(k + 1 < ui.nitems ? k + 1 : k));
	attron(A_DIM);
	fill_line(y, ui.lay.list_x, w);
	need = puts_scrolled(y, ui.lay.list_x + 1, w - 1, d,
			     mine ? ui.fhscroll : 0);
	attroff(A_DIM);
	return need;
}

/* A section's row, a rule with the name in it, "── Staged (2) ──────". */
static void files_section_row(int y, size_t k, int w)
{
	const struct fitem *it = &ui.items[k];
	int x, tl, x0 = ui.lay.list_x;
	size_t n = 0, m;
	char buf[1024];

	/* the section's files, up to the next section */
	for (m = k + 1;
	     m < ui.nitems && (!ui.items[m].header || item_is_dir(m)); m++)
		n += !ui.items[m].header;
	attron(A_BOLD | sec_attr(it->sec));
	if (it->sec == SEC_COMMIT) {
		/* the commit selected in the Series pane */
		char l[32];

		sv_label(ui.cpos, l, sizeof(l));
		snprintf(buf, sizeof(buf), "Commit %s (%zu)", l, n);
	} else if (it->untracked) {
		/* folded, the files are not in the list */
		snprintf(buf, sizeof(buf), "Untracked (%zu) %s", ui.nuntracked,
			 untracked_shown() ? "[-]" : "[+]");
	} else {
		snprintf(buf, sizeof(buf), "%s (%zu)",
			 it->sec == SEC_STAGED ? "Staged" : "Unstaged", n);
	}
	/* the cursor on it (the Untracked row, which folds): '>' */
	if (k == ui.fcur)
		attron(ui.active == P_FILES ? A_REVERSE : A_UNDERLINE);
	tl = (int)strlen(buf);
	mvaddch(y, x0, k == ui.fcur ? '>' : line_ch(ACS_HLINE, '-'));
	for (x = tl + 3; x < w; x++)
		mvaddch(y, x0 + x, line_ch(ACS_HLINE, '-'));
	mvaddch(y, x0 + 1, ' ');
	puts_at(y, x0 + 2, w - 2, buf);
	if (tl + 2 < w)
		mvaddch(y, x0 + tl + 2, ' ');
	attroff(A_BOLD | A_REVERSE | A_UNDERLINE | sec_attr(it->sec));
}

/*
 * A file's row, with a mark, the status letter and the name (old -> new
 * for a rename).
 */
static int files_file_row(int y, size_t k, int w, bool mine)
{
	const struct fitem *it = &ui.items[k];
	/* without a status entry, a file of the commit shown */
	const struct file_status *f = it->sec == SEC_COMMIT ? NULL :
							      &ui.fs[it->fi];
	char letter = !f		    ? ui.cfiles[it->fi].status :
		      f->unmerged	    ? 'U' :
		      it->sec == SEC_STAGED ? f->x :
					      f->y;
	const char *from = !f			 ? ui.cfiles[it->fi].from :
			   it->sec == SEC_STAGED ? f->from_x :
						   f->from_y;
	/* under its directory's row, indented, the name only */
	int in = it->dirlen ? 2 : 0, need, x0 = ui.lay.list_x;
	char buf[4], pb[1024];

	if (k == ui.fcur)
		attron(ui.active == P_FILES ? A_REVERSE : A_UNDERLINE);
	fill_line(y, x0, w);
	/* + in a v range of files */
	buf[0] = k == ui.fcur ? '>' : in_frange(k) ? '+' : ' ';
	buf[1] = letter;
	buf[2] = ' ';
	buf[3] = '\0';
	/* the letter in its section's colour, red for conflicts */
	attron(letter == 'U' ? role_attr(C_DEL) : sec_attr(it->sec));
	puts_at(y, x0, 3, buf);
	attroff(role_attr(C_DEL) | sec_attr(it->sec));
	snprintf(pb, sizeof(pb), "%s%s%s", from ? from : "", from ? " -> " : "",
		 item_path(k) + it->dirlen);
	need = puts_scrolled(y, x0 + 3 + in, w - 3 - in, pb,
			     mine ? ui.fhscroll : 0);
	attroff(A_REVERSE | A_UNDERLINE);
	return need;
}

static void draw_files(void)
{
	int fmax = 0; /* how far h can scroll the cursor's rows */
	int y0 = ui.lay.files_y + 1, h = ui.lay.files_h, w = ui.lay.files_w;
	size_t k, dir = (size_t)-1;
	int i;

	files_title();
	if (!ui.nitems && ui.filter) {
		puts_at(y0 + 1, ui.lay.list_x + 1, w - 1,
			"No file matches the filter");
		puts_keys(y0 + 2, ui.lay.list_x + 1, w - 1,
			  "(`{clear}` clears it)");
		return;
	}
	if (!ui.nitems) {
		files_hint(y0 + 1, w);
		return;
	}
	files_scroll(h);
	/*
	 * h and l scroll only the selected file and its directory's row,
	 * and the other rows keep their end, the name, in view
	 */
	if (ui.fhscroll_row != ui.fcur) {
		ui.fhscroll_row = ui.fcur;
		ui.fhscroll = 0;
	}
	for (k = ui.fcur; k > 0 && k < ui.nitems; k--) {
		if (item_is_dir(k - 1)) {
			dir = k - 1;
			break;
		}
		if (ui.items[k - 1].header)
			break;
	}
	for (i = 0, k = ui.ftop; i < h && k < ui.nitems; i++, k++) {
		bool mine = k == ui.fcur || k == dir;
		int need;

		if (item_is_dir(k)) {
			need = files_dir_row(y0 + i, k, w, mine);
		} else if (ui.items[k].header) {
			files_section_row(y0 + i, k, w);
			continue;
		} else {
			need = files_file_row(y0 + i, k, w, mine);
		}
		if (mine)
			fmax = max_int(fmax, need);
	}
	ui.fhscroll_max = fmax;
	if (ui.fhscroll > fmax)
		ui.fhscroll = fmax;
	/* with only a commit listed, the hint goes below it */
	if (!worktree_items() && i + 3 <= h)
		files_hint(y0 + i + 1, w);
}

static bool in_frange(size_t k)
{
	size_t a = ui.fanchor < ui.fcur ? ui.fanchor : ui.fcur;
	size_t b = ui.fanchor < ui.fcur ? ui.fcur : ui.fanchor;

	return ui.franging && ui.active == P_FILES && k >= a && k <= b &&
	       !ui.items[k].header && ui.items[k].sec == ui.items[ui.fcur].sec;
}

static bool in_range(const struct dpane *dp, size_t i)
{
	size_t a = dp->anchor, b = dp->cur;

	if (!dp->ranging)
		return false;
	if (a > b) {
		size_t t = a;

		a = b;
		b = t;
	}
	return i >= a && i <= b;
}

/* Keep the cursor and selection markers visible without colour. */
static void draw_diff_gutter(int y, int x, bool marked, bool ranged,
			     bool cursor, bool active)
{
	attr_t focus = cursor && active ? role_attr(C_TITLE_ACT) : 0;

	attron(focus);
	mvaddch(y, x, cursor ? '>' : ' ');
	mvaddch(y, x + 1, marked ? '+' : ranged ? '*' : ' ');
	attroff(focus);
}

static void draw_diff(struct dpane *dp, bool active)
{
	size_t i;
	int y;

	if (dp->h <= 0)
		return;
	if (dp->cur < dp->top)
		dp->top = dp->cur;
	if (dp->cur >= dp->top + (size_t)dp->h)
		dp->top = dp->cur - (size_t)dp->h + 1;
	for (y = 0, i = dp->top; y < dp->h && i < dp->nrows; y++, i++) {
		int yy = dp->y + y, x = dp->x, w = dp->w;
		const struct row *rw = &dp->rows[i];
		attr_t cattr = 0;
		bool selected = in_range(dp, i) || rw->marked;
		char buf[64];

		if (selected)
			attron(role_attr(C_SEL));
		if (i == dp->cur && !active)
			attron(A_UNDERLINE);
		fill_line(yy, x, w);
		draw_diff_gutter(yy, x, rw->marked, in_range(dp, i),
				 i == dp->cur, active && !selected);
		x += ROW_MARGIN;
		w -= ROW_MARGIN;
		switch (rw->kind) {
		case R_NOTE: {
			/*
			 * in conflict rows ours look removed and theirs
			 * added
			 */
			static const int role[] = {
				[CROW_NOTE] = C_DIM,   [CROW_HEAD] = C_HUNK,
				[CROW_OURS] = C_DEL,   [CROW_BASE] = C_DIM,
				[CROW_THEIRS] = C_ADD,
			};
			attr_t a;

			a = selected ?
				    0 :
				    role_attr(
					    role[rw->j <= CROW_THEIRS ? rw->j :
									0]);

			attron(a);
			put_text_from(yy, x + 1, w - 1, rw->note,
				      strlen(rw->note),
				      rw->j == CROW_NOTE ? 0 : dp->hscroll);
			attroff(a);
			break;
		}
		case R_HUNK: {
			const struct hunk *h = &dp->fd->hunks[rw->h];
			char hb[512];

			snprintf(hb, sizeof(hb), "@@ -%d,%d +%d,%d @@%s%s",
				 h->old_start, h->old_count, h->new_start,
				 h->new_count, *h->section ? " " : "",
				 h->section);
			if (!selected)
				attron(role_attr(C_HUNK));
			puts_at(yy, x, w, hb);
			attroff(role_attr(C_HUNK));
			break;
		}
		case R_LINE: {
			char o[8] = "", n[8] = "";
			const struct dline *d =
				&dp->fd->hunks[rw->h].lines[rw->j];

			if (d->old_no)
				snprintf(o, sizeof(o), "%d", d->old_no);
			if (d->new_no)
				snprintf(n, sizeof(n), "%d", d->new_no);
			snprintf(buf, sizeof(buf), "%5s %5s ", o, n);
			puts_at(yy, x, w, buf);
			/* the line text is scrolled (h / l): more on the left */
			if (dp->hscroll && w > 12) {
				attron(A_BOLD);
				puts_at(yy, x + 11, 1, more_left());
				attroff(A_BOLD);
			}
			if (!selected) {
				if (d->op == '+')
					cattr = role_attr(C_ADD);
				else if (d->op == '-')
					cattr = role_attr(C_DEL);
			}
			attron(cattr);
			buf[0] = d->op;
			buf[1] = '\0';
			if (w > 12)
				puts_at(yy, x + 12, 1, buf);
			if (w > 13) {
				int used = put_text_from(yy, x + 13, w - 13,
							 d->text, d->len,
							 dp->hscroll);

				if (d->no_eol && used + 3 < w - 13)
					puts_at(yy, x + 13 + used + 1,
						w - 14 - used, "\\ no newline");
				if (d->op != ' ' && !selected)
					draw_intraline(yy, x + 13, w - 13,
						       &dp->fd->hunks[rw->h],
						       rw->j, dp->hscroll,
						       i == dp->cur);
				if (d->op == '+' && !selected)
					draw_ws_errors(yy, x + 13, w - 13, d,
						       dp->hscroll);
				mark_more_right(yy, x + 13, w - 13,
						text_col(d->text, d->len,
							 d->len) -
							dp->hscroll);
			}
			attroff(cattr);
			break;
		}
		}
		attroff(A_REVERSE | A_UNDERLINE | role_attr(C_SEL));
	}
}

/* Rows the Series pane wants (title included), 0 when it is not shown. */
static int series_want(void)
{
	int want;

	if (!series_mode() || ui.series_hidden)
		return 0;
	/* the top and bottom lines of the frame */
	want = (int)series_len() + 2;
	/* the upstream divider, "..." for more */
	want += ui.sv.bounded && ui.sv.inrange < ui.sv.n;
	want += !ui.sv.end;
	return want;
}

/*
 * Narrow (COLS below carve.narrowWidth): the lists across the whole
 * width on top, the two diffs below them, each as wide as the screen.
 */
/* v within lo..hi (hi wins when they cross). */
static int clamp(int v, int lo, int hi)
{
	if (v < lo)
		v = lo;
	if (v > hi)
		v = hi;
	return v;
}

static void layout_stacked(int body)
{
	int rows = body + 1, list, want = series_want(), d, h0;

	ui.lay.files_w = COLS;
	/*
	 * the lists get what they hold, up to half, and the diffs at least
	 * six
	 */
	list = want + 2 + (int)(ui.nitems ? ui.nitems : 2);
	if (list > rows / 2)
		list = rows / 2;
	if (list < 3)
		list = 3;
	if (rows - list < 6 && rows >= 9)
		list = rows - 6;
	if (ui.lay.want_list_h)
		list = clamp(ui.lay.want_list_h, 3, rows - 6);
	/*
	 * the Series pane gets up to a third of them, the rest is for the
	 * files
	 */
	ui.lay.series_h = want < list / 3 ? want : list / 3;
	if (want && ui.lay.series_h < 3 && list >= 7)
		ui.lay.series_h = 3;
	if (want && ui.lay.want_series_h)
		ui.lay.series_h = clamp(ui.lay.want_series_h, 3, list - 3);
	ui.lay.files_y = 1 + ui.lay.series_h;
	ui.lay.files_h = list - 1 - ui.lay.series_h;
	d = rows - list;
	h0 = (d - 2) / 2;
	if (ui.lay.want_top_h)
		h0 = clamp(ui.lay.want_top_h, 1, d - 3);
	ui.dp[0].y = list + 2;
	ui.dp[0].h = h0;
	ui.dp[1].y = list + 3 + h0;
	ui.dp[1].h = d - 2 - h0;
	ui.dp[0].x = ui.dp[1].x = 0;
	ui.dp[0].w = ui.dp[1].w = COLS;
}

/*
 * The diff area as the layouts above left it, from dp[0]'s title row to
 * dp[1]'s last row. The message pane goes on top of it when shown, then
 * one diff (in layout one the one of the cursor's section) or both.
 */
static void diff_area(void)
{
	int top = ui.dp[0].y - 1, bottom = ui.dp[1].y + ui.dp[1].h;
	int rows = bottom - top, lines, k;

	ui.lay.msg_h = 0;
	if (ui.lay.msg_on) {
		const struct strbuf *m = pane_message();

		lines = (int)count_lines(sb_str(m), m->len);
		/* and the bottom line of its frame */
		ui.lay.msg_h = (lines < 3 ? 3 : lines > 8 ? 8 : lines) + 1;
		/* the diff keeps six rows at least */
		if (rows - ui.lay.msg_h - 1 < 7)
			ui.lay.msg_h = rows - 8;
		if (ui.lay.msg_h < 3)
			ui.lay.msg_h = 0;
	}
	ui.lay.msg_y = top + 1;
	if (ui.lay.msg_h)
		top += ui.lay.msg_h + 1;
	rows = bottom - top;
	if (!ui.lay.one) {
		int h0 = ui.dp[0].h * (rows - 2) / (ui.dp[0].h + ui.dp[1].h);

		if (h0 < 2)
			h0 = 2;
		ui.dp[0].y = top + 1;
		ui.dp[0].h = h0;
		ui.dp[1].y = top + 2 + h0;
		ui.dp[1].h = rows - 2 - h0;
		return;
	}
	k = one_side();
	ui.dp[k].y = top + 1;
	ui.dp[k].h = rows - 1;
	ui.dp[!k].h = 0;
}

static void layout_panes(void)
{
	int body = LINES - 4; /* title row, pane titles, status, msg */
	int rw, top_h, want;

	ui.lay.stacked = COLS < ui.lay.narrow_width;
	if (ui.lay.stacked) {
		layout_stacked(body);
		goto zoom;
	}
	ui.lay.files_w = COLS / 4;
	if (ui.lay.files_w < 20)
		ui.lay.files_w = 20;
	if (ui.lay.files_w > 40)
		ui.lay.files_w = 40;
	if (ui.lay.files_w > COLS - 20)
		ui.lay.files_w = COLS / 2;
	if (ui.lay.want_left_w) {
		ui.lay.files_w = clamp(ui.lay.want_left_w, 12, COLS - 20);
	} else if (ui.active == P_SERIES && series_mode()) {
		/*
		 * the Series pane with the cursor is wide enough for the
		 * subjects
		 */
		int wide = series_width() + 1, max = COLS - 24;

		if (wide > max)
			wide = max;
		if (wide > ui.lay.files_w)
			ui.lay.files_w = wide;
	}
	/* the title and rows of the Series pane above the Files pane */
	want = series_want();
	ui.lay.series_h = want < body / 3 ? want : body / 3;
	if (want && ui.lay.series_h < 3)
		ui.lay.series_h = body >= 9 ? 3 : 0;
	if (want && ui.lay.series_h && ui.lay.want_series_h)
		ui.lay.series_h = clamp(ui.lay.want_series_h, 3, body - 3);
	ui.lay.files_y = 1 + ui.lay.series_h;
	ui.lay.files_h = body - ui.lay.series_h;
	rw = COLS - ui.lay.files_w - 1;
	top_h = (body - 1) / 2;
	if (ui.lay.want_top_h)
		top_h = clamp(ui.lay.want_top_h, 1, body - 3);
	ui.dp[0].y = 2;
	ui.dp[0].x = ui.lay.files_w + 1;
	ui.dp[0].h = top_h;
	ui.dp[0].w = rw;
	ui.dp[1].y = 2 + top_h + 1;
	ui.dp[1].x = ui.lay.files_w + 1;
	ui.dp[1].h = body - top_h - 1;
	ui.dp[1].w = rw;
zoom:
	diff_area();
	/* zoomed, the active pane alone on the whole screen */
	if (ui.lay.zoom && ((ui.active == P_SERIES && !series_want()) ||
			    ui.active == P_MESSAGE))
		ui.lay.zoom = false;
	if (!ui.lay.zoom)
		return;
	ui.lay.msg_h = 0;
	ui.dp[0].h = ui.dp[1].h = 0;
	if (ui.active == P_UNSTAGED || ui.active == P_STAGED) {
		struct dpane *z = &ui.dp[ui.active == P_STAGED];

		ui.lay.series_h = ui.lay.files_h = 0;
		z->y = 2;
		z->x = 0;
		z->w = COLS;
		z->h = body;
		return;
	}
	ui.lay.files_w = COLS;
	if (ui.active == P_SERIES) {
		ui.lay.series_h = body + 1;
		ui.lay.files_h = 0;
	} else {
		ui.lay.series_h = 0;
		ui.lay.files_y = 1;
		ui.lay.files_h = body;
	}
}

/*
 * Each pane's title line becomes the top of its frame and its last row
 * the bottom. Side by side the left frames take the column between the
 * lists and the diffs.
 */
static void frame_panes(void)
{
	struct ui_layout *l = &ui.lay;
	int lw = l->files_w + (!l->zoom && !l->stacked), k;

	memset(l->box, 0, sizeof(l->box));
	if (l->series_h < 3)
		l->series_h = 0;
	if (l->series_h) {
		l->box[P_SERIES] = (struct box){ 1, 0, l->series_h, lw };
		l->series_h--;
	}
	if (l->files_h > 0) {
		l->box[P_FILES] =
			(struct box){ l->files_y, 0, l->files_h + 1, lw };
		l->files_h--;
	}
	l->list_x = 1;
	l->files_w = lw - 2;
	if (l->msg_h > 0) {
		l->box[P_MESSAGE] = (struct box){ l->msg_y - 1, ui.dp[0].x,
						  l->msg_h + 1, ui.dp[0].w };
		l->msg_h--;
	}
	for (k = 0; k < 2; k++) {
		struct dpane *dp = &ui.dp[k];

		if (dp->h > 0) {
			l->box[P_UNSTAGED + k] = (struct box){ dp->y - 1, dp->x,
							       dp->h + 1,
							       dp->w };
			dp->h--;
		}
		dp->x++;
		dp->w -= 2;
	}
}

static void layout(void)
{
	layout_panes();
	frame_panes();
}

bool too_small(void)
{
	return COLS < MIN_COLS || LINES < MIN_LINES;
}

static bool cursor_has_pair(void)
{
	const struct dpane *dp = active_dp();
	const struct row *row;

	if (!dp || !dp->fd || dp->cur >= dp->nrows)
		return false;
	row = &dp->rows[dp->cur];
	if (row->kind != R_LINE ||
	    dline_partner(&dp->fd->hunks[row->h], row->j) < 0)
		return false;
	return true;
}

/* "Tab: pane Space: stage/unstage ..." with the keys actually bound. */
static void status_hint(char *buf, size_t size)
{
	static const struct {
		enum action a;
		const char *what;
	} items[] = {
		{ ACT_NEXT_PANE, "pane" }, { ACT_STAGE, "stage/unstage" },
		{ ACT_RANGE, "range" },	   { ACT_EDIT, "edit" },
		{ ACT_COMMIT, "commit" },  { ACT_QUIT, "quit" },
		{ ACT_HELP, "keys" },
	};
	size_t i, len = 0;
	char key[32];

	buf[0] = '\0';
	if (cursor_has_pair() &&
	    keymap_key(&ui.keys, ACT_MARK_PAIR, key, sizeof(key)))
		len = (size_t)snprintf(buf, size, "%s: mark both lines", key);
	for (i = 0; i < ARRAY_SIZE(items) && len < size; i++) {
		int k = keymap_first(&ui.keys, items[i].a);

		if (k < 0)
			continue;
		len += (size_t)snprintf(buf + len, size - len, "%s%s: %s",
					len ? "  " : "", key_label(k),
					items[i].what);
	}
}

void draw(void)
{
	int i;

	if (too_small()) {
		erase();
		attron(A_BOLD);
		puts_at(0, 0, COLS, "terminal too small");
		attroff(A_BOLD);
		if (LINES > 1) {
			char buf[64];

			snprintf(buf, sizeof(buf), "need %dx%d, have %dx%d",
				 MIN_COLS, MIN_LINES, COLS, LINES);
			puts_at(1, 0, COLS, buf);
		}
		if (LINES > 2)
			puts_keys(2, 0, COLS, "{quit}: quit");
		refresh();
		return;
	}
	layout();
	erase();
	/* the title stops before the labels on its right */
	i = 9;
	if (ui.signoff && COLS > 50)
		i = 19;
	if (bg_running(&ui.bg) && COLS > 40)
		i = 39;
	draw_header(COLS - 1 - i);
	attron(A_DIM);
	if (bg_running(&ui.bg) && COLS > 40)
		puts_at(0, COLS - 38, 19, "scanning untracked");
	if (ui.signoff && COLS > 50)
		puts_at(0, COLS - 18, 9, "sign-off");
	puts_at(0, COLS - 8 > 0 ? COLS - 8 : 0, 8, "?:help");
	attroff(A_DIM);
	attroff(role_attr(C_TITLE) | A_BOLD);

	if (ui.lay.files_h > 0)
		draw_files();
	draw_series();
	{
		/*
		 * The path ("old -> new" for a rename), and the view options
		 * when not the defaults.
		 */
		const struct file_status *f = cur_fs();
		char pth[2][1024], opt[64] = "";
		int k, n = 0;

		if (ui.dopts.context != 3)
			n = snprintf(opt, sizeof(opt), " -U%d",
				     ui.dopts.context);
		snprintf(opt + n, sizeof(opt) - (size_t)n, "%s%s",
			 ui.dopts.function_context ? " -W" : "",
			 ui.dopts.ignore_ws ? " -w (view only)" : "");
		for (k = 0; k < 2; k++) {
			const char *from = !f ? NULL :
					   k  ? f->from_x :
						f->from_y;

			snprintf(pth[k], sizeof(pth[k]), "%s%s%s%s%s%s",
				 from ? from : "", from ? " -> " : "",
				 ui.path ? ui.path : "", *opt ? "  [" : "",
				 *opt ? opt + 1 : "", *opt ? "]" : "");
			/* how many lines are marked, whatever is on screen */
			if (ui.dp[k].nmarked) {
				size_t l = strlen(pth[k]);

				snprintf(pth[k] + l, sizeof(pth[k]) - l,
					 "  [%zu marked]", ui.dp[k].nmarked);
			}
		}
		for (k = 0; ui.lay.zoom && k < 2; k++) {
			size_t l = strlen(pth[k]);

			snprintf(pth[k] + l, sizeof(pth[k]) - l, "  [zoom]");
		}
		if (ui.lay.msg_h)
			draw_message();
		/* zoomed, only the active pane has any rows */
		if (ui.dp[0].h > 0) {
			draw_pane_frame(P_UNSTAGED,
					ui.dp[0].commit ? commit_title() :
					ui.lay.one	? "Diff (unstaged)" :
							  "Unstaged",
					ui.path ? pth[0] : NULL);
			draw_diff(&ui.dp[0], ui.active == P_UNSTAGED);
		}
		if (ui.dp[1].h > 0) {
			draw_pane_frame(P_STAGED,
					ui.lay.one ? "Diff (staged)" : "Staged",
					ui.path ? pth[1] : NULL);
			draw_diff(&ui.dp[1], ui.active == P_STAGED);
		}
	}

	attron(role_attr(C_STATUS));
	fill_line(LINES - 2, 0, COLS);
	if (ui.lay.resizing) {
		puts_keys(LINES - 2, 1, COLS - 2,
			  ui.lay.stacked ?
				  "RESIZE  {down}/{up}: move the divider  "
				  "{next-pane}: another pane  =: default  "
				  "Enter: done" :
				  "RESIZE  {scroll-left}/{scroll-right}: "
				  "narrower/wider  {down}/{up}: move the "
				  "divider  {next-pane}: another pane  =: "
				  "default  Enter: done");
	} else if (ui.st.head[0]) {
		struct strbuf sb = STRBUF_INIT;

		status_text(COLS - 2, &sb);
		puts_at(LINES - 2, 1, COLS - 2, sb.buf);
		sb_release(&sb);
	} else if (!worktree_items()) {
		char hint[64], line[96];

		snprintf(line, sizeof(line), "Nothing to stage%s%s",
			 split_hint(hint, sizeof(hint)) ? " \xe2\x80\x94 " : "",
			 split_hint(hint, sizeof(hint)) ? hint : "");
		puts_keys(LINES - 2, 1, COLS - 2, line);
	} else {
		char hint[160];

		status_hint(hint, sizeof(hint));
		puts_at(LINES - 2, 1, COLS - 2, hint);
	}
	attroff(role_attr(C_STATUS));
	if (ui.msg.box) {
		draw_msg_box();
	} else if (ui.msg.text[0]) {
		if (ui.msg.err)
			attron(role_attr(C_ERR) | A_BOLD);
		puts_at(LINES - 1, 0, COLS, ui.msg.text);
		attroff(role_attr(C_ERR) | A_BOLD);
	}
	if (!draw_held)
		refresh();
}

void draw_under(void)
{
	draw_held = true;
	draw();
	draw_held = false;
}

/* ---- the message box, a refusal or a warning mid-screen ---- */

struct wrapped {
	char **v;
	size_t n, alloc;
};

static void wrap_push(struct wrapped *w, const char *s, size_t len)
{
	ALLOC_GROW(w->v, w->n + 1, w->alloc);
	w->v[w->n++] = xstrndup(s, len);
}

/*
 * One paragraph without newlines in lines of cols columns, broken at
 * spaces, else where it has to be, as in a long path.
 */
static void wrap_para(struct wrapped *w, const char *s, size_t len, int cols)
{
	if (!len)
		wrap_push(w, "", 0);
	while (len) {
		size_t i, brk = 0, hard = 0;

		if (text_col(s, len, len) <= cols) {
			wrap_push(w, s, len);
			return;
		}
		for (i = 1; i <= len && text_col(s, len, i) <= cols; i++) {
			if ((s[i] & 0xc0) != 0x80)
				hard = i;
			if (i < len && s[i] == ' ')
				brk = i;
		}
		if (!brk)
			brk = hard ? hard : 1;
		wrap_push(w, s, brk);
		s += brk;
		len -= brk;
		while (len && *s == ' ') {
			s++;
			len--;
		}
	}
}

/*
 * The message in lines of cols columns. A one-line message "what; what to
 * do" puts what to do apart, below.
 */
static void wrap_msg(struct wrapped *w, const char *msg, int cols)
{
	const char *semi = strstr(msg, "; ");
	const char *p = msg;

	if (semi && !strchr(msg, '\n')) {
		wrap_para(w, msg, (size_t)(semi - msg), cols);
		wrap_push(w, "", 0);
		wrap_para(w, semi + 2, strlen(semi + 2), cols);
		return;
	}
	while (*p) {
		size_t len = strcspn(p, "\n");

		wrap_para(w, p, len, cols);
		p += len;
		if (*p)
			p++;
	}
}

static void wrapped_free(struct wrapped *w)
{
	size_t i;

	for (i = 0; i < w->n; i++)
		free(w->v[i]);
	free(w->v);
}

/* The box as the last draw showed it, its lines and how many fit. */
static size_t box_n, box_rows;

static void draw_msg_box(void)
{
	struct wrapped wr = { 0 };
	int w, h, y0, x0, rows, i;
	char foot[96];

	w = COLS - 8 > 84 ? 84 : COLS - 8;
	if (w < 24)
		w = COLS - 2;
	/* frame, title, blank, the lines, blank, footer, frame */
	rows = LINES - 4 - 6;
	if (w < 16 || rows < 1) {
		attron(role_attr(C_ERR) | A_BOLD);
		puts_at(LINES - 1, 0, COLS, ui.msg.text);
		attroff(role_attr(C_ERR) | A_BOLD);
		return;
	}
	wrap_msg(&wr, ui.msg.text, w - 4);
	if (wr.n < (size_t)rows)
		rows = (int)wr.n;
	box_n = wr.n;
	box_rows = (size_t)rows;
	if (ui.msg.top + box_rows > box_n)
		ui.msg.top = box_n - box_rows;
	h = rows + 6;
	y0 = (LINES - h) / 2;
	x0 = (COLS - w) / 2;
	for (i = 0; i < h; i++)
		fill_line(y0 + i, x0, w);
	attron(role_attr(C_ERR) | A_BOLD);
	puts_at(y0 + 1, x0 + 2, w - 4, ui.msg.title);
	attroff(role_attr(C_ERR) | A_BOLD);
	for (i = 0; i < rows; i++)
		puts_at(y0 + 3 + i, x0 + 2, w - 4,
			wr.v[ui.msg.top + (size_t)i]);
	if (box_n > box_rows)
		snprintf(foot, sizeof(foot),
			 "up/down: scroll (lines %zu-%zu "
			 "of %zu)  any other key: close",
			 ui.msg.top + 1, ui.msg.top + box_rows, box_n);
	else
		snprintf(foot, sizeof(foot), "any key: close");
	attron(A_BOLD);
	puts_at(y0 + h - 2, x0 + 2, w - 4, foot);
	draw_frame(y0, x0, h, w);
	attroff(A_BOLD);
	wrapped_free(&wr);
}

/*
 * A key while the box is shown. True when it scrolled a long box, false
 * when it closes it.
 */
bool msg_box_key(int c)
{
	size_t last = box_n > box_rows ? box_n - box_rows : 0;
	enum action act = key_action(&ui.keys, c);

	if (!last)
		return false;
	if (act == ACT_DOWN || c == KEY_DOWN)
		ui.msg.top = ui.msg.top < last ? ui.msg.top + 1 : last;
	else if (act == ACT_UP || c == KEY_UP)
		ui.msg.top = ui.msg.top ? ui.msg.top - 1 : 0;
	else if (act == ACT_PAGE_DOWN || c == KEY_NPAGE)
		ui.msg.top = ui.msg.top + box_rows < last ?
				     ui.msg.top + box_rows :
				     last;
	else if (act == ACT_PAGE_UP || c == KEY_PPAGE)
		ui.msg.top = ui.msg.top > box_rows ? ui.msg.top - box_rows : 0;
	else
		return false;
	return true;
}

/* ---- main loop -------------------------------------------------------- */

/*
 * The colours of each role, as foreground and background. A background of
 * -1 is the terminal's own. The 8 basic colours look as the terminal's
 * theme makes them. The 256 colour set picks softer shades of its own,
 * for a dark terminal background.
 */
static const short colors8[][2] = {
	[C_ADD] = { COLOR_GREEN, -1 },
	[C_DEL] = { COLOR_RED, -1 },
	[C_HUNK] = { COLOR_CYAN, -1 },
	[C_SEL] = { COLOR_BLACK, COLOR_YELLOW },
	[C_TITLE] = { -1, -1 },
	[C_TITLE_ACT] = { COLOR_BLACK, COLOR_CYAN },
	[C_ERR] = { COLOR_RED, -1 },
	[C_DIM] = { COLOR_BLUE, -1 },
	[C_STATUS] = { COLOR_BLACK, COLOR_WHITE },
	[C_WS] = { COLOR_WHITE, COLOR_RED },
	[C_ID] = { COLOR_YELLOW, -1 },
	[C_FRAME] = { -1, -1 },
	[C_FRAME_ACT] = { COLOR_CYAN, -1 },
};

static const short colors256[][2] = {
	[C_ADD] = { 114, -1 },	     [C_DEL] = { 174, -1 },
	[C_HUNK] = { 110, -1 },	     [C_SEL] = { 235, 222 },
	[C_TITLE] = { 252, 238 },    [C_TITLE_ACT] = { 235, 110 },
	[C_ERR] = { 203, -1 },	     [C_DIM] = { 247, -1 },
	[C_STATUS] = { 252, 237 },   [C_WS] = { 255, 131 },
	[C_ID] = { 179, -1 },	     [C_FRAME] = { 240, -1 },
	[C_FRAME_ACT] = { 110, -1 },
};

void init_colors(void)
{
	const char *nc = getenv("NO_COLOR");
	const short (*set)[2] = colors8;
	bool own_bg = true;
	int i;

	/* https://no-color.org, set and not empty */
	ui.cfg.color = has_colors() && !(nc && *nc);
	if (!ui.cfg.color)
		return;
	start_color();
	if (use_default_colors() == ERR)
		own_bg = false;
	if (ui.cfg.colors256 && COLORS >= 256)
		set = colors256;
	for (i = C_ADD; i <= C_FRAME_ACT; i++) {
		short bg = set[i][1];

		if (bg < 0 && !own_bg)
			bg = COLOR_BLACK;
		init_pair((short)i, set[i][0], bg);
	}
}

/* ---- the Series pane ---------------------------------------------------- */

bool series_mode(void)
{
	return sv_rows() > 0;
}

static size_t series_len(void)
{
	return sv_rows();
}

/* The widest row of the Series pane, in columns. */
static int series_width(void)
{
	size_t k, w = 12, l;

	for (k = 0; k < series_len(); k++) {
		if (ui.sv.own && !k)
			continue;
		else
			l = 8 + strlen(ui.sv.lines[k - (ui.sv.own ? 1 : 0)]);
		if (l > w)
			w = l;
	}
	return (int)w;
}

/*
 * The range, newest first, the work tree (own) and then the commits, with
 * a divider where the upstream starts and M on merges. The cursor is the
 * commit shown.
 */
/*
 * " (origin/main, dev)", local branches green and remote-tracking ones
 * red, as git log colours them. Returns the columns used.
 */
static int draw_branches(int y, int x, int w, const char *refs)
{
	bool first = true;
	int used = 0;
	attr_t row;
	short pair;

	if (w < 4)
		return 0;
	/* the row's attributes (the cursor's) come back after each name */
	attr_get(&row, &pair, NULL);
	used += put_text(y, x, w, " (", 2);
	while (*refs && used < w) {
		size_t n = strcspn(refs, " ");
		bool remote;
		remote = !strncmp(refs, "refs/remotes/", 13);

		size_t skip = remote				? 13 :
			      !strncmp(refs, "refs/heads/", 11) ? 11 :
								  0;

		if (!first)
			used += put_text(y, x + used, w - used, ", ", 2);
		first = false;
		attron(role_attr(remote ? C_DEL : C_ADD) | A_BOLD);
		used += put_text(y, x + used, w - used, refs + skip, n - skip);
		attr_set(row, pair, NULL);
		refs += n;
		while (*refs == ' ')
			refs++;
	}
	if (used < w)
		used += put_text(y, x + used, w - used, ")", 1);
	return used;
}

static void draw_browse_series(int y0, int h, int w)
{
	const struct sview *s = &ui.sv;
	int x0 = ui.lay.list_x;
	size_t n = sv_rows(), off = s->own ? 1 : 0, k;
	/* the divider goes above the first row past the range */
	size_t div = s->bounded && s->inrange < s->n ? s->inrange + off :
						       (size_t)-1;
	int i;

	int smax = 0; /* how far l can scroll the cursor's subject */

	ui.scur = s->sel;
	/* l scrolls the subject under the cursor only */
	if (ui.shscroll_row != ui.scur) {
		ui.shscroll_row = ui.scur;
		ui.shscroll = 0;
	}
	if (ui.scur < ui.stop)
		ui.stop = ui.scur;
	/* the divider takes a line when it is between the top and the cursor */
	while (ui.scur + (div > ui.stop && div <= ui.scur ? 1 : 0) >=
	       ui.stop + (size_t)h)
		ui.stop++;
	for (i = 0, k = ui.stop; i < h && k < n; i++, k++) {
		char buf[512], l[32];

		if (k == div && i + 1 < h) {
			char d[256];

			snprintf(d, sizeof(d), "-- %s --",
				 s->base_name ? s->base_name : "upstream");
			attron(A_DIM);
			fill_line(y0 + i, x0, w);
			puts_at(y0 + i, x0 + 1, w - 1, d);
			attroff(A_DIM);
			i++;
		}
		/*
		 * > is the cursor in the pane and * the row shown, also
		 * without colours
		 */
		char mark = k != ui.scur	  ? ' ' :
			    ui.active == P_SERIES ? '>' :
						    '*';

		if (k == ui.scur)
			attron((ui.active == P_SERIES ? A_REVERSE :
							A_UNDERLINE) |
			       A_BOLD);
		fill_line(y0 + i, x0, w);
		if (s->own && !k) {
			snprintf(buf, sizeof(buf), "%c  work tree", mark);
			puts_at(y0 + i, x0, w, buf);
		} else {
			int need;

			int x = 8;
			const char *line = s->lines[k - off];
			size_t idlen = strcspn(line, " ");

			/* the subject from l's column on, marks where cut */
			sv_label(k - off, l, sizeof(l));
			snprintf(buf, sizeof(buf), "%c%c%-5s ", mark,
				 s->merge[k - off] ? 'M' : ' ', l);
			puts_at(y0 + i, x0, w, buf);
			/* the branches at it after the id, as git log shows them */
			if (s->refs[k - off] &&
			    !(k == ui.scur && ui.shscroll)) {
				x += put_text(y0 + i, x0 + x, w - x, line,
					      idlen);
				x += draw_branches(y0 + i, x0 + x, w - x,
						   s->refs[k - off]);
				line += idlen;
			}
			need = puts_window(y0 + i, x0 + x, w - x, line,
					   k == ui.scur ? ui.shscroll : 0);
			if (k == ui.scur)
				smax = need;
		}
		attroff(A_REVERSE | A_UNDERLINE | A_BOLD);
	}
	ui.shscroll_max = smax;
	if (ui.shscroll > smax)
		ui.shscroll = smax;
	if (i < h && !s->end) {
		/*
		 * more below, loaded when the cursor gets there, past the
		 * range the upstream first
		 */
		char d[256];

		if (s->bounded && s->n == s->inrange)
			snprintf(d, sizeof(d), "-- %s --",
				 s->base_name ? s->base_name : "upstream");
		else
			snprintf(d, sizeof(d), "...");
		attron(A_DIM);
		fill_line(y0 + i, x0, w);
		puts_at(y0 + i, x0 + 1, w - 1, d);
		attroff(A_DIM);
	}
}

static void draw_series(void)
{
	int y0 = 2, h = ui.lay.series_h - 1, w = ui.lay.files_w;

	if (!ui.lay.series_h)
		return;
	draw_pane_frame(P_SERIES, "Series", shown_label());
	draw_browse_series(y0, h, w);
}
