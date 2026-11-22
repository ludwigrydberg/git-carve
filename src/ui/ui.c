// SPDX-License-Identifier: MIT
/* The main loop, movement and key dispatch. */
#include "ui.h"
#include "ui_int.h"
#include "actions.h"
#include "dcache.h"
#include "draw.h"
#include "state.h"
#include "sview.h"
#include "term.h"
#include "../gc.h"
#include "../message.h"
#include "../opstate.h"
#include "../rename.h"
#include "../undo.h"

/* 0 and $ outside a diff pane */
#define LINE_SCROLL_MSG "0 and $ scroll the diff panes"

static bool resize_key(int c);

struct ui ui;

enum msg_kind { MSG_INFO, MSG_MISS, MSG_REFUSE, MSG_WARN };

static void vset_msg(enum msg_kind kind, const char *fmt, va_list ap)
{
	struct strbuf f = STRBUF_INIT;

	/* {action} in the format becomes the key bound to it */
	keys_subst(&ui.keys, fmt, &f, true);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
	vsnprintf(ui.msg.text, sizeof(ui.msg.text), sb_str(&f), ap);
#pragma GCC diagnostic pop
	sb_release(&f);
	ui.msg.err = kind != MSG_INFO;
	ui.msg.box = ui.msg.text[0] && (kind == MSG_REFUSE || kind == MSG_WARN);
	ui.msg.top = 0;
	if (kind == MSG_WARN) {
		snprintf(ui.msg.title, sizeof(ui.msg.title), "Warning");
	} else if (kind == MSG_REFUSE && ui.cur_act != ACT_NONE &&
		   *actions[ui.cur_act].name) {
		char *p;

		/* "Can't split", "Can't stage file" */
		snprintf(ui.msg.title, sizeof(ui.msg.title), "Can't %s",
			 actions[ui.cur_act].name);
		for (p = ui.msg.title; *p; p++)
			if (*p == '-')
				*p = ' ';
	} else {
		snprintf(ui.msg.title, sizeof(ui.msg.title), "Not done");
	}
}

/* with err, an operation refused or failed, shown in a box (msg_box) */
void set_msg(bool err, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vset_msg(err ? MSG_REFUSE : MSG_INFO, fmt, ap);
	va_end(ap);
}

/* A search or a jump that found nothing, on the bottom line as an error. */
void set_miss(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vset_msg(MSG_MISS, fmt, ap);
	va_end(ap);
}

/* A warning, in a box like a refusal's, titled "Warning". */
void set_warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vset_msg(MSG_WARN, fmt, ap);
	va_end(ap);
}

void msg_clear(void)
{
	ui.msg.text[0] = '\0';
	ui.msg.err = false;
	ui.msg.box = false;
}

void msg_repo_err(void)
{
	struct strbuf t = STRBUF_INIT;

	/* {finish} and {abort} in the core's messages become the keys */
	keys_subst(&ui.keys, ui.r->err.len ? ui.r->err.buf : "failed", &t,
		   false);
	if (ui.r->err_detail.len) {
		size_t keep = sizeof(ui.msg.text) - t.len - 16;
		/* what git said (a hook): its end, if it does not all fit */
		const char *d = ui.r->err_detail.buf;

		if (ui.r->err_detail.len > keep) {
			d += ui.r->err_detail.len - keep;
			d += strcspn(d, "\n");
			if (*d)
				d++;
			set_msg(true, "%s\n\n...\n%s", t.buf, d);
		} else {
			set_msg(true, "%s\n\n%s", t.buf, d);
		}
	} else {
		set_msg(true, "%s", t.buf);
	}
	sb_release(&t);
	sb_reset(&ui.r->err);
	sb_reset(&ui.r->err_detail);
}

int one_side(void)
{
	if (ui.active == P_STAGED)
		return 1;
	if (ui.active == P_UNSTAGED)
		return 0;
	/*
	 * the cursor's section decides, and a Staged file shows its staged
	 * diff
	 */
	return ui.fcur < ui.nitems && ui.items[ui.fcur].sec == SEC_STAGED;
}

struct dpane *active_dp(void)
{
	if (ui.active == P_UNSTAGED)
		return &ui.dp[0];
	if (ui.active == P_STAGED)
		return &ui.dp[1];
	return NULL;
}

static void move_files(long delta)
{
	long step = delta > 0 ? 1 : -1;
	long c = (long)ui.fcur;
	long n = labs(delta);

	if (!ui.nitems)
		return;
	while (n--) {
		long nx = c + step;

		while (nx >= 0 && nx < (long)ui.nitems &&
		       !item_stop((size_t)nx))
			nx += step;
		if (nx < 0 || nx >= (long)ui.nitems)
			break;
		c = nx;
	}
	if ((size_t)c != ui.fcur) {
		ui.fcur = (size_t)c;
		select_item();
		ui.dp[0].cur = ui.dp[0].top = 0;
		ui.dp[1].cur = ui.dp[1].top = 0;
		ui.dp[0].ranging = ui.dp[1].ranging = false;
		/* diffs load once the cursor settles (next_key) */
		diffs_stale();
	}
}

static bool cross_file(int dir);

/* A move this far goes to the top or the bottom (gg, G), never further. */
#define TO_END 1000000000L

static void move_cursor(long delta)
{
	struct dpane *dp = active_dp();
	long c;

	if (ui.active == P_SERIES) {
		sv_select((long)ui.sv.sel + delta);
		return;
	}
	if (!dp) {
		move_files(delta);
		return;
	}
	if (!dp->nrows)
		return;
	c = (long)dp->cur + delta;
	if (labs(delta) < TO_END && (c < 0 || c >= (long)dp->nrows) &&
	    cross_file(c < 0 ? -1 : 1))
		return;
	if (c < 0)
		c = 0;
	if (c >= (long)dp->nrows)
		c = (long)dp->nrows - 1;
	dp->cur = (size_t)c;
}

/*
 * The panes Tab goes through, in the order Series, Files, the message and
 * the diffs. Layout one has one diff pane, the one of the cursor's
 * section. Returns how many there are.
 */
static int pane_order(int order[NPANES])
{
	int n = 0;

	if (series_mode() && !ui.series_hidden)
		order[n++] = P_SERIES;
	order[n++] = P_FILES;
	if (ui.lay.msg_on && ui.lay.msg_h)
		order[n++] = P_MESSAGE;
	if (ui.lay.one) {
		order[n++] = one_side() ? P_STAGED : P_UNSTAGED;
	} else {
		order[n++] = P_UNSTAGED;
		order[n++] = P_STAGED;
	}
	return n;
}

/* The pane dir steps away from pane, Tab's for 1 and Shift-Tab's for -1. */
int pane_after(int pane, int dir)
{
	int order[NPANES], n = pane_order(order), i;

	for (i = 0; i < n && order[i] != pane; i++)
		;
	if (i == n)
		i = 0;
	return order[(i + dir + n) % n];
}

/* Zoomed, Tab goes to the next pane and stays zoomed. */
static void next_pane(int dir)
{
	ui.active = pane_after(ui.active, dir);
}

/* Select Files entry k (a file or a fold row) as moving there would. */
static void files_goto(size_t k)
{
	if (!item_stop(k) || k == ui.fcur)
		return;
	move_files((long)k > (long)ui.fcur ? 1 : -1);
	/* move_files steps over headers, go straight to k */
	if (ui.fcur != k) {
		ui.fcur = k;
		select_item();
		ui.dp[0].cur = ui.dp[0].top = 0;
		ui.dp[1].cur = ui.dp[1].top = 0;
		ui.dp[0].ranging = ui.dp[1].ranging = false;
		diffs_stale();
	}
}

