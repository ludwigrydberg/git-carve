/* SPDX-License-Identifier: MIT */
/* Internal to the TUI, the state and functions src/ui/ shares. */
#ifndef CARVE_UI_INT_H
#define CARVE_UI_INT_H

#include <ctype.h>
/* the wide API, for the heavy lines of the active pane's frame */
#define NCURSES_WIDECHAR 1
#include <curses.h>
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "../diff.h"
#include "../git.h"
#include "../series.h"
#include "../split.h"
#include "../status.h"
#include "keys.h"
#include "../cdiff.h"

/* P_SERIES is the Series pane, above the Files pane */
enum { P_FILES, P_UNSTAGED, P_STAGED, P_SERIES, P_MESSAGE, NPANES };

enum {
	C_ADD = 1,
	C_DEL,
	C_HUNK,
	C_SEL,
	C_TITLE,
	C_TITLE_ACT,
	C_ERR,
	C_DIM,
	C_STATUS,
	C_WS,
	C_ID, /* a commit id, yellow as git has it */
	C_FRAME,
	C_FRAME_ACT, /* the last role */
};

enum row_kind { R_NOTE, R_HUNK, R_LINE };

/* The sections of the Files list (and what a search goes through). */
enum sec { SEC_UNSTAGED, SEC_STAGED, SEC_COMMIT };

/* The style of an R_NOTE row (its j). */
enum { CROW_NOTE, CROW_HEAD, CROW_OURS, CROW_BASE, CROW_THEIRS };

struct row {
	enum row_kind kind;
	size_t h, j;
	const char *note;
	bool marked; /* marked with m, part of a selection of lines */
};

struct dpane {
	struct fdiff *fd;
	bool cached;
	struct row *rows;
	size_t nrows, arows;
	size_t cur, top;
	bool ranging;
	size_t anchor;
	size_t nmarked;
	int hscroll; /* columns of line text scrolled off with h and l */
	/*
	 * not loaded, being too big (carve.diffLimit, carve.maxFileSize)
	 * or skipped (carve.skipDiff), which allows only whole-file
	 * operations
	 */
	bool too_big;
	bool cancelled; /* its loading was stopped by a key */
	bool commit;	/* the diff of the commit shown */
	/*
	 * The status letter the loaded diff stands for (' ' no changes, 'A',
	 * 'D', 'M'), 0 when it does not tell (not loaded, a rename, ...)
	 */
	char letter;
	char big_note[256];
	/*
	 * the conflicts of an unmerged file, as R_NOTE rows whose text
	 * lives here, with the conflict number in h and the CROW_ kind in
	 * j
	 */
	char **ctext;
	size_t nctext;
	int y, x, h, w; /* body geometry */
};

/* A row of an unmerged file's conflict view (CROW_* style, conflict number). */
struct crow {
	char *text;
	int style;
	size_t num;
};

/*
 * A search hit, a line of a file's diff or a row of an unmerged file's
 * conflict view. conflict is its number, else 0, and seq its place among
 * the searchable rows of the file's pane.
 */
struct hit {
	char *path;
	size_t seq;
	size_t conflict;
	char op;
	int old_no, new_no;
	char *text;
};

/*
 * An unmerged file's conflicts as first shown, and which of them O and T
 * resolved.
 */
struct cnum {
	char *path;
	size_t total;
	bool *done;
};

/*
 * Above this many changed files a commit's editor gets no git status. It
 * costs seconds and buries the message, and the Files pane shows it all.
 */
#define NO_STATUS_FILES 200
/* Above this many changed files a commit derives the list it leaves. */
#define AFTER_COMMIT_FILES 3000
/* Above this many changed files a split's status counts files. */
#define SPLIT_BIG_FILES 1000