/*
 * The nearest Files entry to k the cursor stops on, looking in direction
 * dir.
 */
static size_t files_entry(long k, int dir)
{
	if (k < 0)
		k = 0;
	if (k >= (long)ui.nitems)
		k = (long)ui.nitems - 1;
	while (k >= 0 && k < (long)ui.nitems && !item_stop((size_t)k))
		k += dir;
	if (k < 0 || k >= (long)ui.nitems)
		return ui.fcur;
	return (size_t)k;
}

/*
 * Ctrl-E and Ctrl-Y move the view a line down or up, and the cursor stays
 * in view.
 */
static void scroll_view(int dir)
{
	struct dpane *dp = active_dp();

	if (dp) {
		long h = dp->h > 0 ? dp->h : 1;
		long t = (long)dp->top + dir;
		long max;

		max = (long)dp->nrows > h ? (long)dp->nrows - h : 0;

		if (t < 0)
			t = 0;
		if (t > max)
			t = max;
		dp->top = (size_t)t;
		if (dp->cur < dp->top)
			dp->cur = dp->top;
		if (dp->cur >= dp->top + (size_t)h)
			dp->cur = dp->top + (size_t)h - 1;
	} else {
		long h = ui.lay.files_h > 0 ? ui.lay.files_h : 1;
		long t = (long)ui.ftop + dir;
		long max = (long)ui.nitems > h ? (long)ui.nitems - h : 0;

		if (!ui.nitems)
			return;
		if (t < 0)
			t = 0;
		if (t > max)
			t = max;
		ui.ftop = (size_t)t;
		if (ui.fcur < ui.ftop)
			files_goto(files_entry((long)ui.ftop, 1));
		else if (ui.fcur >= ui.ftop + (size_t)h)
			files_goto(files_entry((long)ui.ftop + h - 1, -1));
	}
}

/*
 * zt, zz and zb put the cursor line at the top, middle or bottom of the
 * view.
 */
static void place_cursor(int where)
{
	struct dpane *dp = active_dp();
	long h = dp ? dp->h : ui.lay.files_h,
	     cur = dp ? (long)dp->cur : (long)ui.fcur;
	long t = cur - (where == 0 ? 0 : where == 1 ? (h - 1) / 2 : h - 1);

	if (t < 0)
		t = 0;
	if (dp)
		dp->top = (size_t)t;
	else
		ui.ftop = (size_t)t;
}

/* H, M and L put the cursor on the top, middle or bottom line of the view. */
static void screen_cursor(int where)
{
	struct dpane *dp = active_dp();
	long top = dp ? (long)dp->top : (long)ui.ftop;
	long n = dp ? (long)dp->nrows : (long)ui.nitems;
	long h = dp ? dp->h : ui.lay.files_h, vis = n - top < h ? n - top : h;
	long k;

	if (vis <= 0)
		return;
	k = top + (where == 0 ? 0 : where == 1 ? (vis - 1) / 2 : vis - 1);
	if (dp)
		dp->cur = (size_t)k;
	else
		files_goto(files_entry(k, where == 2 ? -1 : 1));
}

/* $ scrolls the diff text until the cursor line ends in view. */
static void line_end(void)
{
	struct dpane *dp = active_dp();
	const struct dline *d;
	const struct row *rw;
	int w, text_w;
	size_t k;

	if (!dp || !dp->fd || dp->cur >= dp->nrows) {
		set_msg(false, LINE_SCROLL_MSG);
		return;
	}
	rw = &dp->rows[dp->cur];
	if (rw->kind != R_LINE) {
		dp->hscroll = 0;
		return;
	}
	d = &dp->fd->hunks[rw->h].lines[rw->j];
	w = (int)d->len;
	for (k = 0; k < d->len; k++)
		if (d->text[k] == '\t')
			w += ui.cfg.tabw - 1;
	text_w = dp->w - ROW_TEXT_X;
	dp->hscroll = w > text_w ? w - text_w : 0;
}

/* A hunk header, or in an unmerged file's view a conflict's heading. */
static bool row_is_hunk(const struct dpane *dp, size_t i)
{
	return dp->rows[i].kind == R_HUNK ||
	       (dp->rows[i].kind == R_NOTE && dp->rows[i].j == CROW_HEAD);
}

static char row_op(const struct dpane *dp, size_t i)
{
	const struct row *rw = &dp->rows[i];

	if (rw->kind != R_LINE)
		return 0;
	return dp->fd->hunks[rw->h].lines[rw->j].op;
}

/* First row of a block of changed lines. */
static bool row_starts_change(const struct dpane *dp, size_t i)
{
	char op = row_op(dp, i);

	if (!op || op == ' ')
		return false;
	return !i || !row_op(dp, i - 1) || row_op(dp, i - 1) == ' ' ||
	       dp->rows[i - 1].h != dp->rows[i].h;
}

/*
 * ] [ ) ( move to the next or previous row that want() accepts. In the
 * Files pane they move one file.
 */
static void move_to(int dir, bool (*want)(const struct dpane *, size_t),
		    const char *what)
{
	struct dpane *dp = active_dp();
	long i;

	if (!dp) {
		move_files(dir);
		return;
	}
	for (i = (long)dp->cur + dir; i >= 0 && i < (long)dp->nrows; i += dir) {
		if (want(dp, (size_t)i)) {
			dp->cur = (size_t)i;
			return;
		}
	}
	set_msg(false, "no %s %s", dir > 0 ? "next" : "previous", what);
}

/*
 * } {: from a diff pane, the next/previous file that has something in
 * this pane (its section of the Files list), landing on its first change.
 */
static void move_file(int dir)
{
	enum sec sec = ui.active == P_STAGED ? SEC_STAGED :
		       ui.path_commit	     ? SEC_COMMIT :
					       SEC_UNSTAGED;
	struct dpane *dp;
	long i, from = (long)ui.fcur;

	if (ui.active == P_FILES) {
		move_files(dir);
		return;
	}
	/*
	 * selected in another section (Unstaged, in the Staged pane): from
	 * the file's entry in this one, else from that section's edge
	 */
	if (ui.fcur < ui.nitems && ui.items[ui.fcur].sec != sec) {
		long first = -1, last = -1;

		from = -2;
		for (i = 0; i < (long)ui.nitems; i++) {
			if (ui.items[i].header || ui.items[i].sec != sec)
				continue;
			if (first < 0)
				first = i;
			last = i;
			if (ui.path && !strcmp(item_path((size_t)i), ui.path))
				from = i;
		}
		if (from == -2)
			from = first < 0 ? -1 : dir > 0 ? first - 1 : last + 1;
	}
	for (i = from + dir; i >= 0 && i < (long)ui.nitems; i += dir)
		if (!ui.items[i].header && ui.items[i].sec == sec &&
		    !(ui.path && !strcmp(item_path((size_t)i), ui.path)))
			break;
	if (i < 0 || i >= (long)ui.nitems) {
		set_msg(false, "no %s file with %s changes",
			dir > 0 ? "next" : "previous", sec_name(sec));
		return;
	}
	ui.fcur = (size_t)i;
	select_item();
	ui.dp[0].cur = ui.dp[0].top = ui.dp[1].cur = ui.dp[1].top = 0;
	ui.dp[0].ranging = ui.dp[1].ranging = false;
	diffs_stale();
	diffs_load();
	dp = active_dp();
	for (i = 0; dp && i < (long)dp->nrows; i++) {
		if (row_starts_change(dp, (size_t)i)) {
			dp->cur = (size_t)i;
			break;
		}
	}
}

/*
 * After crossing into the next file, its first row is drawn below the bar
 * that follows the end of the file before, where the cursor was. Into the
 * file before, its last row is drawn above the bar it had.
 */
static void place_crossed(struct dpane *dp, int dir, int row)
{
	if (!dp->nrows)
		return;
	if (dir > 0) {
		dp->cur = dp->top = 0;
		dp->lead = row + 2;
		return;
	}
	dp->cur = dp->nrows - 1;
	dp->lead = 0;
	if (row < 2)
		dp->top = dp->cur;
	else if (dp->cur > (size_t)(row - 2))
		dp->top = dp->cur - (size_t)(row - 2);
	else
		dp->top = 0;
}

/*
 * With all files shown, moving past the end of a file goes on into the
 * next one, and past its start into the end of the one before. Returns
 * false when the cursor stays in the file.
 */
static bool cross_file(int dir)
{
	struct dpane *dp = active_dp();
	char *was;
	int row;

	if (!ui.all_files || ui.path_commit || !dp ||
	    dp->cur != (dir > 0 ? dp->nrows - 1 : 0))
		return false;
	if (dp->ranging) {
		set_msg(false, "a range stays within one file");
		return true;
	}
	/* the screen row of the cursor, which the view keeps */
	row = (int)(dp->cur - dp->top) + dp->lead;
	was = xstrdup(ui.path ? ui.path : "");
	move_file(dir);
	dp = active_dp();
	if (dp && ui.path && strcmp(was, ui.path))
		place_crossed(dp, dir, row);
	free(was);
	return true;
}

/*
 * h and l in the Files or Series pane move their text sideways, half the
 * pane at a time. Files show the end of a path, and h goes towards its
 * start. Series show the start of a subject, and l goes towards its end.
 * Only the cursor's rows move, each as far as its text goes (draw_files,
 * draw_browse_series).
 */
static void list_hscroll(int dir)
{
	int step = ui.lay.files_w > 16 ? ui.lay.files_w / 2 : 8;
	bool series = ui.active == P_SERIES;
	int *off;
	int max;

	off = series ? &ui.shscroll : &ui.fhscroll;
	max = series ? ui.shscroll_max : ui.fhscroll_max;

	*off += (series ? dir : -dir) * step;
	if (*off > max)
		*off = max;
	if (*off < 0)
		*off = 0;
}

/* h and l scroll the line text of the active diff pane sideways. */
static void hscroll(int dir)
{
	struct dpane *dp = active_dp();
	int step, text_w, widest = 0;
	size_t h, j, k;

	if (!dp) {
		list_hscroll(dir);
		return;
	}
	text_w = dp->w - ROW_TEXT_X;
	step = text_w > 16 ? text_w / 2 : 8;
	for (h = 0; dp->fd && h < dp->fd->nhunks; h++) {
		for (j = 0; j < dp->fd->hunks[h].nlines; j++) {
			const struct dline *d = &dp->fd->hunks[h].lines[j];
			int w = (int)d->len;

			/*
			 * bytes, with tabs at full width, are never less
			 * than shown
			 */
			for (k = 0; k < d->len; k++)
				if (d->text[k] == '\t')
					w += ui.cfg.tabw - 1;
			if (w > widest)
				widest = w;
		}
	}
	dp->hscroll += dir * step;
	if (dp->hscroll > widest - text_w)
		dp->hscroll = widest - text_w;
	if (dp->hscroll < 0)
		dp->hscroll = 0;
}

/* Reload both diffs, keeping each cursor on the same diff line. */
static void reload_view(void)
{
	int i;

	ui.hits_valid = false;

	for (i = 0; i < 2; i++) {
		struct dpane *dp = &ui.dp[i];
		int old_no = 0, new_no = 0;
		size_t k;

		if (dp->fd && dp->cur < dp->nrows &&
		    dp->rows[dp->cur].kind == R_LINE) {
			const struct row *rw = &dp->rows[dp->cur];
			const struct dline *d =
				&dp->fd->hunks[rw->h].lines[rw->j];

			old_no = d->old_no;
			new_no = d->new_no;
		}
		dp_load(dp, ui.path, i == 1);
		dp->ranging = false;
		for (k = 0; (old_no || new_no) && dp->fd && k < dp->nrows;
		     k++) {
			const struct row *rw = &dp->rows[k];
			const struct dline *d;

			if (rw->kind != R_LINE)
				continue;
			d = &dp->fd->hunks[rw->h].lines[rw->j];
			if ((new_no && d->new_no == new_no) ||
			    (!new_no && d->old_no == old_no)) {
				dp->cur = k;
				break;
			}
		}
	}
}

/*
 * +, -, W and w set how much context the diffs show and whether whitespace
 * counts.
 */
static void view_option(enum action act)
{
	switch (act) {
	case ACT_MORE_CONTEXT:
		ui.dopts.context += ui.dopts.context < 10 ? 1 : 10;
		break;
	case ACT_LESS_CONTEXT:
		if (ui.dopts.context > 0)
			ui.dopts.context -= ui.dopts.context <= 10 ? 1 : 10;
		break;
	case ACT_FUNC_CONTEXT:
		ui.dopts.function_context = !ui.dopts.function_context;
		break;
	default:
		ui.dopts.ignore_ws = !ui.dopts.ignore_ws;
		break;
	}
	reload_view();
	set_msg(false, "%d context line%s%s%s", ui.dopts.context,
		ui.dopts.context == 1 ? "" : "s",
		ui.dopts.function_context ? ", whole functions" : "",
		ui.dopts.ignore_ws ? ", whitespace ignored (view only)" : "");
}

static int page(void)
{
	struct dpane *dp = active_dp();
	int h;

	h = dp ? dp->h : ui.lay.files_h;

	return h > 2 ? h - 1 : 1;
}

/*
 * The undo entries of line keys are written once the keys pause this long.
 * At typing speed that is one write for several keys, not one per key. Any
 * other command, a child process and quitting write them first.
 */
#define UNDO_PAUSE_MS 1000

static long long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/*
 * Wait for a key. While background work is pending, wake up regularly
 * to collect it and redraw only when something changed.
 */
#define SETTLE_MS 150

/* "idle" or "busy" to GIT_CARVE_IDLE, when that changes */
static void note_idle(bool idle)
{
	FILE *f;

	if (!ui.idle_file || ui.idle == idle)
		return;
	ui.idle = idle;
	f = fopen(ui.idle_file, "a");
	if (f) {
		fputs(idle ? "idle\n" : "busy\n", f);
		fclose(f);
	}
}

/*
 * Before getch() blocks with nothing pending: idle, unless a key is already
 * waiting (keys sent in one write are one step). Then getch() waits again
 * as wait says.
 */
static void note_wait(int wait)
{
	int c;

	if (!ui.idle_file)
		return;
	timeout(0);
	c = getch();
	timeout(wait);
	if (c != ERR)
		ungetch(c);
	else
		note_idle(true);
}

int user_key(void)
{
	int c;

	note_wait(-1);
	c = getch();
	note_idle(false);
	return c;
}