struct fitem {
	bool header; /* a section's or a directory's row, not a file */
	enum sec sec;
	/* index into fs (or cfiles), for a directory its first file's */
	size_t fi;
	/*
	 * for a directory row the length of its path ("a/b/") in its first
	 * file's, for a file the part of its path its directory row shows
	 */
	size_t dirlen;
	bool untracked; /* in the Untracked section, with sec unstaged */
};

/*
 * The status line of an operation in progress, in parts. It is drawn as
 * "HEAD KEYS, INFO, ID \"SUBJECT\"" with dashes between them, and the
 * subject shortened to fit.
 */
struct status_line {
	char head[32];	/* "SPLIT", "REBASE 2/5", or empty */
	char keys[192]; /* with {action} placeholders for keys_subst() */
	char info[160];
	char id[16];
	char subject[256];
	bool optional_keys; /* the keys go before the subject is cut */
};

/*
 * The Series pane. A range of first-parent commits, newest first, loaded a
 * chunk at a time and grown downwards past the range by carve.seriesStep
 * as the cursor gets there. With own (the tip is HEAD) row 0 is the work
 * tree and row k is ids[k - 1].
 */
struct sview {
	char *range;	 /* as given, NULL for @{u}..HEAD or HEAD */
	char *tip;	 /* its tip commit */
	char *right;	 /* the tip as given (HEAD by default) */
	char *base;	 /* what it leaves out (the upstream), or NULL */
	char *base_name; /* e.g. origin/master, for the divider */
	/*
	 * the range ends below, at base or without an upstream at what the
	 * remote-tracking branches have ("pushed")
	 */
	bool bounded;
	bool own;     /* tip is HEAD */
	char **ids;   /* newest first */
	char **lines; /* "abc1234 subject" */
	/*
	 * the branches at each commit, local and remote-tracking, but not
	 * HEAD's own ("master, origin/main"), or NULL
	 */
	char **refs;
	bool *merge;
	size_t n, alloc;
	size_t inrange; /* ids[0 .. inrange) are the range itself */
	bool end;	/* no older commits to load */
	char *next;	/* the first parent of the oldest loaded */
	size_t grow;	/* how many past the range to load next */
	size_t sel;	/* the row shown (the Commit section) */
	size_t step;	/* carve.seriesStep */
	bool stale;	/* sel moved, shown once the cursor settles */
	bool fresh;	/* just opened, with no need to check the tip */
	/* until sel is chosen, the work tree while it has changes, else HEAD */
	bool follow;
};

/* The message line, or a box mid-screen (box). */
struct ui_msg {
	char text[16384];
	bool err;
	/*
	 * A refusal or a warning, in a box mid-screen that the next key
	 * closes and does nothing else. title is its title and top its
	 * scroll.
	 */
	bool box;
	char title[64];
	size_t top;
};

/* Where the panes are, and the sizes asked for. */
/* A pane's frame, its rows and columns with the lines around it. */
struct box {
	int y, x, h, w;
};

/*
 * The panes' places on the screen. The sizes of Files, Series, the message
 * and the diffs are inside their frames, which box[] holds. A pane that is
 * not shown has a box of height 0.
 */
struct ui_layout {
	struct box box[NPANES];
	int list_x; /* the first column inside Series and Files */
	int files_w, files_h;
	int files_y;  /* the top line of the Files pane's frame */
	int series_h; /* rows of the Series pane (top line included), or 0 */
	bool stacked; /* narrow, lists on top and diffs below */
	int narrow_width; /* carve.narrowWidth */
	bool zoom;	  /* the active diff pane on the whole screen (o) */
	/*
	 * Ctrl-W resizes the panes. The sizes asked for (0 for the
	 * default) are kept within what the screen allows at every layout.
	 */
	bool resizing;
	bool ov_zoom; /* overlays on the whole screen (o in one) */
	/*
	 * carve.layout = one, the default, shows one diff pane, the one of
	 * the cursor's section. two shows the Unstaged and the Staged
	 * diff.
	 */
	bool one;
	bool msg_on;	   /* the message pane shown (gc, carve.messagePane) */
	int msg_y, msg_h;  /* its first row and rows (title not counted) */
	int want_left_w;   /* columns of Series and Files (side by side) */
	int want_series_h; /* rows of the Series pane */
	int want_list_h;   /* rows of Series and Files together, when stacked */
	int want_top_h;	   /* rows of the Unstaged diff */
};