static int next_key(void)
{
	static long long key_at;
	long long undo_wait;
	int c, wait;

	for (;;) {
		undo_wait = !undo_queued(ui.r) ?
				    -1 :
				    key_at + UNDO_PAUSE_MS - now_ms();
		if (undo_queued(ui.r) && undo_wait < 1)
			undo_wait = 1;
		/*
		 * settled when no key came for SETTLE_MS. A held key
		 * repeats faster (macOS's default is 11 a second), and
		 * nothing loads meanwhile.
		 */
		wait = ui.diff_stale || ui.sv.stale	   ? SETTLE_MS :
		       bg_running(&ui.bg) || dcache_busy() ? 50 :
		       undo_wait > 0			   ? (int)undo_wait :
		       dcache_wanted()			   ? 300 :
							     -1;
		timeout(wait);
		/* nothing pending, only a key wakes us */
		if (wait < 0)
			note_wait(wait);
		c = getch();
		if (want_stop) {
			/*
			 * Ctrl-Z gives the terminal back and stops, and fg
			 * draws again
			 */
			want_stop = 0;
			suspend_curses();
			raise(SIGTSTP);
			resume_curses();
			draw();
			if (c == ERR)
				continue;
		}
		if (c != ERR) {
			note_idle(false);
			key_at = now_ms();
			break;
		}
		if (ui.sv.stale) {
			/*
			 * the Series pane cursor has settled, show its
			 * commit
			 */
			sv_show();
			draw();
		}
		if (ui.diff_stale) {
			/* no key for a moment, the cursor has settled */
			diffs_load();
			draw();
		}
		if (undo_queued(ui.r) && now_ms() - key_at >= UNDO_PAUSE_MS) {
			/* the keys paused, write the undo entries kept back */
			if (flush_undo()) {
				set_msg(true, UNDO_FAILED_MSG,
					sb_str(&ui.r->err));
				draw();
			}
		}
		if (bg_running(&ui.bg) && bg_pump(&ui.bg) != 0) {
			scan_done();
			draw();
		}
		/* nothing else to do, fill the diff cache in the background */
		if (!ui.diff_stale && !ui.sv.stale && !undo_queued(ui.r) &&
		    (dcache_busy() || dcache_wanted())) {
			bool was_busy = dcache_busy();

			dcache_idle();
			/* the view of all files shows the diffs it now has */
			if (ui.all_files && was_busy && !dcache_busy())
				draw();
		}
	}
	timeout(-1);
	return c;
}

static void cleanup(void)
{
	size_t i;

	if (undo_flush(ui.r))
		fprintf(stderr, "git-carve: " UNDO_FAILED_MSG "\n",
			sb_str(&ui.r->err));
	bg_stop(&ui.bg);
	dcache_drop();
	free(ui.status_stamp);
	free_status(ui.fs, ui.nfs);
	free_status(ui.tracked, ui.ntracked);
	free_renames(ui.rv, ui.nrv);
	free_status(ui.scan, ui.nscan);
	free(ui.items);
	free(ui.path);
	free(ui.head_oid);
	free(ui.upstream);
	free(ui.scan_ix_stamp);
	free(ui.upstream_for);
	free(ui.branch);
	split_state_release(&ui.split);
	series_state_release(&ui.series);
	cfiles_free(ui.cfiles, ui.ncfiles);
	cfiles_free(ui.sfiles, ui.nsfiles);
	free(ui.sfiles_of);
	free(ui.filter);
	free(ui.ccommit);
	free(ui.cmsg_of);
	sb_release(&ui.cmsg);
	sb_release(&ui.draft);
	free(ui.search);
	for (i = 0; i < ui.cfg.nskip; i++)
		free(ui.cfg.skip[i]);
	free(ui.cfg.skip);
	free(ui.force_path);
	free(ui.stopped_path);
	hits_clear();
	cnums_free();
	sv_release();
	dp_clear(&ui.dp[0]);
	dp_clear(&ui.dp[1]);
	free(ui.dp[0].rows);
	free(ui.dp[1].rows);
	memset(&ui, 0, sizeof(ui));
}

static bool is_move_key(int c)
{
	switch (key_action(&ui.keys, c)) {
	case ACT_DOWN:
	case ACT_UP:
	case ACT_PAGE_DOWN:
	case ACT_PAGE_UP:
	case ACT_TOP:
	case ACT_BOTTOM:
	case ACT_HALF_DOWN:
	case ACT_HALF_UP:
		return true;
	default:
		return c == KEY_DOWN || c == KEY_UP;
	}
}

/* A line or file of a commit of the Series pane. */
static bool browse_commit(void)
{
	return ui.path_commit && ui.ccommit && ui.active != P_STAGED &&
	       ui.active != P_SERIES;
}

/* The lines of a commit shown, not its file list. */
static bool commit_lines(void)
{
	return browse_commit() && ui.active != P_FILES;
}

/*
 * The name an action gets in the undo list (the CLI's command names), or
 * NULL for actions that do not change the index or HEAD.
 */
static const char *undo_op(enum action act)
{
	/* the Series pane only moves */
	if (ui.active == P_SERIES)
		return NULL;
	switch (act) {
	case ACT_REMOVE:
		return "remove";
	case ACT_STAGE:
	case ACT_STAGE_FILE:
		if (browse_commit())
			return "move";
		if (ui.active == P_FILES)
			return ui.fcur < ui.nitems && ui.items[ui.fcur].sec ==
							      SEC_STAGED ?
				       "unstage-file" :
				       "stage-file";
		return ui.active == P_STAGED ? "unstage" : "stage";
	case ACT_DISCARD:
		if (browse_commit())
			return "drop";
		return ui.active == P_FILES ? "discard-file" : "discard";
	case ACT_EDIT:
		return commit_lines() ? "edit" : NULL;
	case ACT_COMMIT:
		return "commit";
	case ACT_AMEND:
		return "commit --amend";
	case ACT_SPLIT:
		return ui.series.op != OP_NONE ? "continue" : "split";
	case ACT_ABSORB:
		return "absorb";
	case ACT_FIXUP:
		return browse_commit() ? "move" : "fixup-to";
	case ACT_FIXUP_COMMIT:
	case ACT_AMEND_COMMIT:
		return "fixup";
	case ACT_SQUASH:
		return "squash";
	case ACT_REWORD:
		return "reword";
	case ACT_ABORT:
		return "abort";
	default:
		return NULL;
	}
}

/*
 * Whether Space in diff pane dp only applies a patch of the selected
 * lines. It does not for a new file, which is made intent-to-add first or
 * put back that way when unstaged, a rename with its move, or a whole-file
 * operation.
 */
static bool pure_line_op(const struct dpane *dp)
{
	const struct file_status *f = cur_fs();

	if (!dp || !dp->fd || dp->too_big || dp->commit || ui.path_commit ||
	    !fdiff_line_ops(dp->fd, NULL) || !f || f->unmerged)
		return false;
	if (dp == &ui.dp[0])
		return !ui.path_untracked && f->y != '?' && !f->from_y;
	return f->x != 'A' && !f->from_x;
}

/* ---- what each action does (handle_key runs them) -------------------- */

static void do_split(void)
{
	/*
	 * s starts a split, and ZZ finishes it and whatever else is in
	 * progress
	 */
	if (ui.split.active)
		set_msg(true, "a split is in progress; {finish} finishes it, "
			      "{abort} gives it up");
	else if (ui.series.op != OP_NONE)
		set_msg(true, OP_BUSY_MSG);
	else
		act_split();
}

static void do_series_pane(void)
{
	ui.series_hidden = !ui.series_hidden;
	if (ui.series_hidden && ui.active == P_SERIES)
		ui.active = P_FILES;
	set_msg(false, ui.series_hidden ?
			       "Series pane hidden: "
			       "{series-pane} shows it, {next-commit} / "
			       "{prev-commit} still move" :
			       "Series pane shown");
}

static void do_message_pane(void)
{
	ui.lay.msg_on = !ui.lay.msg_on;
	if (!ui.lay.msg_on && ui.active == P_MESSAGE)
		ui.active = P_FILES;
	set_msg(false, ui.lay.msg_on ? "message pane shown" :
				       "message pane hidden: {message-pane} "
				       "shows it");
}

/*
 * A key in the message pane. The movement keys scroll it and e writes the
 * message. True when it was one of those, or one that does nothing here,
 * which is then said. The keys that work anywhere go on.
 */
static bool message_key(enum action act)
{
	const struct strbuf *m = pane_message();
	size_t n = count_lines(sb_str(m), m->len);
	size_t last = n > (size_t)ui.lay.msg_h ? n - (size_t)ui.lay.msg_h : 0;

	switch (act) {
	case ACT_DOWN:
	case ACT_PAGE_DOWN:
	case ACT_HALF_DOWN:
	case ACT_BOTTOM:
		ui.draft_top = act == ACT_DOWN && ui.draft_top < last ?
				       ui.draft_top + 1 :
				       last;
		return true;
	case ACT_UP:
	case ACT_PAGE_UP:
	case ACT_HALF_UP:
	case ACT_TOP:
		ui.draft_top =
			act == ACT_UP && ui.draft_top ? ui.draft_top - 1 : 0;
		return true;
	case ACT_EDIT:
		/* for a commit shown, a new message for it, as r gives */
		if (ui.ccommit)
			act_reword();
		else
			act_message_edit();
		return true;
	case ACT_NEXT_PANE:
	case ACT_PREV_PANE:
	case ACT_COMMIT:
	case ACT_AMEND:
	case ACT_QUIT:
	case ACT_HELP:
	case ACT_REFRESH:
	case ACT_REPAINT:
	case ACT_SIGNOFF:
	case ACT_UNDO:
	case ACT_SPLIT:
	case ACT_FINISH:
	case ACT_ABORT:
	case ACT_MESSAGE_PANE:
	case ACT_SERIES_PANE:
	case ACT_NEXT_COMMIT:
	case ACT_PREV_COMMIT:
	case ACT_SHOW_COMMIT:
	case ACT_RESIZE:
	case ACT_CLEAR:
	case ACT_NONE:
		return false;
	default:
		set_msg(false, "not in the message pane: there {edit} edits "
			       "the message and the movement keys scroll it");
		return true;
	}
}

static void do_resize(void)
{
	ui.lay.resizing = true;
	ui.lay.zoom = false;
}

static void do_zoom(void)
{
	/* the active pane alone on the screen, or back */
	ui.lay.zoom = !ui.lay.zoom;
	if (ui.lay.zoom)
		set_msg(false, "zoomed in: {zoom} zooms out, "
			       "{next-pane} goes to the next pane");
}

static void do_all_files(void)
{
	ui.all_files = !ui.all_files;
	set_msg(false, ui.all_files ?
			       "all files: moving past the end of a file "
			       "goes on into the next" :
			       "one file at a time");
}

static void do_abort(void)
{
	if (ui.series.op != OP_NONE)
		act_series_abort();
	else
		act_abort();
}

static void do_fixup(void)
{
	if (browse_commit())
		act_commit_move_to();
	else
		act_fixup();
}

static void do_fixup_commit(void)
{
	act_fixup_commit(false);
}

static void do_amend_commit(void)
{
	act_fixup_commit(true);
}

static void do_pane(void)
{
	next_pane(ui.cur_act == ACT_NEXT_PANE ? 1 : -1);
}

static void do_down(void)
{
	move_cursor(1);
}

static void do_up(void)
{
	move_cursor(-1);
}

static void do_page_down(void)
{
	move_cursor(page());
}

static void do_page_up(void)
{
	move_cursor(-page());
}

static void do_half_down(void)
{
	move_cursor(page() / 2 ? page() / 2 : 1);
}

static void do_half_up(void)
{
	move_cursor(-(page() / 2 ? page() / 2 : 1));
}

static void do_top(void)
{
	size_t k;

	move_cursor(-TO_END);
	/* in Files, the first file, and k reaches the section's row */
	if (ui.active == P_FILES && item_is_section(ui.fcur))
		for (k = ui.fcur; k < ui.nitems; k++)
			if (!ui.items[k].header) {
				files_goto(k);
				break;
			}
}

static void do_bottom(void)
{
	move_cursor(TO_END);
}

static void do_view_option(void)
{
	view_option(ui.cur_act);
}

static void do_scroll_left(void)
{
	hscroll(-1);
}

static void do_scroll_right(void)
{
	hscroll(1);
}

static void do_line_down(void)
{
	scroll_view(1);
}

static void do_line_up(void)
{
	scroll_view(-1);
}

static void do_scroll_top(void)
{
	place_cursor(0);
}

static void do_scroll_center(void)
{
	place_cursor(1);
}

static void do_scroll_bottom(void)
{
	place_cursor(2);
}

static void do_screen_top(void)
{
	screen_cursor(0);
}

static void do_screen_middle(void)
{
	screen_cursor(1);
}

static void do_screen_bottom(void)
{
	screen_cursor(2);
}

static void do_line_start(void)
{
	struct dpane *dp = active_dp();

	if (dp)
		dp->hscroll = 0;
	else
		set_msg(false, LINE_SCROLL_MSG);
}

static void do_next_hunk(void)
{
	move_to(1, row_is_hunk, "hunk");
}

static void do_prev_hunk(void)
{
	move_to(-1, row_is_hunk, "hunk");
}

static void do_next_change(void)
{
	move_to(1, row_starts_change, "change");
}

static void do_prev_change(void)
{
	move_to(-1, row_starts_change, "change");
}

static void do_next_file(void)
{
	move_file(1);
}

static void do_prev_file(void)
{
	move_file(-1);
}

static void do_stage(void)
{
	if (browse_commit())
		act_commit_out();
	else
		act_space();
}

static void do_stage_file(void)
{
	if (ui.active == P_SERIES)
		series_goto();
	else if (browse_commit())
		act_commit_out();
	else if (ui.active == P_FILES && ui.fcur < ui.nitems)
		act_file_toggle(ui.items[ui.fcur].sec == SEC_STAGED);
	else
		act_space();
}

static void do_range(void)
{
	struct dpane *dp = active_dp();

	if (dp) {
		dp->ranging = !dp->ranging;
		dp->anchor = dp->cur;
	} else if (ui.active == P_FILES && !ui.path_commit) {
		/* for a range of files Enter and Space take them all */
		ui.franging = !ui.franging;
		ui.fanchor = ui.fcur;
	}
}

static void do_clear(void)
{
	struct dpane *dp = active_dp();
	size_t i;

	/* in the Files pane, its range of files, else its filter */
	if (ui.active == P_FILES && ui.franging) {
		ui.franging = false;
	} else if (ui.active == P_FILES && ui.filter) {
		set_filter(NULL);
		set_msg(false, "filter cleared");
	}
	if (dp) {
		dp->ranging = false;
		for (i = 0; i < dp->nrows; i++)
			dp->rows[i].marked = false;
		dp->nmarked = 0;
	}
}

static void do_search(void)
{
	/* the Files pane filters its list and the diffs search */
	if (ui.active == P_FILES)
		act_filter();
	else
		act_search();
}

static void do_search_next(void)
{
	search_next(1);
}

static void do_search_prev(void)
{
	search_next(-1);
}

/*
 * e edits what the cursor is on. In the Series pane that is a commit's
 * message, or on the work tree row the next commit's.
 */
static void do_edit(void)
{
	if (ui.active == P_SERIES && ui.ccommit)
		act_reword();
	else if (ui.active == P_SERIES)
		act_message_edit();
	else if (commit_lines())
		act_commit_edit();
	else
		act_open_file();
}

static void do_ours(void)
{
	act_resolve(false);
}

static void do_theirs(void)
{
	act_resolve(true);
}

static void do_discard(void)
{
	if (browse_commit())
		act_commit_drop();
	else
		act_discard();
}