/* What the configuration and the terminal say, read at startup and F5. */
struct ui_config {
	bool utf8;
	int tabw;
	bool color;	  /* colours in use (terminal has them, no NO_COLOR) */
	bool ascii_lines; /* carve.asciiLines */
	bool colors256;	  /* carve.colors = 256 */
	long diff_limit;  /* carve.diffLimit, most diff lines a pane shows */
	long max_file_size; /* carve.maxFileSize, larger whole-file diffs are not loaded */
	char **skip; /* carve.skipDiff, patterns of files not loaded */
	size_t nskip;
	bool ws_trailing; /* core.whitespace, the errors to highlight */
	bool ws_space_tab;
};

struct ui {
	struct repo *r;
	struct file_status *fs;
	size_t nfs;
	struct fitem *items;
	size_t nitems, aitems;
	size_t nuntracked;   /* files in the Untracked section */
	size_t fdrawn;	     /* fcur when the Files pane was last drawn */
	bool stopped_by_key; /* a load stopped by the key that comes next */
	bool untracked_open; /* the Untracked section unfolded */
	char *filter;	     /* the Files list shows only paths matching it */
	size_t fcur, ftop;
	char *path;	  /* selected path, NULL if none */
	bool path_staged; /* selected in the Staged section */
	bool path_untracked;
	bool path_commit; /* selected in the Commit section */
	/* the files of the commit shown (ccommit) */
	struct cfile *cfiles;
	size_t ncfiles;
	char *ccommit;
	/* the files of the commit being split (sfiles_of) */
	struct cfile *sfiles;
	size_t nsfiles;
	char *sfiles_of;
	bool commit_changed; /* another commit shown (J / K) */
	size_t cpos;	     /* its distance from the tip */
	/* a rebase of yours stopped cleanly, with HEAD shown as a commit */
	bool rebase_stop;
	int rstep, rtotal;  /* step of total, 0 when unknown */
	struct dpane dp[2]; /* indexed by P_UNSTAGED and P_STAGED minus 1 */
	int active;
	struct ui_msg msg;
	enum action cur_act; /* the action being run, the box's title */
	char *search;
	struct hit *hits; /* ui.search in every diff, made when searching */
	size_t nhits;
	bool hits_valid;
	enum sec hits_sec; /* the section the hits are of */
	char head[256];
	int head_abbrev; /* the length of %h in the title, 0 unknown */
	char *head_oid;	 /* HEAD when the title was made */
	char *branch;	 /* the branch HEAD is on, NULL detached */
	/*
	 * HEAD's upstream as a full ref name or "", asked on branch
	 * upstream_for, and again on another branch and on F5
	 */
	char *upstream, *upstream_for;
	bool full;	/* this reload reads configuration and upstream anew */
	bool skip_scan; /* this reload keeps the untracked files as they are */
	bool status_stale; /* the file's status entry is older than it */
	/*
	 * The index file ui.tracked holds for. With another one, such as
	 * after a git add in another shell, no status letter is trusted
	 * without git.
	 */
	char *status_stamp;
	bool dc_vouched; /* reload_path forgot its one file for this key */
	struct status_line st; /* split or series op in progress */
	struct split_state split;
	/*
	 * the status, tracked (read at once) and untracked (the background
	 * scan)
	 */
	struct file_status *tracked, *scan;
	size_t ntracked, nscan;
	char *scan_ix_stamp; /* the index file the scan's ix_known are of */
	/* the renames found by the last merge_status, reused if keep_renames */
	struct rename *rv;
	size_t nrv;
	bool keep_renames;
	struct bg_proc bg;
	bool untracked_async;
	bool diff_stale; /* diff panes wait for the cursor to settle */
	/*
	 * GIT_CARVE_IDLE, the file that gets "idle" when nothing is left to
	 * do but wait for a key, and "busy" when one comes (for the tests)
	 */
	const char *idle_file;
	bool idle;
	struct series_state series;
	struct ui_layout lay;
	/*
	 * Columns scrolled with h and l, cut off the end of the Files
	 * pane's text and off the start of the Series pane's subjects.
	 */
	int fhscroll, shscroll;
	/* the rows they scroll, the cursor's, and another row starts at 0 */
	size_t fhscroll_row, shscroll_row;
	/* how far they can go for those rows (set when drawn) */
	int fhscroll_max, shscroll_max;
	size_t scur, stop;  /* its cursor and first row shown */
	bool series_hidden; /* gS, carve.seriesPane = false */
	struct ui_config cfg;
	bool scan_lsfiles; /* the scan is git ls-files -o (no cache) */
	bool after_commit; /* the next reload follows a commit */
	bool after_split;  /* the next reload follows a split of HEAD */
	bool freveal;	   /* the list was rebuilt, show the rows above */
	/*
	 * at the next reload the cursor stays in the section of the file
	 * staged or unstaged
	 */
	bool stay_in_section;
	bool franging;	  /* v in Files, a range of files ... */
	size_t fanchor;	  /* ... from this item to the cursor */
	char *force_path; /* this path loads regardless, after load-diff */
	/*
	 * its loading was stopped by a key, and it does not load again
	 * until another file is selected or load-diff
	 */
	char *stopped_path;
	bool signoff;	/* commits get Signed-off-by (Ctrl-S) */
	bool help_quit; /* quit was chosen in the help */
	struct diff_opts
		dopts; /* how the diff panes show diffs, set with + - W w */
	struct keymap keys; /* default + carve.keys.* */
	struct cnum *cnums; /* conflict numbers per unmerged file */
	size_t ncnums;
	struct sview sv; /* the Series pane */
	/* the commit message being written (CARVE_MESSAGE) */
	struct strbuf draft;
	size_t draft_top; /* the message pane's first line shown */
	/*
	 * the message of the commit shown for the message pane, and which
	 * commit it is
	 */
	struct strbuf cmsg;
	char *cmsg_of;
};