static void do_commit(void)
{
	act_commit(false);
}

static void do_amend(void)
{
	act_commit(true);
}

static void do_refresh(void)
{
	/* everything anew, the configuration too */
	reload_full();
	set_msg(false, "refreshed");
}

static void do_signoff(void)
{
	ui.signoff = !ui.signoff;
	set_msg(false, "sign-off %s",
		ui.signoff ? "on: commits get "
			     "Signed-off-by" :
			     "off");
}

static void do_repaint(void)
{
	/* the screen and what it shows, both anew */
	reload_full();
	clearok(curscr, TRUE);
}

static bool handle_action(enum action act, int c);

/* The help, and then the action chosen in it as if its key was pressed. */
static void act_help(void)
{
	enum action act = help_pick();

	ui.cur_act = act;
	if (act != ACT_NONE && act != ACT_HELP && !handle_action(act, 0))
		ui.help_quit = true;
}

static void (*const action_fn[NACTIONS])(void) = {
	[ACT_SPLIT] = do_split,
	[ACT_SERIES_PANE] = do_series_pane,
	[ACT_RESIZE] = do_resize,
	[ACT_MESSAGE_PANE] = do_message_pane,
	[ACT_ZOOM] = do_zoom,
	[ACT_ALL_FILES] = do_all_files,
	[ACT_NEXT_COMMIT] = act_next_commit,
	[ACT_PREV_COMMIT] = act_prev_commit,
	[ACT_ABORT] = do_abort,
	[ACT_SHOW_COMMIT] = act_show_commit,
	[ACT_FINISH] = act_finish,
	[ACT_ABSORB] = act_absorb,
	[ACT_FIXUP] = do_fixup,
	[ACT_FIXUP_COMMIT] = do_fixup_commit,
	[ACT_AMEND_COMMIT] = do_amend_commit,
	[ACT_SQUASH] = act_squash,
	[ACT_NEXT_PANE] = do_pane,
	[ACT_PREV_PANE] = do_pane,
	[ACT_DOWN] = do_down,
	[ACT_UP] = do_up,
	[ACT_PAGE_DOWN] = do_page_down,
	[ACT_PAGE_UP] = do_page_up,
	[ACT_TOP] = do_top,
	[ACT_BOTTOM] = do_bottom,
	[ACT_MORE_CONTEXT] = do_view_option,
	[ACT_LESS_CONTEXT] = do_view_option,
	[ACT_FUNC_CONTEXT] = do_view_option,
	[ACT_IGNORE_WS] = do_view_option,
	[ACT_SCROLL_LEFT] = do_scroll_left,
	[ACT_SCROLL_RIGHT] = do_scroll_right,
	[ACT_HALF_DOWN] = do_half_down,
	[ACT_HALF_UP] = do_half_up,
	[ACT_LINE_DOWN] = do_line_down,
	[ACT_LINE_UP] = do_line_up,
	[ACT_SCROLL_TOP] = do_scroll_top,
	[ACT_SCROLL_CENTER] = do_scroll_center,
	[ACT_SCROLL_BOTTOM] = do_scroll_bottom,
	[ACT_SCREEN_TOP] = do_screen_top,
	[ACT_SCREEN_MIDDLE] = do_screen_middle,
	[ACT_SCREEN_BOTTOM] = do_screen_bottom,
	[ACT_LINE_START] = do_line_start,
	[ACT_LINE_END] = line_end,
	[ACT_NEXT_HUNK] = do_next_hunk,
	[ACT_PREV_HUNK] = do_prev_hunk,
	[ACT_NEXT_CHANGE] = do_next_change,
	[ACT_PREV_CHANGE] = do_prev_change,
	[ACT_NEXT_FILE] = do_next_file,
	[ACT_PREV_FILE] = do_prev_file,
	[ACT_STAGE] = do_stage,
	[ACT_STAGE_FILE] = do_stage_file,
	[ACT_RANGE] = do_range,
	[ACT_MARK] = act_mark,
	[ACT_MARK_PAIR] = act_mark_pair,
	[ACT_REMOVE] = act_remove,
	[ACT_SELECT_HUNK] = act_select_hunk,
	[ACT_CLEAR] = do_clear,
	[ACT_GOTO] = act_goto,
	[ACT_SEARCH] = do_search,
	[ACT_SEARCH_NEXT] = do_search_next,
	[ACT_SEARCH_LIST] = act_search_list,
	[ACT_SEARCH_PREV] = do_search_prev,
	[ACT_EDIT] = do_edit,
	[ACT_OURS] = do_ours,
	[ACT_THEIRS] = do_theirs,
	[ACT_DISCARD] = do_discard,
	[ACT_COMMIT] = do_commit,
	[ACT_AMEND] = do_amend,
	[ACT_LOAD_DIFF] = act_load_diff,
	[ACT_IGNORE] = act_ignore,
	[ACT_REFRESH] = do_refresh,
	[ACT_SIGNOFF] = do_signoff,
	[ACT_UNDO] = act_undo,
	[ACT_REPAINT] = do_repaint,
	[ACT_REWORD] = act_reword,
	[ACT_HELP] = act_help,
};

static bool handle_key(int c)
{
	enum action act;

	if (c == KEY_RESIZE)
		return true;
	if (ui.lay.resizing && resize_key(c))
		return true;
	/*
	 * The key that stopped a load is read after it. Esc was only to
	 * stop it, not to clear the filter, the range or the marks.
	 */
	if (ui.stopped_by_key) {
		ui.stopped_by_key = false;
		if (c == 27)
			return true;
	}
	/* moving through the series (J, K, in its pane) leaves it for later */
	if (ui.sv.stale && !(ui.active == P_SERIES && is_move_key(c)) &&
	    key_action(&ui.keys, c) != ACT_NEXT_COMMIT &&
	    key_action(&ui.keys, c) != ACT_PREV_COMMIT)
		sv_show();
	/*
	 * moving through the files or the series loads the diffs once it
	 * settles
	 */
	if (ui.diff_stale &&
	    !((ui.active == P_FILES || ui.active == P_SERIES) &&
	      is_move_key(c)))
		diffs_load();
	act = key_action(&ui.keys, c);
	ui.cur_act = act;
	if (act == ACT_PREFIX) {
		/* the second key of a sequence such as gg */
		struct strbuf hint = STRBUF_INIT;
		char first[16];
		int c2;

		snprintf(first, sizeof(first), "%s", key_label(c));
		keymap_seq_hint(&ui.keys, c, &hint);
		set_msg(false, "%s", hint.buf);
		sb_release(&hint);
		draw();
		c2 = next_key();
		set_msg(false, "%s", "");
		if (c2 == 27 || c2 == KEY_RESIZE)
			return true;
		act = key_seq_action(&ui.keys, c, c2);
		ui.cur_act = act;
		if (act == ACT_NONE) {
			set_miss("no key %s%s", first, key_label(c2));
			return true;
		}
	}
	return handle_action(act, c) && !ui.help_quit;
}

/*
 * Do what an action does, with its undo entry. c is the key pressed for
 * it, or 0. Returns false to quit.
 */
/*
 * Whether an action is answered before it runs, with a message or by the
 * Untracked row folding. Returns true when it is done with.
 */
static bool action_intercepted(enum action act, const struct dpane *dp)
{
	if (ui.active == P_MESSAGE && message_key(act))
		return true;
	/*
	 * a view that ignores whitespace cannot be applied, no line
	 * operations
	 */
	if (dp && dp->fd && dp->fd->view_only &&
	    (act == ACT_STAGE || act == ACT_STAGE_FILE || act == ACT_DISCARD ||
	     act == ACT_FIXUP || (act == ACT_EDIT && commit_lines()))) {
		set_msg(true,
			"whitespace is ignored in this view; {ignore-whitespace} turns line "
			"operations back on");
		return true;
	}
	/*
	 * On a commit of the Series pane its lines move (Space, d, f), and
	 * the rest has nothing to do there.
	 */
	if (ui.path_commit && ui.active != P_STAGED && ui.active != P_SERIES &&
	    (act == ACT_OURS || act == ACT_THEIRS)) {
		set_msg(true,
			"on a commit's lines: {stage} takes them out to the "
			"work tree, {discard} drops them, {fixup-to} moves "
			"them, {edit} edits them");
		return true;
	}
	/*
	 * on the Untracked section's row Enter and l unfold it and h folds
	 * it, while Space stages all of it
	 */
	if (ui.active == P_FILES && item_is_fold(ui.fcur) && !ui.franging &&
	    (act == ACT_STAGE_FILE || act == ACT_SCROLL_RIGHT ||
	     act == ACT_SCROLL_LEFT)) {
		fold_untracked(act == ACT_SCROLL_RIGHT ? 1 :
			       act == ACT_SCROLL_LEFT  ? -1 :
							 0);
		return true;
	}
	return false;
}

static bool handle_action(enum action act, int c)
{
	struct dpane *dp = active_dp();
	struct undo_state ub = { 0 };
	bool quit = false;
	char *what = NULL;
	const char *op;

	if (action_intercepted(act, dp))
		return true;
	op = undo_op(act);
	if (op && undo_kind(op) == UNDO_NONE)
		op = NULL;
	if (op) {
		/*
		 * a line stage or unstage and nothing else is kept as its
		 * patch
		 */
		ub.as_patch = act == ACT_STAGE && pure_line_op(dp);
		undo_begin(ui.r, &ub, op);
		what = xstrdup(ui.path ? ui.path : "");
		/* it changes the work tree, save the file first */
		if (undo_kind(op) == UNDO_WORKTREE && ui.path) {
			const struct file_status *f = cur_fs();
			const char *p[2] = { ui.path, f ? f->from_y : NULL };

			if (undo_capture_files(ui.r, &ub, p, p[1] ? 2 : 1,
					       true)) {
				msg_repo_err();
				undo_state_release(&ub);
				free(what);
				return true;
			}
		}
	}
	if (act == ACT_QUIT) {
		quit = !((ui.split.active || ui.series.op != OP_NONE) &&
			 !confirm("An operation is in progress; quit anyway? "
				  "(it stays in progress) [y/N] "));
	} else if (act < NACTIONS && action_fn[act]) {
		action_fn[act]();
	} else if (c == KEY_DOWN || c == KEY_UP) {
		/* arrows always move, whatever the key map says */
		move_cursor(c == KEY_DOWN ? 1 : -1);
	}
	if (op) {
		bool st = status_still();

		undo_end(ui.r, &ub, op, what);
		free(what);
		show_warnings();
		/* undo's write-tree saves the index's tree cache, no change */
		if (st)
			status_restamp();
	}
	if (ui.dc_vouched)
		dcache_restamp();
	ui.dc_vouched = false;
	return !quit;
}

/*
 * The key map, the defaults plus carve.keys.* from git config. Problems go
 * to warn, one line each.
 */
static void load_keys(struct repo *r, struct keymap *km, struct strbuf *warn)
{
	struct strbuf cfg = STRBUF_INIT;

	keymap_defaults(km);
	repo_config_all(r, "carve.keys.", false, &cfg);
	keymap_apply(km, sb_str(&cfg), warn);
	sb_release(&cfg);
}

int ui_print_keys(struct repo *r)
{
	struct strbuf warn = STRBUF_INIT, keys = STRBUF_INIT,
		      help = STRBUF_INIT;
	struct keymap km;
	int a;

	load_keys(r, &km, &warn);
	printf("%-20s %-18s %s\n", "ACTION", "KEYS", "DESCRIPTION");
	for (a = 1; a < NACTIONS; a++) {
		if (!*actions[a].name)
			continue; /* ACT_PREFIX */
		sb_reset(&keys);
		sb_reset(&help);
		keymap_describe(&km, (enum action)a, &keys);
		keys_subst(&km, actions[a].help, &help, false);
		printf("%-20s %-18s %s\n", actions[a].name, keys.buf,
		       sb_str(&help));
	}
	printf("\nChange a binding with: git config carve.keys.<action> "
	       "'<key> [<key>...]'\n(keys like x, Space, Enter, Tab, Esc, "
	       "Ctrl-X, F5, PgDn, Up, sequences like gg; 'none' unbinds)\n");
	if (warn.len)
		fprintf(stderr, "%s", warn.buf);
	sb_release(&warn);
	sb_release(&keys);
	sb_release(&help);
	return 0;
}

/*
 * carve.skipDiff, patterns of files whose diff is not loaded. Unset, it is
 * the tag files editors and code browsers generate. An empty value clears
 * the list up to there, as for git's options with several values.
 */
static void load_skip(struct repo *r)
{
	static const char *const defaults[] = {
		"tags",	 "TAGS",   "cscope.out", "cscope.*.out",
		"GTAGS", "GRTAGS", "GPATH",
	};
	struct strbuf out = STRBUF_INIT;
	const char *pos, *end, *l;
	size_t len, alloc = 0, i;

	for (i = 0; i < ui.cfg.nskip; i++)
		free(ui.cfg.skip[i]);
	free(ui.cfg.skip);
	ui.cfg.skip = NULL;
	ui.cfg.nskip = 0;
	if (!repo_config_all(r, "carve.skipDiff", true, &out)) {
		for (i = 0; i < ARRAY_SIZE(defaults); i++) {
			ALLOC_GROW(ui.cfg.skip, ui.cfg.nskip + 1, alloc);
			ui.cfg.skip[ui.cfg.nskip++] = xstrdup(defaults[i]);
		}
		return;
	}
	pos = sb_str(&out);
	end = pos + out.len;
	while ((l = next_line(&pos, end, &len))) {
		if (!len) {
			while (ui.cfg.nskip)
				free(ui.cfg.skip[--ui.cfg.nskip]);
			continue;
		}
		ALLOC_GROW(ui.cfg.skip, ui.cfg.nskip + 1, alloc);
		ui.cfg.skip[ui.cfg.nskip++] = xstrndup(l, len);
	}
	sb_release(&out);
}

/* The configuration of what is shown, read at the start and again on F5. */
void read_view_config(void)
{
	struct repo *r = ui.r;
	const char *ws, *u;

	ui.cfg.tabw = (int)repo_config_int(r, "carve.tabWidth", 8);
	if (ui.cfg.tabw < 1 || ui.cfg.tabw > 16)
		ui.cfg.tabw = 8;
	ui.cfg.diff_limit = repo_config_int(r, "carve.diffLimit", 100000);
	ui.cfg.max_file_size =
		repo_config_int(r, "carve.maxFileSize", 5L << 20);
	load_skip(r);
	/* git's defaults, and "-name" turns one off, as in git */
	ws = repo_config(r, "core.whitespace");
	ui.cfg.ws_trailing = !(ws && (strstr(ws, "-trailing-space") ||
				      strstr(ws, "-blank-at-eol")));
	ui.cfg.ws_space_tab = !(ws && strstr(ws, "-space-before-tab"));
	ui.cfg.ascii_lines = repo_config_bool(r, "carve.asciiLines", false);
	u = repo_config(r, "carve.colors");
	ui.cfg.colors256 = u && !strcmp(u, "256");
	ui.lay.narrow_width = (int)repo_config_int(r, "carve.narrowWidth", 100);
	u = repo_config(r, "carve.layout");
	ui.lay.one = !u || strcmp(u, "two");
	/* async, the default, scans in the background, and no never scans */
	u = repo_config(r, "carve.untrackedFiles");
	ui.untracked_async = !u || strcmp(u, "no");
}