/* All UI state (global mutable state is allowed in ui/ only). */
extern struct ui ui;
extern volatile sig_atomic_t want_stop;

/* A diff row has cursor and mark columns, the line numbers, then the text. */
#define ROW_MARGIN 2
#define ROW_TEXT_X (ROW_MARGIN + 13)
#define MIN_COLS 40
#define MIN_LINES 10

/* A part of a line of a text window, bytes [from, to), and its look. */
struct span {
	size_t from, to;
	attr_t a;
};
/*
 * The parts of line i of lines to colour, in order and up to max. Returns
 * how many there are. The rest of the line is plain.
 */
typedef size_t (*line_paint_fn)(char *const *lines, size_t n, size_t i,
				struct span *sp, size_t max);

/* ui.c, messages (a refusal in a box with set_msg(true)) and the active pane */
void set_msg(bool err, const char *fmt, ...) PRINTF_LIKE(2, 3);
void set_miss(const char *fmt, ...) PRINTF_LIKE(1, 2);
void set_warn(const char *fmt, ...) PRINTF_LIKE(1, 2);
void msg_clear(void);
void msg_repo_err(void);
struct dpane *active_dp(void);
/*
 * getch() for a key the user is asked for, with GIT_CARVE_IDLE told while
 * it waits
 */
int user_key(void);
/*
 * In layout one, the diff shown, 0 for unstaged (or a commit's) and 1 for
 * staged.
 */
int one_side(void);
int pane_after(int pane, int dir);
void read_view_config(void);

#endif