/*
 * After Ctrl-W the movement keys move the dividers and the pane keys pick
 * the pane whose divider moves. = puts the default layout back. Enter, Esc
 * or Ctrl-W again end it, and any other key ends it and does its own work.
 */
static bool resize_key(int c)
{
	bool lists = ui.active == P_SERIES || ui.active == P_FILES;
	enum action act = key_action(&ui.keys, c);
	int d = 0;

	if (act == ACT_NEXT_PANE || act == ACT_PREV_PANE)
		return false;
	if (c == '\n' || c == '\r' || c == KEY_ENTER || c == 27 ||
	    act == ACT_RESIZE || act == ACT_QUIT) {
		ui.lay.resizing = false;
		return true;
	}
	if (c == '=') {
		ui.lay.want_left_w = ui.lay.want_series_h = 0;
		ui.lay.want_list_h = ui.lay.want_top_h = 0;
		set_msg(false, "the default layout");
		return true;
	}
	if (act == ACT_SCROLL_LEFT || act == ACT_SCROLL_RIGHT) {
		if (ui.lay.stacked) {
			set_msg(false, "the panes are stacked: {down} / {up} "
				       "move the dividers");
			return true;
		}
		/* the frames' width without the column they share */
		ui.lay.want_left_w = ui.lay.box[P_FILES].w - 1 +
				     (act == ACT_SCROLL_RIGHT ? 2 : -2);
		return true;
	}
	if (act == ACT_DOWN)
		d = 1;
	else if (act == ACT_UP)
		d = -1;
	if (!d) {
		/* not a resize key, back to work */
		ui.lay.resizing = false;
		return false;
	}
	if (!lists) {
		int top = ui.lay.box[P_UNSTAGED].h;

		/* the rows below the frame's top line */
		ui.lay.want_top_h = (top ? top - 1 : 0) + d;
	} else if (ui.active == P_SERIES || !ui.lay.stacked) {
		if (!ui.lay.series_h) {
			set_msg(false, "the Series pane is hidden");
			return true;
		}
		ui.lay.want_series_h = ui.lay.box[P_SERIES].h + d;
	} else {
		/*
		 * stacked, Files moves the divider between the lists and
		 * the diffs
		 */
		ui.lay.want_list_h =
			ui.lay.box[P_SERIES].h + ui.lay.box[P_FILES].h + d;
	}
	return true;
}

/*
 * At startup, what an interrupted operation left behind is cleaned up
 * after asking (carve_gc), and a kept snapshot of the work tree is named.
 */
static void offer_cleanup(void)
{
	struct strbuf out = STRBUF_INIT;

	if (gc_leftovers(ui.r)) {
		draw();
		if (confirm("An interrupted operation left files behind. Clean "
			    "them up? [y/N] ")) {
			if (carve_gc(ui.r, true, &out))
				msg_repo_err();
			else
				set_msg(false, "cleaned up");
			reload();
		}
	}
	if (gc_snapshot_kept(ui.r))
		set_warn("an interrupted move kept a snapshot of your work "
			 "tree: git stash apply " WT_REF " brings it back, "
			 "git update-ref -d " WT_REF " drops it");
	sb_release(&out);
}

/*
 * The gits of the first screen that need no answer of another run side by
 * side with git status, the slowest of them.
 */
static void prefetch_first_screen(struct repo *r)
{
	struct argv a = ARGV_INIT;

	status_args(&a, false);
	repo_prefetch(r, &a);
	argv_clear(&a);
	repo_prefetch_startup(r);
}

int ui_main(struct repo *r, const char *range)
{
	int c;

	memset(&ui, 0, sizeof(ui));
	ui.r = r;
	ui.idle_file = getenv("GIT_CARVE_IDLE");
	if (ui.idle_file && !*ui.idle_file)
		ui.idle_file = NULL;
	/*
	 * the undo entries of stage and unstage are written when the keys
	 * pause
	 */
	r->undo_defer = true;
	prefetch_first_screen(r);
	/*
	 * the Series pane shows your series, and a bad range is reported
	 * first. The title comes first, since it knows the branch the
	 * upstream belongs to.
	 */
	load_head();
	if (sv_open(r, range))
		return -1;
	ui.sv.fresh = true; /* no second look at the tip on the first reload */
	if (!isatty(0) || !isatty(1)) {
		sv_release();
		return repo_err(r, "git carve needs a terminal");
	}
	setlocale(LC_ALL, "");
	r->tty = true;
	ui.cfg.utf8 = !strcmp(nl_langinfo(CODESET), "UTF-8");
	read_view_config();
	ui.signoff = config_signoff(r);
	ui.dopts = (struct diff_opts)DIFF_OPTS_INIT;
	/*
	 * before initscr, which keeps ncurses from installing its own
	 * handler
	 */
	set_tstp(true);
	if (!initscr())
		return repo_err(r, "cannot initialize the terminal");
	cbreak();
	/*
	 * no XON/XOFF flow control, which frees Ctrl-S and Ctrl-Q for
	 * bindings
	 */
	{
		struct termios t;

		if (!tcgetattr(0, &t)) {
			t.c_iflag &= ~(tcflag_t)IXON;
#ifdef VDSUSP
			/*
			 * Ctrl-Y is "delayed suspend" on BSD and macOS,
			 * and a key here
			 */
			t.c_cc[VDSUSP] = _POSIX_VDISABLE;
#endif
			tcsetattr(0, TCSANOW, &t);
			def_prog_mode();
		}
	}
	noecho();
	nonl();
	keypad(stdscr, TRUE);
	curs_set(0);
	/*
	 * scroll with insert and delete line where the terminal can, for
	 * less output
	 */
	idlok(stdscr, TRUE);
#ifdef NCURSES_VERSION
	/*
	 * How long Esc waits for the rest of a key such as an arrow, which a
	 * slow link may split. ESCDELAY in the environment wins, as in any
	 * ncurses program.
	 */
	if (!getenv("ESCDELAY")) {
		long ms = repo_config_int(r, "carve.escDelay", 100);

		set_escdelay(ms < 0 ? 0 : ms > 1000 ? 1000 : (int)ms);
	}
#endif
	ui.bg.fd = -1;
	ui.series_hidden = !repo_config_bool(r, "carve.seriesPane", true);
	ui.lay.msg_on = repo_config_bool(r, "carve.messagePane", true);
	{
		struct strbuf warn = STRBUF_INIT;

		load_keys(r, &ui.keys, &warn);
		if (warn.len)
			set_warn("key bindings: %s(see git carve keys)",
				 warn.buf);
		sb_release(&warn);
	}
	init_colors();
	ui.active = P_FILES;
	reload();
	repo_prefetch_drop(r);
	offer_cleanup();
	for (;;) {
		draw();
		c = next_key();
		if (c == ERR)
			continue;
		/*
		 * while a box is shown, the key scrolls or closes it and
		 * does nothing else
		 */
		if (ui.msg.box && c != KEY_RESIZE) {
			if (!msg_box_key(c))
				msg_clear();
			continue;
		}
		msg_clear();
		/* while too small, only quitting and resizing make sense */
		if (too_small() && key_action(&ui.keys, c) != ACT_QUIT)
			continue;
		if (!handle_key(c))
			break;
		ui.cur_act = ACT_NONE;
	}
	endwin();
	cleanup();
	return 0;
}
