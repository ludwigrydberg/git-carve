// SPDX-License-Identifier: MIT
/* The actions keys are bound to. */
#include "actions.h"
#include "ui_int.h"
#include "dcache.h"
#include "draw.h"
#include "state.h"
#include "sview.h"
#include "term.h"
#include "../absorb.h"
#include "../message.h"
#include "../conflict.h"
#include "../index.h"
#include "../move.h"
#include "../ops.h"
#include "../opstate.h"
#include "../rebase.h"
#include "../rename.h"
#include "../subst.h"
#include "../undo.h"

#define NOT_FOUND_MSG "not found in any %s diff: %s"

/* ---- actions ---------------------------------------------------------- */

static void row_range(const struct dpane *dp, size_t *a, size_t *b)
{
	*a = dp->ranging ? dp->anchor : dp->cur;
	*b = dp->cur;
	if (*a > *b) {
		size_t t = *a;

		*a = *b;
		*b = t;
	}
}

/*
 * Mark the lines covered by rows [a, b], or exactly those marked with m if
 * there are any. A hunk header covers its hunk. Returns the number of
 * change lines marked.
 */
static size_t mark_rows(struct dpane *dp, size_t a, size_t b)
{
	size_t i, j, n = 0;

	sel_clear(dp->fd);
	if (dp->nmarked) {
		for (i = 0; i < dp->nrows; i++) {
			const struct row *rw = &dp->rows[i];

			if (!rw->marked)
				continue;
			dp->fd->hunks[rw->h].lines[rw->j].sel = true;
			n++;
		}
		return n;
	}
	for (i = a; i <= b && i < dp->nrows; i++) {
		const struct row *rw = &dp->rows[i];
		struct hunk *h;

		if (rw->kind == R_NOTE)
			continue;
		h = &dp->fd->hunks[rw->h];
		if (rw->kind == R_HUNK) {
			if (a != b)
				continue;
			for (j = 0; j < h->nlines; j++) {
				if (h->lines[j].op == ' ')
					continue;
				h->lines[j].sel = true;
				n++;
			}
			continue;
		}
		if (h->lines[rw->j].op == ' ')
			continue;
		h->lines[rw->j].sel = true;
		n++;
	}
	return n;
}

/* The status entry of the selected path (either section), or NULL. */
struct file_status *cur_fs(void)
{
	size_t i;

	if (ui.path_commit)
		return NULL; /* a file of the commit shown */
	for (i = 0; ui.path && i < ui.nfs; i++)
		if (!strcmp(ui.fs[i].path, ui.path))
			return &ui.fs[i];
	return NULL;
}

/*
 * Before a line operation in the Unstaged pane a new file becomes
 * intent-to-add and a work-tree rename gets its move staged. The lines are
 * then ordinary changes of the path in the index.
 */
static int prepare_unstaged(void)
{
	struct file_status *f = cur_fs();

	if (f && f->from_y) {
		if (stage_move(ui.r, f->from_y, ui.path))
			return -1;
		free(f->from_y);
		f->from_y = NULL;
		f->y = 'M';
		return 0;
	}
	return ui.path_untracked ? intent_to_add(ui.r, ui.path) : 0;
}

/*
 * A file marked intent-to-add (git add -N). Unstaging the whole index
 * would drop the mark, and unstaging all then names the staged paths.
 */
static bool any_intent_to_add(void)
{
	size_t i;

	for (i = 0; i < ui.nfs; i++)
		if (ui.fs[i].x == ' ' && ui.fs[i].y == 'A')
			return true;
	return false;
}

/* Above this many files a whole-section stage or unstage asks first. */
#define BULK_ASK 100

/*
 * Enter or Space on a section's row, or on a v range of files, takes all
 * of them (those the filter shows) in one git. Files with conflicts are
 * left out, since staging one would mark it resolved.
 */
static void act_files_bulk(void)
{
	bool whole = !ui.franging && !(ui.filter && *ui.filter);
	size_t *fi = NULL, n, nf = 0, i, np = 0, conflicts = 0;
	bool untracked = ui.items[ui.fcur].untracked;
	enum sec sec = ui.items[ui.fcur].sec;
	bool stage_tracked = !untracked && !ui.split.active;
	bool staged = sec == SEC_STAGED;
	const char **paths;
	char q[200];
	int rc;

	if (ui.franging) {
		size_t a = ui.fanchor < ui.fcur ? ui.fanchor : ui.fcur;
		size_t b = ui.fanchor < ui.fcur ? ui.fcur : ui.fanchor;

		/* the cursor on the next file of the section afterwards */
		ui.stay_in_section = true;
		fi = xcalloc(b - a + 2, sizeof(*fi));
		for (n = 0, i = a; i <= b && i < ui.nitems; i++)
			if (!ui.items[i].header && ui.items[i].sec == sec &&
			    ui.items[i].untracked == untracked)
				fi[n++] = ui.items[i].fi;
	} else {
		n = section_files(sec, untracked, &fi);
	}
	ui.franging = false;
	paths = xcalloc(2 * n + 1, sizeof(*paths));
	for (i = 0; i < n; i++) {
		const struct file_status *f = &ui.fs[fi[i]];
		const char *from;

		from = staged ? f->from_x : f->from_y;

		if (f->unmerged) {
			conflicts++;
			continue;
		}
		nf++;
		if (from || !strchr("MD", f->y))
			stage_tracked = false;
		paths[np++] = f->path;
		/* both halves of a rename */
		if (from)
			paths[np++] = from;
	}
	if (!nf) {
		set_msg(true,
			conflicts ? "only files with conflicts: resolve "
				    "them one by one" :
				    "no files to %s",
			staged ? "unstage" : "stage");
		goto out;
	}
	snprintf(q, sizeof(q), "%s %zu files%s? [y/N] ",
		 staged ? "Unstage" : "Stage", nf,
		 ui.filter && *ui.filter ? " that match the filter" : "");
	if (nf > BULK_ASK && !confirm(q)) {
		set_msg(false, "nothing %s", staged ? "unstaged" : "staged");
		goto out;
	}
	/* git may take seconds here, tell the user first */
	set_msg(false, "%s %zu files...", staged ? "unstaging" : "staging", nf);
	draw();
	if (staged && whole && !conflicts && !any_intent_to_add())
		rc = unstage_all(ui.r);
	else if (staged)
		rc = unstage_files(ui.r, paths, np);
	else if (stage_tracked && np > PATHSPEC_ARGS_MAX)
		rc = stage_tracked_paths(ui.r, paths, np);
	else
		rc = stage_files(ui.r, paths, np);
	if (rc) {
		msg_repo_err();
	} else {
		set_msg(false, "%s %zu files%s ({undo} undoes it)",
			staged ? "unstaged" : "staged", nf,
			conflicts ? "; files with conflicts left out" : "");
	}
	reload();
out:
	free(paths);
	free(fi);
}

void act_file_toggle(bool staged_section)
{
	const struct file_status *f = cur_fs();
	int rc;

	if (ui.franging || item_is_section(ui.fcur)) {
		act_files_bulk();
		return;
	}

	if (!ui.path) {
		set_msg(true, "no file selected");
		return;
	}
	/*
	 * a rename is one entry, the move when staging and both halves
	 * when unstaging
	 */
	if (staged_section && f && f->from_x)
		rc = unstage_move(ui.r, f->from_x, ui.path);
	else if (!staged_section && f && f->from_y)
		rc = stage_move(ui.r, f->from_y, ui.path);
	else
		rc = staged_section ? unstage_file(ui.r, ui.path) :
				      stage_file(ui.r, ui.path);
	if (rc) {
		msg_repo_err();
		return;
	}
	if (!staged_section && f && f->from_y) {
		set_msg(false,
			"staged the move %s -> %s; its edits stay unstaged",
			f->from_y, ui.path);
		reload();
		return;
	}
	set_msg(false, "%s %s", staged_section ? "unstaged" : "staged",
		ui.path);
	/* the next file of the section next, not this one where it went */
	ui.stay_in_section = true;
	/* a rename (two paths) gets the whole reload there */
	reload_path(ui.path);
}

/* The selected path is a rename in the staged / unstaged section. */
static bool is_rename_entry(bool staged)
{
	const struct file_status *f = cur_fs();

	return f && (staged ? f->from_x : f->from_y);
}

/* Row i of dp is a changed line (not context, a hunk header or a note). */
static bool change_row(const struct dpane *dp, size_t i)
{
	const struct row *rw = &dp->rows[i];

	return rw->kind == R_LINE &&
	       dp->fd->hunks[rw->h].lines[rw->j].op != ' ';
}

/*
 * After Space the cursor goes to the first changed line from row from on,
 * else to the last one before it, and the next Space goes on with the next
 * change. A staged + line stays where it was as context, and a - line
 * goes.
 */
static void cursor_to_change(struct dpane *dp, size_t from)
{
	size_t i;

	if (!dp->fd || !dp->nrows)
		return;
	if (from >= dp->nrows)
		from = dp->nrows - 1;
	for (i = from; i < dp->nrows; i++)
		if (change_row(dp, i)) {
			dp->cur = i;
			return;
		}
	for (i = from + 1; i-- > 0;)
		if (change_row(dp, i)) {
			dp->cur = i;
			return;
		}
}

void act_space(void)
{
	struct dpane *dp = active_dp();
	enum patch_dir dir;
	char *was = NULL;
	size_t a, b, n;
	bool moved;

	if (!dp) {
		if (ui.fcur < ui.nitems)
			act_file_toggle(ui.items[ui.fcur].sec == SEC_STAGED);
		return;
	}
	dir = dp == &ui.dp[0] ? PATCH_STAGE : PATCH_UNSTAGE;
	if (!dp->fd && dp->cancelled) {
		set_msg(false, "the diff is not loaded: {load-diff} loads it, "
			       "Enter in Files takes the whole file");
		return;
	}
	if (!dp->fd && (dp->too_big || is_rename_entry(dir == PATCH_UNSTAGE))) {
		act_file_toggle(dir == PATCH_UNSTAGE);
		return;
	}
	if (!dp->fd) {
		set_msg(true, "nothing to %s",
			dir == PATCH_STAGE ? "stage" : "unstage");
		return;
	}
	if (!fdiff_line_ops(dp->fd, NULL)) {
		act_file_toggle(dir == PATCH_UNSTAGE);
		return;
	}
	row_range(dp, &a, &b);
	n = mark_rows(dp, a, b);
	if (!n) {
		set_msg(true, "no changed line here");
		return;
	}
	/* staging a work-tree rename's move changes two paths */
	moved = dir == PATCH_STAGE && is_rename_entry(false);
	if (dir == PATCH_STAGE && prepare_unstaged()) {
		msg_repo_err();
		return;
	}
	if (apply_selection(ui.r, dp->fd, dir)) {
		msg_repo_err();
		reload();
		return;
	}
	set_msg(false, "%s %zu line%s",
		dir == PATCH_STAGE ? "staged" : "unstaged", n,
		n == 1 ? "" : "s");
	dp->ranging = false;
	/*
	 * With all files shown, a file with no changes left here gives way
	 * to the next one, at its first change.
	 */
	if (ui.all_files) {
		ui.stay_in_section = true;
		was = xstrdup(ui.path);
	}
	if (moved)
		reload();
	else
		reload_path(ui.path);
	cursor_to_change(dp, was && ui.path && strcmp(was, ui.path) ? 0 : a);
	free(was);
}

/*
 * m marks or unmarks the changed line under the cursor and moves on.
 * Space, e and f then act on exactly the marked lines, which need not be
 * next to each other, such as the old and new version of one line in a
 * block where git lists several removals before the additions.
 */
void act_mark(void)
{
	struct dpane *dp = active_dp();
	struct row *rw;

	if (!dp || !dp->fd || !dp->nrows)
		return;
	rw = &dp->rows[dp->cur];
	if (rw->kind != R_LINE || dp->fd->hunks[rw->h].lines[rw->j].op == ' ') {
		set_msg(false, "{mark} marks changed lines");
		return;
	}
	rw->marked = !rw->marked;
	if (rw->marked)
		dp->nmarked++;
	else
		dp->nmarked--;
	dp->ranging = false;
	if (dp->cur + 1 < dp->nrows)
		dp->cur++;
	set_msg(false, "%zu line%s marked", dp->nmarked,
		dp->nmarked == 1 ? "" : "s");
}

/*
 * gm marks a changed line and the one it is paired with (-a +A, as staging
 * pairs them), a changed line in one key. When both are marked it unmarks
 * them.
 */
void act_mark_pair(void)
{
	struct dpane *dp = active_dp();
	size_t i, other = 0;
	struct row *rw;
	bool on;
	long p;

	if (!dp || !dp->fd || !dp->nrows)
		return;
	rw = &dp->rows[dp->cur];
	if (rw->kind != R_LINE || dp->fd->hunks[rw->h].lines[rw->j].op == ' ') {
		set_msg(false, "{mark-pair} marks a changed line and its pair");
		return;
	}
	p = dline_partner(&dp->fd->hunks[rw->h], rw->j);
	if (p < 0) {
		set_msg(false,
			"this line has no pair on the other side: {mark} "
			"marks it alone");
		return;
	}
	for (i = 0; i < dp->nrows; i++)
		if (dp->rows[i].kind == R_LINE && dp->rows[i].h == rw->h &&
		    dp->rows[i].j == (size_t)p)
			other = i;
	on = !(rw->marked && dp->rows[other].marked);
	for (i = 0; i < 2; i++) {
		struct row *r = i ? &dp->rows[other] : rw;

		if (r->marked != on)
			dp->nmarked += on ? 1 : (size_t)-1;
		r->marked = on;
	}
	dp->ranging = false;
	/* on past the pair */
	i = other > dp->cur ? other : dp->cur;
	if (i + 1 < dp->nrows)
		dp->cur = i + 1;
	set_msg(false, "%zu line%s marked", dp->nmarked,
		dp->nmarked == 1 ? "" : "s");
}

void act_select_hunk(void)
{
	struct dpane *dp = active_dp();
	size_t i, first, last;

	if (!dp || !dp->nrows || dp->rows[dp->cur].kind == R_NOTE)
		return;
	i = dp->cur;
	while (i > 0 && dp->rows[i].kind != R_HUNK)
		i--;
	first = i;
	last = i;
	while (last + 1 < dp->nrows && dp->rows[last + 1].kind == R_LINE)
		last++;
	dp->ranging = true;
	dp->anchor = first + 1 <= last ? first + 1 : first;
	dp->cur = last;
}

/*
 * text (len bytes) in $EDITOR, in a temporary file named name, whose
 * extension tells the editor the syntax. *out gets what was saved. Returns
 * -1 with the message line set when that could not be done.
 */
static int edit_text(const char *name, const char *text, size_t len,
		     struct strbuf *out)
{
	struct strbuf file = STRBUF_INIT, dir = STRBUF_INIT;
	const char *tmp = getenv("TMPDIR");
	int rc = -1;

	/* $TMPDIR as git uses it, else /tmp */
	sb_addf(&dir, "%s/git-carve.XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(dir.buf)) {
		set_msg(true, "cannot make a temporary directory: %s",
			strerror(errno));
		sb_release(&dir);
		return -1;
	}
	sb_addf(&file, "%s/%s", dir.buf, name);
	if (write_file(file.buf, text ? text : "", len)) {
		set_msg(true, "cannot write %s", file.buf);
		goto out;
	}
	suspend_curses();
	rc = run_editor(ui.r, file.buf, 0);
	resume_curses();
	if (rc) {
		msg_repo_err();
		goto out;
	}
	rc = read_file(file.buf, out);
	if (rc)
		set_msg(true, "cannot read back %s", file.buf);
out:
	unlink(file.buf);
	rmdir(dir.buf);
	sb_release(&file);
	sb_release(&dir);
	return rc;
}

/* The base name of path, which the editor's file is named after. */
static const char *base_name(const char *path)
{
	const char *b = strrchr(path, '/');

	return b ? b + 1 : path;
}

static int cursor_line(const struct dpane *dp)
{
	size_t i;

	if (!dp || !dp->fd)
		return 0;
	for (i = dp->cur; i < dp->nrows; i++) {
		const struct row *rw = &dp->rows[i];

		if (rw->kind == R_LINE &&
		    dp->fd->hunks[rw->h].lines[rw->j].new_no)
			return dp->fd->hunks[rw->h].lines[rw->j].new_no;
		if (rw->kind == R_HUNK)
			return dp->fd->hunks[rw->h].new_start;
	}
	return 0;
}

void act_open_file(void)
{
	const struct dpane *dp = active_dp();
	int line = cursor_line(dp);

	if (!ui.path) {
		set_msg(true, "no file selected");
		return;
	}
	if (!path_in_worktree(ui.path)) {
		set_msg(false, "%s is not in the work tree%s", ui.path,
			ui.path_commit ? " (the commit deletes it)" : "");
		return;
	}
	suspend_curses();
	if (run_editor(ui.r, ui.path, line))
		msg_repo_err();
	resume_curses();
	reload();
}

void act_commit(bool amend)
{
	/*
	 * the TUI says what happened, and git's summary would stay on the
	 * terminal
	 */
	struct commit_opts o = { .amend = amend,
				 .signoff = ui.signoff,
				 .quiet = true,
				 .no_status = ui.nfs > NO_STATUS_FILES };

	if (!amend && !staged_any()) {
		set_msg(true, "nothing staged to commit");
		return;
	}
	suspend_curses();
	/* a message written in the message pane is committed with it */
	if (!amend && ui.draft.len ? commit_draft(ui.r, &o) :
				     carve_commit(ui.r, &o))
		msg_repo_err();
	else
		set_msg(false, amend ? "amended HEAD" : "committed");
	resume_curses();
	/* a commit leaves the untracked files as they were */
	ui.skip_scan = true;
	/*
	 * and the work tree's side of the tracked ones, but not after an
	 * amend, which changed HEAD's tree under them
	 */
	ui.after_commit = !amend;
	reload();
}

void act_split(void)
{
	struct strbuf ids = STRBUF_INIT;
	char q[512];
	char *head;

	if (ui.split.active) {
		if (split_finish(ui.r, &ids)) {
			msg_repo_err();
		} else {
			size_t n = count_lines(ids.buf, ids.len);
			bool rebase = foreign_rebase(ui.r);

			set_msg(false, "split finished: %zu commit%s%s", n,
				n == 1 ? "" : "s",
				rebase ? "; your rebase goes on with git "
					 "rebase --continue" :
					 "");
		}
		sb_release(&ids);
		reload();
		return;
	}
	/*
	 * refused before asking, since a split needs a clean tree (the
	 * core checks again, against the tree as it is then)
	 */
	if (tracked_clean_now() == 0) {
		set_msg(true, DIRTY_TREE_MSG);
		return;
	}
	/* an older commit shown in the Series pane is the one split */
	if (ui.ccommit && !head_shown() && ui.series.op == OP_NONE) {
		char *c = xstrdup(ui.ccommit);

		{
			head = commit_line(c, true);
			snprintf(q, sizeof(q),
				 "Split %s (the commits above it "
				 "are replayed when it is finished)? [y/N] ",
				 head ? head : c);
			free(head);
			if (!confirm(q)) {
				free(c);
				return;
			}
			if (split_start(ui.r, c))
				msg_repo_err();
			show_warnings();
			reload();
		}
		free(c);
		return;
	}
	head = commit_line("HEAD", true);
	snprintf(q, sizeof(q), "Split HEAD %s? [y/N] ", head ? head : "(none)");
	free(head);
	if (!confirm(q))
		return;
	if (split_start(ui.r, NULL)) {
		msg_repo_err();
		return;
	}
	set_msg(false, "HEAD's changes are unstaged now: stage and commit "
		       "them piece by piece, then {finish} to finish");
	ui.after_split = true;
	show_warnings();
	ui.active = P_FILES;
	ui.fcur = 0;
	free(ui.path);
	ui.path = NULL;
	reload();
}

void act_abort(void)
{
	char q[768];
	int rc;

	if (!ui.split.active) {
		set_msg(true, "no split in progress");
		return;
	}
	snprintf(q, sizeof(q), "Abort the split and restore %.7s? [y/N] ",
		 ui.split.orig);
	if (!confirm(q))
		return;
	rc = split_abort(ui.r, false);
	if (rc) {
		bool forget = strstr(sb_str(&ui.r->err), "forgets");

		if (!strstr(sb_str(&ui.r->err), "--force")) {
			msg_repo_err();
			return;
		}
		/*
		 * when the rebase moved on, forget the split, else changes
		 * are in the way
		 */
		snprintf(q, sizeof(q), "%s \xe2\x80\x94 %s? [y/N] ",
			 ui.r->err.buf, forget ? "forget it" : "discard them");
		sb_reset(&ui.r->err);
		if (!confirm(q))
			return;
		rc = split_abort(ui.r, true);
		if (rc < 0) {
			msg_repo_err();
			return;
		}
	}
	set_msg(false,
		rc ? "split forgotten; nothing else changed" : "split aborted");
	reload();
}

/* t moves the selection, or what is staged, into an older commit. */
void act_fixup(void)
{
	struct dpane *dp = active_dp();
	bool was_clean = true;
	char *c, *subj = NULL;
	int rc;

	if (ui.series.op != OP_NONE ||
	    (ui.split.active && ui.split.commits_made)) {
		set_msg(true,
			ui.split.active ?
				"while splitting, {fixup-to} moves a change into the commit "
				"below the split, before the first commit" :
				OP_BUSY_MSG);
		return;
	}
	if (dp == &ui.dp[1] || (!dp && ui.fcur < ui.nitems &&
				ui.items[ui.fcur].sec == SEC_STAGED)) {
		if (!staged_any()) {
			set_msg(true, "nothing staged");
			return;
		}
		was_clean = false;
	} else if (staged_any()) {
		set_msg(true, "the index already has staged changes; commit "
			      "them, or use {fixup-to} in the Staged pane to "
			      "move them");
		return;
	} else if (dp && (!dp->fd || !fdiff_line_ops(dp->fd, NULL))) {
		set_msg(false, "{fixup-to}: select changed lines first");
		return;
	} else if (dp) {
		size_t a, b;

		row_range(dp, &a, &b);
		if (!mark_rows(dp, a, b)) {
			set_msg(true, "no changed line here");
			return;
		}
	} else if (!ui.path) {
		set_msg(true, "no file selected");
		return;
	}
	if (ui.split.active) {
		/* the one commit a change can go into during a split */
		char q[300], *s = commit_line("HEAD", false);

		snprintf(q, sizeof(q),
			 "Move it into %.200s, the commit below "
			 "the split? [y/N] ",
			 s ? s : "HEAD");
		free(s);
		c = confirm(q) ? repo_rev(ui.r, "HEAD") : NULL;
	} else {
		c = pick_commit_sel("Move the change into which commit?",
				    "HEAD", NULL, true);
	}
	if (!c)
		return;
	if (was_clean) {
		if (dp) {
			if (prepare_unstaged())
				goto err;
			if (apply_selection(ui.r, dp->fd, PATCH_STAGE))
				goto err;
		} else if (is_rename_entry(false) ?
				   /* always both halves of a rename */
				   prepare_unstaged() ||
					   stage_file(ui.r, ui.path) :
				   stage_file(ui.r, ui.path)) {
			goto err;
		}
	}
	/* the id changes when the commit is rewritten, the subject stays */
	subj = repo_commit_subject(ui.r, c);
	rc = fixup_to(ui.r, c, was_clean);
	if (rc < 0)
		goto err;
	if (rc == 1)
		msg_repo_err();
	else
		set_msg(false, "moved into \"%s\"", subj ? subj : c);
	show_warnings();
	if (dp)
		dp->ranging = false;
	free(c);
	free(subj);
	reload();
	return;
err:
	msg_repo_err();
	free(c);
	free(subj);
	reload();
}

/*
 * b in the f and t pickers blames the lines of the change in the diff of
 * the file shown. With something staged that is all of the file's staged
 * change, since it all goes. Else it is the lines selected in the Unstaged
 * pane, which f and t marked, or the whole file's change from the Files
 * pane.
 */
int blame_suggest(struct blame_count **v, size_t *n, int *total)
{
	struct dpane *dp = staged_any() ? &ui.dp[1] : &ui.dp[0];

	if (ui.path_commit || dp->commit)
		return repo_err(ui.r, "the lines of a commit: not blamed");
	if (!dp->fd || dp->too_big)
		return repo_err(ui.r, "the diff of %s is not loaded",
				ui.path ? ui.path : "the file");
	if (dp != active_dp() || dp == &ui.dp[1])
		sel_clear(dp->fd);
	return blame_change(ui.r, dp->fd, v, n, total);
}

/*
 * f makes a fixup! commit for a commit chosen from a list, starting at the
 * one shown in the Series pane. It takes what is staged, else the selected
 * lines (the file in the Files pane), staged once the commit is chosen. F
 * makes an amend! commit with its message in the editor, with what is
 * staged only. Nothing is rewritten, squashing comes later.
 */
void act_fixup_commit(bool amend)
{
	struct fixup_opts o = { .amend = amend, .quiet = true };
	bool lines = false, file = false;
	struct dpane *dp = active_dp();
	char *c, *subj;
	int rc;

	if (ui.split.active || ui.series.op != OP_NONE) {
		set_msg(true, OP_BUSY_MSG);
		return;
	}
	if (!amend && !staged_any()) {
		if (dp == &ui.dp[0] && dp->fd && !dp->too_big &&
		    fdiff_line_ops(dp->fd, NULL)) {
			size_t a, b;

			row_range(dp, &a, &b);
			if (!mark_rows(dp, a, b)) {
				set_msg(true, "no changed line here");
				return;
			}
			lines = true;
		} else if (!dp && ui.active == P_FILES && ui.path &&
			   !ui.path_staged && !ui.path_commit) {
			file = true;
		} else {
			set_msg(true, "nothing staged: select changed lines, "
				      "or stage them first");
			return;
		}
	}
	c = pick_commit_sel(amend ? "An amend! commit (a new message) for "
				    "which commit?" :
				    "A fixup! commit for which commit?",
			    "HEAD", ui.ccommit, !amend || staged_any());
	if (!c)
		return;
	if (lines ? prepare_unstaged() ||
			    apply_selection(ui.r, dp->fd, PATCH_STAGE) :
	    file  ? (is_rename_entry(false) && prepare_unstaged()) ||
			     stage_file(ui.r, ui.path) :
		    0) {
		msg_repo_err();
		free(c);
		reload();
		return;
	}
	subj = repo_commit_subject(ui.r, c);
	o.was_clean = lines || file;
	if (amend)
		suspend_curses();
	rc = fixup_commit(ui.r, c, &o);
	if (amend)
		resume_curses();
	if (rc) {
		msg_repo_err();
	} else {
		set_msg(false, "%s commit for \"%s\" made",
			amend ? "amend!" : "fixup!", subj ? subj : c);
	}
	if (dp)
		dp->ranging = false;
	free(c);
	free(subj);
	ui.skip_scan = true;
	reload();
}

/*
 * S squashes the fixups of the series (the Series pane's, when its tip is
 * HEAD) into their commits, after saying what it does.
 */
void act_squash(void)
{
	struct squash_plan p;
	char q[512], *subj;
	int rc;

	if (ui.split.active || ui.series.op != OP_NONE) {
		set_msg(true, OP_BUSY_MSG);
		return;
	}
	if (squash_plan(ui.r, ui.sv.own ? ui.sv.base : NULL, &p)) {
		msg_repo_err();
		return;
	}
	subj = repo_commit_subject(ui.r, p.c[p.first].id);
	snprintf(q, sizeof(q),
		 "Squash %d fixup%s into %d commit%s, from "
		 "%.7s \"%.80s\" up%s? [y/N] ",
		 p.nfix, p.nfix == 1 ? "" : "s", p.ntargets,
		 p.ntargets == 1 ? "" : "s", p.c[p.first].id, subj ? subj : "",
		 p.left.len ? " (some have no commit in the "
			      "series: they stay)" :
			      "");
	free(subj);
	if (!confirm(q)) {
		set_msg(false, "not squashed");
		squash_plan_release(&p);
		return;
	}
	rc = squash_run(ui.r, &p);
	if (rc)
		msg_repo_err();
	else
		set_msg(false, "squashed %d fixup%s into %d commit%s", p.nfix,
			p.nfix == 1 ? "" : "s", p.ntargets,
			p.ntargets == 1 ? "" : "s");
	show_warnings();
	squash_plan_release(&p);
	reload();
}

static void act_series_continue(void)
{
	int rc = op_continue(ui.r);

	if (rc < 0 || rc == 1)
		msg_repo_err();
	else
		set_msg(false, "done");
	show_warnings();
	reload();
}

void act_series_abort(void)
{
	if (!confirm("Abort and restore the series as it was? [y/N] "))
		return;
	if (op_abort(ui.r))
		msg_repo_err();
	else
		set_msg(false, "aborted");
	reload();
}

/* The work-tree line of row i of dp, or 0 when it has none. */
static int row_worktree_line(const struct dpane *dp, size_t i)
{
	const struct row *rw = &dp->rows[i];
	const struct dline *d;

	if (rw->kind != R_LINE)
		return 0;
	d = &dp->fd->hunks[rw->h].lines[rw->j];
	if (!d->new_no)
		return 0; /* a removed line is not in the file */
	/* for the commit shown, where that line is now */
	if (dp->commit && ui.ccommit) {
		int l = worktree_line(ui.r, ui.ccommit, ui.path, d->new_no);

		return l > 0 ? l : 0;
	}
	return d->new_no;
}

/*
 * :s/pattern/replacement/[gi] on the selected lines (marked, the range,
 * the line or the hunk under the cursor), and :%s/.../ on the whole file.
 * The work tree changes, as with E, and u undoes it.
 */
static void act_substitute(const char *expr)
{
	size_t n = 0, alloc = 0, a, b, i;
	struct strbuf err = STRBUF_INIT;
	struct dpane *dp = active_dp();
	struct line_range *rg = NULL;
	struct undo_state ub = { 0 };
	bool whole = *expr == '%';
	struct subst s;
	const char *p;
	int changed;

	if (!ui.path) {
		set_msg(true, ":s works on a file: select one first");
		return;
	}
	if (subst_parse(whole ? expr + 1 : expr, &s, &err)) {
		set_msg(true, "%s", err.buf);
		sb_release(&err);
		return;
	}
	if (!whole) {
		if (!dp || dp->cached || !dp->fd) {
			set_msg(true,
				":s changes the work tree: use it in the "
				"Unstaged pane or the commit's diff (:%%s "
				"for the whole file)");
			goto out;
		}
		row_range(dp, &a, &b);
		for (i = 0; i < dp->nrows; i++) {
			const struct row *rw = &dp->rows[i];
			bool want;
			int l;

			want = dp->nmarked ? rw->marked : i >= a && i <= b;

			/* a hunk header alone stands for its lines */
			if (!dp->nmarked && a == b && rw->kind == R_HUNK &&
			    i == a) {
				size_t k;

				for (k = i + 1; k < dp->nrows &&
						dp->rows[k].kind == R_LINE;
				     k++) {
					l = row_worktree_line(dp, k);
					if (!l)
						continue;
					ALLOC_GROW(rg, n + 1, alloc);
					rg[n].from = rg[n].to = l;
					n++;
				}
				break;
			}
			if (!want || !(l = row_worktree_line(dp, i)))
				continue;
			ALLOC_GROW(rg, n + 1, alloc);
			rg[n].from = rg[n].to = l;
			n++;
		}
		if (!n) {
			set_msg(true, "no line of the work tree selected");
			goto out;
		}
	}
	undo_begin(ui.r, &ub, "substitute");
	p = ui.path;
	if (undo_capture_files(ui.r, &ub, &p, 1, true)) {
		msg_repo_err();
		undo_state_release(&ub);
		goto out;
	}
	changed = subst_file(ui.r, ui.path, rg, n, &s);
	if (changed < 0) {
		msg_repo_err();
		undo_state_release(&ub);
		goto out;
	}
	if (!changed) {
		undo_state_release(&ub);
		set_miss("no match in the %s", whole  ? "file" :
					       n == 1 ? "line" :
							"lines");
		goto out;
	}
	undo_end(ui.r, &ub, "substitute", ui.path);
	set_msg(false, "replaced in %d line%s of %s ({undo} undoes it)",
		changed, changed == 1 ? "" : "s", ui.path);
	if (dp)
		dp->ranging = false;
	reload();
out:
	free(rg);
	subst_free(&s);
	sb_release(&err);
}

void act_goto(void)
{
	struct dpane *dp;
	bool old = false;
	char buf[256];
	const char *p;
	size_t i;
	int n;

	if (!prompt(":", buf, sizeof(buf)))
		return;
	/* :s/a/b/ and :%s/a/b/ replace, anything else is a line */
	if ((buf[0] == 's' && buf[1] && !isalnum((unsigned char)buf[1])) ||
	    (buf[0] == '%' && buf[1] == 's')) {
		act_substitute(buf);
		return;
	}
	if (ui.active == P_FILES)
		ui.active = P_UNSTAGED;
	dp = active_dp();
	/*
	 * the same forms as CLI selections, n12, o12, new:12, old:12, and
	 * a bare number for the new side
	 */
	p = buf;
	sel_side(&p, &old);
	if (parse_int(p, &p, &n) || *p) {
		set_miss("not a line: %s (use N, nN or oN)", buf);
		return;
	}
	for (i = 0; dp->fd && i < dp->nrows; i++) {
		const struct row *rw = &dp->rows[i];
		const struct dline *d;

		if (rw->kind != R_LINE)
			continue;
		d = &dp->fd->hunks[rw->h].lines[rw->j];
		if ((old ? d->old_no : d->new_no) == n) {
			dp->cur = i;
			return;
		}
	}
	set_miss("%c%d is not in the diff", old ? 'o' : 'n', n);
}

/* A row a search looks at, a diff line or a line of a conflict. */
static bool row_searchable(const struct row *rw)
{
	return rw->kind == R_LINE ||
	       (rw->kind == R_NOTE && rw->j != CROW_NOTE && rw->j != CROW_HEAD);
}

static bool row_matches(const struct dpane *dp, size_t i, const char *pat)
{
	const struct row *rw = &dp->rows[i];
	const struct dline *d;

	/* the lines of conflicts are searchable too (not their headings) */
	if (rw->kind == R_NOTE && row_searchable(rw))
		return smart_match(rw->note, strlen(rw->note), pat);
	if (rw->kind != R_LINE)
		return false;
	d = &dp->fd->hunks[rw->h].lines[rw->j];
	return smart_match(d->text, d->len, pat);
}

/* ---- search across all files ------------------------------------------ */

void hits_clear(void)
{
	size_t i;

	for (i = 0; i < ui.nhits; i++) {
		free(ui.hits[i].path);
		free(ui.hits[i].text);
	}
	free(ui.hits);
	ui.hits = NULL;
	ui.nhits = 0;
	ui.hits_valid = false;
}

/* Hits in the conflict views of the unmerged files (not in git diff). */
static void conflict_hits(void)
{
	size_t f, alloc = ui.nhits;

	for (f = 0; f < ui.nfs; f++) {
		size_t n, i, seq = 0;
		struct crow *v;

		if (!ui.fs[f].unmerged)
			continue;
		n = conflict_view(ui.fs[f].path, &v);
		for (i = 0; i < n; i++) {
			struct hit *k;

			if (v[i].style == CROW_NOTE || v[i].style == CROW_HEAD)
				continue;
			seq++;
			if (!smart_match(v[i].text, strlen(v[i].text),
					 ui.search))
				continue;
			ALLOC_GROW(ui.hits, ui.nhits + 1, alloc);
			k = &ui.hits[ui.nhits++];
			k->path = xstrdup(ui.fs[f].path);
			k->seq = seq - 1;
			k->conflict = v[i].num;
			k->op = 'c';
			k->old_no = k->new_no = 0;
			k->text = xstrdup(v[i].text);
		}
		crows_free(v, n);
	}
}

/* Hits in file order, and in each file in the order of its rows. */
static int hit_cmp(const void *a, const void *b)
{
	const struct hit *x = a, *y = b;
	int c;

	c = strcmp(x->path, y->path);

	if (c)
		return c;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/*
 * Every line of every file's diff in section sec (unstaged, staged, or the
 * commit shown) that matches ui.search, from one git diff made when
 * searching. seq is the line's place among the diff lines of its file,
 * which is its R_LINE row in a diff pane.
 */
/* The lines of one file's diff that match ui.search, as hits. */
static void file_hits(const struct fdiff *fd, void *ctx)
{
	const char *path = fd->new_path ? fd->new_path : fd->old_path;
	size_t seq = 0, alloc = ui.nhits, h, j;

	(void)ctx;
	if (!path || !fdiff_line_ops(fd, NULL))
		return;
	for (h = 0; h < fd->nhunks; h++) {
		for (j = 0; j < fd->hunks[h].nlines; j++, seq++) {
			const struct dline *d = &fd->hunks[h].lines[j];
			struct hit *k;

			if (!smart_match(d->text, d->len, ui.search))
				continue;
			ALLOC_GROW(ui.hits, ui.nhits + 1, alloc);
			k = &ui.hits[ui.nhits++];
			k->path = xstrdup(path);
			k->seq = seq;
			k->conflict = 0;
			k->op = d->op;
			k->old_no = d->old_no;
			k->new_no = d->new_no;
			k->text = xstrndup(d->text, d->len);
		}
	}
}

/*
 * Above this many changed files a search reads the files before it diffs
 * them, and diffs only those that can hold the pattern, unless there are
 * more of them than a command line takes.
 */
#define SEARCH_SCAN_MIN 500
#define SEARCH_SCAN_PATHS 2000

static void hits_build(enum sec sec)
{
	/* skipped files are left out, and a key or a huge diff stops it */
	struct diff_opts lo = ui.dopts;
	enum run_stop stop = RUN_DONE;
	struct argv only = ARGV_INIT;
	struct fdiff *fds = NULL;
	size_t n = 0, f;

	hits_clear();
	ui.hits_valid = true;
	ui.hits_sec = sec;
	/* the diffs the cache holds need no git at all */
	if (sec != SEC_COMMIT &&
	    dcache_each(sec == SEC_STAGED, file_hits, NULL))
		goto done;
	lo.exclude = ui.cfg.skip;
	lo.nexclude = ui.cfg.nskip;
	lo.lim.cancel_on_tty = true;
	lo.lim.max_lines =
		ui.cfg.diff_limit > 0 ? (size_t)ui.cfg.diff_limit * 10 : 0;
	lo.lim.stopped = &stop;
	/* this may take seconds, tell the user and how to stop it */
	set_msg(false, "searching all %s diffs... (any key stops it)",
		sec_name(sec));
	draw();
	/*
	 * In a big change set the files that can hold the pattern are found
	 * first, without their diffs, and only those are diffed.
	 */
	if (sec != SEC_COMMIT && ui.ntracked > SEARCH_SCAN_MIN) {
		if (diff_paths_matching(ui.r, sec == SEC_STAGED, ui.search, &lo,
					&only)) {
			msg_repo_err();
			goto out;
		}
		if (stop == RUN_DONE && !only.n)
			goto done;
		if (only.n <= SEARCH_SCAN_PATHS) {
			lo.only = only.v;
			lo.nonly = only.n;
		}
	}
	if (stop == RUN_DONE &&
	    (sec == SEC_COMMIT ?
		     load_commit_diff_all(ui.r,
					  ui.ccommit ? ui.ccommit : "HEAD", &lo,
					  &fds, &n) :
		     load_diff_all(ui.r, sec == SEC_STAGED, &lo, &fds, &n))) {
		msg_repo_err();
		goto out;
	}
	msg_clear();
	if (stop != RUN_DONE) {
		/* nothing to search, try again next time */
		ui.hits_valid = false;
		ui.stopped_by_key = stop != RUN_LIMIT;
		set_miss(stop == RUN_LIMIT ?
				 "not searched: the diffs are over %ld lines "
				 "(10 x carve.diffLimit)" :
				 "search stopped by a key",
			 ui.cfg.diff_limit * 10);
		goto out;
	}
	for (f = 0; f < n; f++)
		file_hits(&fds[f], NULL);
	fdiff_free_array(fds, n);
done:
	msg_clear();
	if (sec == SEC_UNSTAGED)
		conflict_hits();
	if (ui.nhits > 1)
		qsort(ui.hits, ui.nhits, sizeof(*ui.hits), hit_cmp);
out:
	argv_clear(&only);
}

/* Put the cursor on hit k, switching files as needed. */
static void hit_jump(size_t k)
{
	const struct hit *t = &ui.hits[k];
	size_t i, seq = 0;
	struct dpane *dp;

	if (!ui.path || strcmp(ui.path, t->path)) {
		for (i = 0; i < ui.nitems; i++)
			if (!ui.items[i].header &&
			    ui.items[i].sec == ui.hits_sec &&
			    !strcmp(item_path(i), t->path))
				break;
		if (i == ui.nitems) {
			set_miss("%s is not in the list", t->path);
			return;
		}
		ui.fcur = i;
		select_item();
		ui.dp[0].cur = ui.dp[0].top = ui.dp[1].cur = ui.dp[1].top = 0;
		ui.dp[0].ranging = ui.dp[1].ranging = false;
		diffs_stale();
		diffs_load();
	}
	ui.active = ui.hits_sec == SEC_STAGED ? P_STAGED : P_UNSTAGED;
	dp = active_dp();
	for (i = 0; i < dp->nrows; i++) {
		if (!row_searchable(&dp->rows[i]))
			continue;
		if (seq++ == t->seq) {
			dp->cur = i;
			return;
		}
	}
}

/*
 * Where the cursor is, as the searchable rows before it and whether it is
 * on one.
 */
static size_t cursor_seq(const struct dpane *dp, bool *on_line)
{
	size_t i, seq = 0;

	for (i = 0; i < dp->cur && i < dp->nrows; i++)
		if (row_searchable(&dp->rows[i]))
			seq++;
	*on_line = dp->cur < dp->nrows && row_searchable(&dp->rows[dp->cur]);
	return seq;
}

/*
 * The hit after (dir > 0) or before the cursor, wrapping around. -1 if
 * there is none.
 */
static long hit_next(const struct dpane *dp, int dir, bool *wrapped)
{
	const char *path = ui.path ? ui.path : "";
	bool on_line;
	size_t seq;
	size_t k;

	seq = cursor_seq(dp, &on_line);

	*wrapped = false;
	if (!ui.nhits)
		return -1;
	if (dir > 0) {
		for (k = 0; k < ui.nhits; k++) {
			int c = strcmp(ui.hits[k].path, path);

			if (c > 0 ||
			    (!c && (ui.hits[k].seq > seq ||
				    (!on_line && ui.hits[k].seq == seq))))
				return (long)k;
		}
		*wrapped = true;
		return 0;
	}
	for (k = ui.nhits; k > 0; k--) {
		int c = strcmp(ui.hits[k - 1].path, path);

		if (c < 0 || (!c && ui.hits[k - 1].seq < seq))
			return (long)k - 1;
	}
	*wrapped = true;
	return (long)ui.nhits - 1;
}

/*
 * The first hit in a file after the selected one (dir > 0), or the last in
 * a file before it, wrapping around. -1 if there are none.
 */
static long hit_other_file(int dir, bool *wrapped)
{
	const char *path = ui.path ? ui.path : "";
	size_t k;

	*wrapped = false;
	if (!ui.nhits)
		return -1;
	if (dir > 0) {
		for (k = 0; k < ui.nhits; k++)
			if (strcmp(ui.hits[k].path, path) > 0)
				return (long)k;
		*wrapped = true;
		return 0;
	}
	for (k = ui.nhits; k > 0; k--)
		if (strcmp(ui.hits[k - 1].path, path) < 0)
			return (long)k - 1;
	*wrapped = true;
	return (long)ui.nhits - 1;
}

/* Above this many changed files n searches the file shown first. */
#define SEARCH_FILE_FIRST 3000

/* The diff pane searches go through, and its hits up to date. */
static struct dpane *search_pane(void)
{
	struct dpane *dp = active_dp();
	enum sec sec;

	if (!dp)
		return NULL;
	sec = dp == &ui.dp[1] ? SEC_STAGED :
	      dp->commit      ? SEC_COMMIT :
				SEC_UNSTAGED;
	if (!ui.hits_valid || ui.hits_sec != sec)
		hits_build(sec);
	return dp;
}

void search_next(int dir)
{
	struct dpane *dp;
	size_t n, i, k;
	bool wrapped;
	long h;

	if (!ui.search || !*ui.search) {
		set_msg(false, "no search pattern (/)");
		return;
	}
	/*
	 * In a huge change set the file shown comes first, without the
	 * diff of every file, which takes seconds. That diff runs only
	 * when the search leaves the file.
	 */
	dp = active_dp();
	if (dp && ui.ntracked > SEARCH_FILE_FIRST && !ui.hits_valid) {
		for (i = dp->cur + (size_t)dir; i < dp->nrows;
		     i += (size_t)dir) {
			if (row_matches(dp, i, ui.search)) {
				dp->cur = i;
				set_msg(false, "a match in this file; "
					       "{search-next} searches the "
					       "other files too");
				return;
			}
		}
	}
	dp = search_pane();
	if (!dp) {
		/*
		 * the files listed, then those of the folded Untracked
		 * section
		 */
		for (;;) {
			n = ui.nitems;
			for (k = 1; k <= n; k++) {
				i = (ui.fcur + n + (size_t)dir * k) % n;
				if (!ui.items[i].header &&
				    smart_match(item_path(i),
						strlen(item_path(i)),
						ui.search)) {
					ui.fcur = i;
					select_item();
					dp_load(&ui.dp[0], ui.path, false);
					dp_load(&ui.dp[1], ui.path, true);
					return;
				}
			}
			if (ui.untracked_open || !untracked_match(ui.search))
				break;
			fold_untracked(1);
		}
		set_miss("not found: %s", ui.search);
		return;
	}
	/*
	 * The pane itself first, since it also shows what git diff does
	 * not (a new or renamed file, a conflict), then the other files.
	 */
	for (i = dp->cur + (size_t)dir; i < dp->nrows; i += (size_t)dir) {
		if (row_matches(dp, i, ui.search)) {
			dp->cur = i;
			set_msg(false, "%zu match%s in %s changes", ui.nhits,
				ui.nhits == 1 ? "" : "es",
				sec_name(ui.hits_sec));
			return;
		}
	}
	/* the search stopped before the other files, and hits_build said it */
	if (!ui.hits_valid)
		return;
	h = hit_other_file(dir, &wrapped);
	if (h < 0) {
		set_miss(NOT_FOUND_MSG, sec_name(ui.hits_sec), ui.search);
		return;
	}
	hit_jump((size_t)h);
	set_msg(false, "%s%zu match%s in %s changes",
		wrapped ? (dir > 0 ? "search wrapped to the top; " :
				     "search wrapped to the bottom; ") :
			  "",
		ui.nhits, ui.nhits == 1 ? "" : "es", sec_name(ui.hits_sec));
}

void act_search(void)
{
	char buf[256];

	if (!prompt("/", buf, sizeof(buf)))
		return;
	free(ui.search);
	ui.search = xstrdup(buf);
	ui.hits_valid = false;
	search_next(1);
}

static void filter_changed(const char *text)
{
	set_filter(text);
}

/*
 * / in the Files pane shows only the files whose path matches (smart
 * case), as it is typed. Enter keeps it (empty for none), and Esc puts
 * back the one before.
 */
void act_filter(void)
{
	char buf[256], *before = ui.filter ? xstrdup(ui.filter) : NULL;

	/* a new pattern, as a search, and Esc keeps the one before */
	buf[0] = '\0';
	if (!prompt_live("filter files: ", buf, sizeof(buf), filter_changed)) {
		set_filter(before);
		free(before);
		return;
	}
	free(before);
	set_filter(buf);
	if (!ui.filter)
		set_msg(false, "filter cleared");
	else
		set_msg(false, "%zu file%s match%s /%s ({clear} clears it)",
			filter_count(), filter_count() == 1 ? "" : "s",
			filter_count() == 1 ? "es" : "", ui.filter);
}

/* Ctrl-F lists every hit as "path:n12: text", to jump to. */
void act_search_list(void)
{
	char **lines, title[300];
	struct dpane *dp;
	long start, k;
	bool wrapped;
	size_t i;

	if (!ui.search || !*ui.search) {
		act_search();
		if (!ui.search || !*ui.search)
			return;
	}
	dp = search_pane();
	if (!dp) {
		ui.active = P_UNSTAGED;
		dp = search_pane();
	}
	if (!ui.nhits) {
		set_miss(NOT_FOUND_MSG, sec_name(ui.hits_sec), ui.search);
		return;
	}
	lines = xcalloc(ui.nhits, sizeof(*lines));
	for (i = 0; i < ui.nhits; i++) {
		const struct hit *t = &ui.hits[i];
		struct strbuf sb = STRBUF_INIT;

		if (t->conflict)
			sb_addf(&sb, "%s:c%zu: %s", t->path, t->conflict,
				t->text);
		else
			sb_addf(&sb, "%s:%c%d: %c%s", t->path,
				t->new_no ? 'n' : 'o',
				t->new_no ? t->new_no : t->old_no, t->op,
				t->text);
		lines[i] = sb_detach(&sb);
	}
	snprintf(title, sizeof(title),
		 "%zu match%s for \"%.200s\" in %s "
		 "changes",
		 ui.nhits, ui.nhits == 1 ? "" : "es", ui.search,
		 sec_name(ui.hits_sec));
	start = hit_next(dp, 1, &wrapped);
	k = pick_list(title, lines, ui.nhits, start < 0 ? 0 : (size_t)start,
		      paint_hit);
	for (i = 0; i < ui.nhits; i++)
		free(lines[i]);
	free(lines);
	if (k >= 0)
		hit_jump((size_t)k);
}

/* Whether done ("op what") undid an index-only command on path. */
static bool undid_index_of(const char *done, const char *path)
{
	static const char *const ops[] = { "stage ",	  "unstage ",
					   "stage-file ", "unstage-file ",
					   "set-staged ", NULL };
	size_t i;

	for (i = 0; ops[i]; i++)
		if (starts_with(done, ops[i]))
			return !strcmp(done + strlen(ops[i]), path);
	return false;
}

/* u reverts the newest undo entry, and offers to force when things moved on. */
void act_undo(void)
{
	struct strbuf done = STRBUF_INIT;
	int rc;

	rc = undo_last(ui.r, false, &done);

	if (rc && strstr(sb_str(&ui.r->err), "--force")) {
		const char *why = sb_str(&ui.r->err);
		const char *hint = strstr(why, " (use --force");
		char q[200];

		/* without the CLI hint */
		snprintf(q, sizeof(q), "%.*s. Undo anyway? [y/N] ",
			 (int)(hint && hint - why < 150 ? hint - why : 150),
			 why);
		if (confirm(q)) {
			rc = undo_last(ui.r, true, &done);
		} else {
			set_msg(false, "not undone");
			return;
		}
	}
	if (rc)
		msg_repo_err();
	else
		set_msg(false, "undid %s", done.buf);
	/* a line or file of the file shown was staged or unstaged, that file */
	if (!rc && ui.path && undid_index_of(sb_str(&done), ui.path))
		reload_path(ui.path);
	else
		reload();
	sb_release(&done);
}

/*
 * Discard the selection of fd, or all of path for NULL, keeping a split's
 * target.
 */
static int take_out(const char *path, const struct fdiff *fd)
{
	struct split_take t;

	if (split_take_begin(ui.r, path, &t))
		return -1;
	if (fd ? discard_selection(ui.r, fd) : discard_file(ui.r, path)) {
		split_take_release(&t);
		return -1;
	}
	return split_take_end(ui.r, &t);
}

/* Whole-file discard of the selected file, after asking. */
static void discard_whole(void)
{
	const struct file_status *f = cur_fs();
	char q[600];
	char *from;

	from = f && f->from_y ? xstrdup(f->from_y) : NULL;

	if (from)
		snprintf(q, sizeof(q),
			 "Discard the rename %.200s -> %.200s? "
			 "(the new file is deleted, the old one back; u "
			 "undoes it) [y/N] ",
			 from, ui.path);
	else
		snprintf(q, sizeof(q),
			 "Discard all unstaged changes of "
			 "%.200s%s? ({undo} undoes it) [y/N] ",
			 ui.path,
			 ui.path_untracked ?
				 " (an untracked file: it is deleted)" :
				 "");
	if (!confirm(q)) {
		set_msg(false, "not discarded");
		free(from);
		return;
	}
	if (take_out(ui.path, NULL) || (from && take_out(from, NULL)))
		msg_repo_err();
	else
		set_msg(false, "discarded the changes of %s", ui.path);
	free(from);
	reload();
}

/*
 * d throws unstaged changes away, the selected lines in the Unstaged pane
 * or the whole file in the Files pane (Unstaged section).
 */
void act_discard(void)
{
	struct dpane *dp = active_dp();
	size_t a, b, n;
	char q[200];

	if (!ui.path) {
		set_msg(true, "no file selected");
		return;
	}
	if (dp == &ui.dp[1] || (!dp && ui.path_staged)) {
		set_msg(true,
			"discard works on unstaged changes (Unstaged pane "
			"or section); unstage first");
		return;
	}
	if (dp && !dp->fd && dp->cancelled) {
		set_msg(false, "the diff is not loaded: {load-diff} loads it");
		return;
	}
	if (!dp || dp->too_big || ui.path_untracked ||
	    (is_rename_entry(false) && !dp->fd) ||
	    (dp->fd && !fdiff_line_ops(dp->fd, NULL))) {
		discard_whole();
		return;
	}
	if (!dp->fd) {
		set_msg(true, "nothing to discard");
		return;
	}
	row_range(dp, &a, &b);
	n = mark_rows(dp, a, b);
	if (!n) {
		set_msg(true, "no changed line here");
		return;
	}
	snprintf(q, sizeof(q),
		 "Discard %zu changed line%s from the work tree? "
		 "({undo} undoes it) [y/N] ",
		 n, n == 1 ? "" : "s");
	if (!confirm(q)) {
		set_msg(false, "not discarded");
		return;
	}
	if (take_out(ui.path, dp->fd))
		msg_repo_err();
	else
		set_msg(false, "discarded %zu line%s", n, n == 1 ? "" : "s");
	dp->ranging = false;
	/* lines of one file, and in a split its target changes too */
	if (ui.split.active)
		reload();
	else
		reload_path(ui.path);
}

/*
 * O and T resolve a conflict by taking our or their side, for the one
 * under the cursor in the Unstaged pane or the whole file after asking.
 */
void act_resolve(bool theirs)
{
	const char *side = theirs ? "theirs" : "ours";
	size_t nc = 0, num = 0, total = 0;
	struct dpane *dp = active_dp();
	struct undo_state ub;
	int n = 0, rc;

	if (!ui.path || ui.fcur >= ui.nitems ||
	    !ui.fs[ui.items[ui.fcur].fi].unmerged) {
		set_msg(false,
			"{take-ours} / {take-theirs} resolve conflicted (U) files");
		return;
	}
	if (dp == &ui.dp[0] && dp->nrows && dp->rows[dp->cur].kind == R_NOTE)
		n = (int)dp->rows[dp->cur].h;
	if (!n) {
		char q[300];

		snprintf(q, sizeof(q),
			 "Take %s for all of %.200s? Resolutions "
			 "already made in it are lost [y/N] ",
			 side, ui.path);
		if (!confirm(q)) {
			set_msg(false, "not resolved");
			return;
		}
	}
	if (n) {
		size_t i;

		for (i = 0; i < dp->nrows; i++)
			nc += dp->rows[i].kind == R_NOTE &&
			      dp->rows[i].j == CROW_HEAD;
		num = conflict_number(ui.path, nc, (size_t)n - 1, &total);
	}
	if (undo_conflict_begin(ui.r, &ub, ui.path)) {
		msg_repo_err();
		return;
	}
	rc = resolve_conflict(ui.r, ui.path, n, theirs);
	undo_conflict_end(ui.r, &ub, ui.path);
	if (rc) {
		msg_repo_err();
	} else if (n) {
		/* numbered as first shown, not as what is left */
		conflict_resolved(ui.path, nc, (size_t)n - 1);
		set_msg(false, "took %s for conflict %zu of %zu in %s", side,
			num, total, ui.path);
	} else {
		set_msg(false, "took %s for %s", side, ui.path);
	}
	reload();
	/* tell the user the file is staged once resolved */
	if (!ui.msg.err && ui.fcur < ui.nitems &&
	    !ui.fs[ui.items[ui.fcur].fi].unmerged && ui.path &&
	    strlen(ui.msg.text) + 30 < sizeof(ui.msg.text)) {
		size_t len = strlen(ui.msg.text);

		snprintf(ui.msg.text + len, sizeof(ui.msg.text) - len,
			 "; resolved and staged");
	}
}

/*
 * ga absorbs. It shows the plan first (every change block, where it goes
 * or why it stays), and Enter does it. The series comes from
 * series_revs().
 */
void act_absorb(void)
{
	size_t n, i, nl = 0, len, targets = 0;
	struct strbuf plan = STRBUF_INIT;
	const char *pos, *end, *l;
	char title[200], **lines;
	struct absorb_hunk *v;
	int moved, rc;
	long k;

	if (ui.split.active || ui.series.op != OP_NONE) {
		set_msg(true, OP_BUSY_MSG);
		return;
	}
	if (absorb_plan(ui.r, NULL, &v, &n)) {
		msg_repo_err();
		return;
	}
	for (i = 0; i < n; i++)
		targets += v[i].target != NULL;
	absorb_describe(ui.r, v, n, &plan);
	if (!targets) {
		set_msg(true, n ? "nothing to absorb: no change block has one "
				  "clear commit" :
				  "nothing to absorb");
		sb_release(&plan);
		absorb_plan_free(v, n);
		return;
	}
	lines = xcalloc(n + 1, sizeof(*lines));
	pos = sb_str(&plan);
	end = pos + plan.len;
	while ((l = next_line(&pos, end, &len)) && nl < n)
		lines[nl++] = xstrndup(l, len);
	snprintf(title, sizeof(title), "Absorb %zu of %zu change blocks?",
		 targets, n);
	k = confirm_list(title, lines, nl, "absorb", paint_absorb) ? 0 : -1;
	for (i = 0; i < nl; i++)
		free(lines[i]);
	free(lines);
	sb_release(&plan);
	if (k < 0) {
		set_msg(false, "not absorbed");
		absorb_plan_free(v, n);
		return;
	}
	/* the plan just shown, nothing changed after it */
	rc = absorb_apply_plan(ui.r, NULL, v, n, &moved);
	absorb_plan_free(v, n);
	/* failed, or stopped at a conflict, and the core says which */
	if (rc)
		msg_repo_err();
	else
		set_msg(false, "absorbed %d change block%s", moved,
			moved == 1 ? "" : "s");
	show_warnings();
	reload();
}

/* Show text (a range-diff, say) in the overlay, to read and close. */
static void show_text(const char *title, const char *text, line_paint_fn paint)
{
	size_t n = 0, alloc = 0, len, i;
	const char *pos, *end, *l;
	char **lines = NULL;

	if (!text)
		return;
	pos = text;
	end = text + strlen(text);

	while ((l = next_line(&pos, end, &len))) {
		ALLOC_GROW(lines, n + 1, alloc);
		lines[n++] = xstrndup(l, len);
	}
	if (n)
		view_lines_paint(title, lines, n, paint);
	for (i = 0; i < n; i++)
		free(lines[i]);
	free(lines);
}

/*
 * The selected changed lines of dp (range, marks or the cursor line) as a
 * selection such as "o12,n12". False with a message when there are none.
 */
static bool sel_spec(struct dpane *dp, struct strbuf *spec)
{
	size_t a, b, h, j;

	row_range(dp, &a, &b);
	if (!mark_rows(dp, a, b)) {
		set_msg(true, "no changed line here");
		return false;
	}
	for (h = 0; h < dp->fd->nhunks; h++) {
		for (j = 0; j < dp->fd->hunks[h].nlines; j++) {
			const struct dline *d = &dp->fd->hunks[h].lines[j];

			if (!d->sel || d->op == ' ')
				continue;
			sb_addf(spec, "%s%c%d", spec->len ? "," : "",
				d->op == '-' ? 'o' : 'n',
				d->op == '-' ? d->old_no : d->new_no);
		}
	}
	return true;
}

/* D shows the commit shown, if one is, else HEAD. */
void act_show_commit(void)
{
	struct strbuf out = STRBUF_INIT, title = STRBUF_INIT;
	const char *c = ui.ccommit ? ui.ccommit : "HEAD";
	char *s = commit_line(c, false);

	if (!s) {
		set_msg(true, "no commit yet");
		return;
	}
	if (commit_show(ui.r, c, &out)) {
		msg_repo_err();
	} else {
		sb_addf(&title, "%s", s);
		show_text(title.buf, out.buf, paint_show);
	}
	free(s);
	sb_release(&out);
	sb_release(&title);
}

/* ---- the Series pane ----------------------------------------------------- */

/*
 * Show row of the Series pane (the work tree, or a commit): once the
 * cursor settles, as a file of the Files pane is (holding J does not load
 * every commit passed).
 */
void sv_select(long row)
{
	size_t n;

	if (row < 0)
		row = 0;
	sv_grow((size_t)row);
	n = sv_rows();
	if (!n)
		return;
	if ((size_t)row >= n)
		row = (long)n - 1;
	if ((size_t)row == ui.sv.sel)
		return;
	ui.sv.sel = (size_t)row;
	ui.sv.follow = false; /* chosen */
	ui.sv.stale = true;
	diffs_stale();
}

/* J and K show the next (older) or previous commit of the Series pane. */
static void browse_move(int dir)
{
	size_t before = ui.sv.sel;

	sv_select((long)ui.sv.sel + dir);
	if (ui.sv.sel == before)
		set_msg(false, dir > 0	 ? "the oldest commit" :
			       ui.sv.own ? "the work tree: at the top" :
					   "the tip: at the top");
}

/*
 * Enter in the Series pane shows the commit (or the work tree) and its
 * files.
 */
void series_goto(void)
{
	if (ui.sv.stale) {
		ui.sv.stale = false;
		reload();
	}
	ui.active = P_FILES;
}

/* R runs git rm on the selected file, after asking. */
void act_remove(void)
{
	char q[300];

	if (ui.active != P_FILES || !ui.path || ui.path_commit) {
		set_msg(false, "R removes a file: select it in the Files pane");
		return;
	}
	if (ui.split.active) {
		set_msg(true, SPLIT_BUSY_MOVE_MSG);
		return;
	}
	if (ui.path_untracked) {
		set_msg(false, "%s is not tracked: d deletes it", ui.path);
		return;
	}
	snprintf(q, sizeof(q),
		 "Delete %.200s and stage its deletion? (u undoes "
		 "it) [y/N] ",
		 ui.path);
	if (!confirm(q)) {
		set_msg(false, "not removed");
		return;
	}
	if (remove_file(ui.r, ui.path))
		msg_repo_err();
	else
		set_msg(false, "removed %s (staged)", ui.path);
	reload();
}

/* J shows the older commit of the Series pane, below. */
void act_next_commit(void)
{
	browse_move(1);
}

/* K shows the newer commit, above. */
void act_prev_commit(void)
{
	browse_move(-1);
}

/* gd loads the diff of the selected file, although it is big or skipped. */
void act_load_diff(void)
{
	if (!ui.path) {
		set_msg(true, "no file selected");
		return;
	}
	free(ui.force_path);
	ui.force_path = xstrdup(ui.path);
	free(ui.stopped_path);
	ui.stopped_path = NULL;
	diffs_stale();
	diffs_load();
}

/* gi puts an untracked file into .git/info/exclude and out of the list. */
void act_ignore(void)
{
	struct strbuf pat = STRBUF_INIT;
	char q[400];

	if (!ui.path || ui.path_commit || !ui.path_untracked) {
		set_msg(false, "{ignore} is for untracked files (a tracked "
			       "one: git rm --cached it first)");
		return;
	}
	exclude_pattern(ui.path, &pat);
	snprintf(q, sizeof(q), "Add %.200s to .git/info/exclude? [y/N] ",
		 pat.buf);
	sb_release(&pat);
	if (!confirm(q)) {
		set_msg(false, "not ignored");
		return;
	}
	if (exclude_path(ui.r, ui.path)) {
		msg_repo_err();
		return;
	}
	set_msg(false, "%s is ignored now (.git/info/exclude)", ui.path);
	reload();
	start_scan();
}

/* ---- changes to the commits of the Series pane --------------------------- */

/* The commit shown can be changed from here. */
static bool commit_ops_ok(void)
{
	if (!ui.ccommit)
		return false;
	if (ui.split.active || ui.series.op != OP_NONE) {
		set_msg(true, "finish ({finish}) or abort ({abort}) what is in "
			      "progress first");
		return false;
	}
	return true;
}

/*
 * Run a move. The editor, for a new commit's message or a new message,
 * gets the terminal. A conflict asks whether to resolve it now or give the
 * change up.
 */
static void run_move(const struct move_req *req, bool editor, struct dpane *dp)
{
	struct strbuf msg = STRBUF_INIT;
	int rc;

	if (editor)
		suspend_curses();
	rc = move_lines(ui.r, req, &msg);
	if (editor)
		resume_curses();
	if (rc == 1) {
		reload();
		draw();
		if (confirm("The change conflicts with a later commit. Resolve "
			    "it now? (n: give it up, nothing changes) [y/N] ")) {
			set_msg(true, "resolve the conflict, stage it, then "
				      "{finish} goes on ({abort} gives up)");
		} else if (move_abort(ui.r)) {
			msg_repo_err();
		} else {
			set_msg(false, "given up: nothing changed");
		}
	} else if (rc) {
		msg_repo_err();
	} else {
		set_msg(false, "%s", msg.buf);
	}
	show_warnings();
	if (dp)
		dp->ranging = false;
	sb_release(&msg);
	reload();
}

/*
 * A change to the commit shown. Its selected lines (in the Files pane, the
 * file) are taken out, dropped, or moved into dest. A conflict asks
 * whether to resolve it now or give the change up.
 */
static void commit_move(enum move_how how, const char *commit, const char *dest)
{
	struct dpane *dp = ui.active == P_FILES ? NULL : active_dp();
	bool editor = how == MOVE_NEW || how == MOVE_REWORD;
	struct strbuf spec = STRBUF_INIT;
	struct commit_opts co = { 0 };
	struct move_req req = { 0 };

	if (how != MOVE_REWORD && !commit_ops_ok())
		goto out;
	if (how != MOVE_REWORD) {
		if (!ui.path_commit) {
			set_msg(true, "select lines of the commit's diff, or "
				      "its file in the Files pane");
			goto out;
		}
		if (commit_from(ui.path)) {
			set_msg(true,
				"%s is a rename in this commit: split it "
				"({split-older}) to move the rename",
				ui.path);
			goto out;
		}
		if (dp && (!dp->fd || !fdiff_line_ops(dp->fd, NULL)))
			dp = NULL; /* binary or a mode change only, the whole file */
		if (dp && !sel_spec(dp, &spec))
			goto out;
	}
	req.how = how;
	req.commit = commit;
	req.path = ui.path;
	req.sel = spec.len ? spec.buf : NULL;
	req.dest = dest;
	co.quiet = true;
	co.signoff = ui.signoff;
	req.co = &co;
	run_move(&req, editor, dp);
out:
	sb_release(&spec);
}

/*
 * Space or Enter on the commit's lines takes them out of it into the work
 * tree.
 */
void act_commit_out(void)
{
	commit_move(MOVE_OUT, ui.ccommit, NULL);
}

/* d on the commit's lines takes them out of it and of the work tree. */
void act_commit_drop(void)
{
	struct dpane *dp = ui.active == P_FILES ? NULL : active_dp();
	struct strbuf spec = STRBUF_INIT;
	char q[300];

	if (!commit_ops_ok())
		return;
	if (dp && dp->fd && fdiff_line_ops(dp->fd, NULL) && dp->nmarked == 0 &&
	    !dp->ranging)
		snprintf(q, sizeof(q),
			 "Drop the line from the commit (and "
			 "the work tree)? ({undo} undoes it) [y/N] ");
	else
		snprintf(
			q, sizeof(q),
			"Drop the selected lines (or the file) "
			"from the commit and the work tree? ({undo} undoes it) "
			"[y/N] ");
	sb_release(&spec);
	if (!confirm(q)) {
		set_msg(false, "not dropped");
		return;
	}
	commit_move(MOVE_DROP, ui.ccommit, NULL);
}

/*
 * f on the commit's lines moves them into another commit of the series, or
 * into a new commit in one of the gaps between them.
 */
void act_commit_move_to(void)
{
	const struct sview *s = &ui.sv;
	size_t n = 0, cur = 0, k, cap;
	char *dest = NULL;
	char **lines;
	bool *gap;
	long pick;

	if (!commit_ops_ok() || !ui.path_commit)
		return;
	/* every commit of the range, not just those the pane has shown */
	sv_load_range();
	cap = 2 * s->n + 2;
	lines = xcalloc(cap, sizeof(*lines));
	gap = xcalloc(cap, sizeof(*gap));
	/* a gap above every commit, for a new commit after the one below */
	for (k = 0; k <= s->n; k++) {
		struct strbuf l = STRBUF_INIT;
		char lab[32];

		if (k == 0)
			sb_adds(&l, "   -- a new commit on top --");
		else
			sb_addf(&l, "   -- a new commit before %.*s --",
				(int)strcspn(s->lines[k - 1], " "),
				s->lines[k - 1]);
		gap[n] = true;
		lines[n++] = sb_detach(&l);
		if (k == s->n)
			break;
		sv_label(k, lab, sizeof(lab));
		sb_addf(&l, "%c %-5s %s",
			!strcmp(s->ids[k], ui.ccommit) ? '*' : ' ', lab,
			s->lines[k]);
		if (!strcmp(s->ids[k], ui.ccommit))
			cur = n;
		lines[n++] = sb_detach(&l);
	}
	pick = pick_list("Move the lines into which commit? (a -- line: a new "
			 "commit there)",
			 lines, n, cur, NULL);
	if (pick >= 0) {
		/* the row's commit, for a gap the commit below it */
		size_t row = (size_t)pick, c = 0, i;

		for (i = 0; i < row; i++)
			c += !gap[i];
		if (!gap[row] || c < s->n) {
			dest = xstrdup(s->ids[c]);
		} else {
			/* below the last loaded one, after its parent */
			struct strbuf spec = STRBUF_INIT;

			sb_addf(&spec, "%s^", s->ids[s->n - 1]);
			dest = repo_rev(ui.r, spec.buf);
			sb_release(&spec);
		}
		if (!dest)
			set_msg(true, "no commit below that one");
		else if (!gap[row] && !strcmp(dest, ui.ccommit))
			set_msg(false, "the lines are in that commit already");
		else
			commit_move(gap[row] ? MOVE_NEW : MOVE_INTO, ui.ccommit,
				    dest);
	}
	for (k = 0; k < n; k++)
		free(lines[k]);
	free(lines);
	free(gap);
	free(dest);
}

/* r opens the message of the commit shown (or HEAD's) in the editor. */
void act_reword(void)
{
	char *head;

	if (ui.ccommit) {
		if (commit_ops_ok())
			commit_move(MOVE_REWORD, ui.ccommit, NULL);
		return;
	}
	head = repo_rev(ui.r, "HEAD");
	if (!head)
		set_msg(true, "no commit yet");
	else
		commit_move(MOVE_REWORD, head, NULL);
	free(head);
}

/* e on the commit's lines edits them as the commit has them. */
void act_commit_edit(void)
{
	struct strbuf spec = STRBUF_INIT, blob = STRBUF_INIT,
		      text = STRBUF_INIT, content = STRBUF_INIT;
	struct region rg = { 0, 0, STRBUF_INIT };
	struct dpane *dp = active_dp();
	struct move_req req = { 0 };

	if (!commit_ops_ok())
		return;
	if (!dp || !dp->commit || !dp->fd || !fdiff_line_ops(dp->fd, NULL)) {
		set_msg(false, "{edit} on a commit: select lines of its diff");
		return;
	}
	if (!sel_spec(dp, &spec))
		goto out;
	if (commit_region(ui.r, ui.ccommit, ui.path, spec.buf, &rg, &blob)) {
		msg_repo_err();
		goto out;
	}
	if (edit_text(base_name(ui.path), rg.text.buf, rg.text.len, &text))
		goto out;
	if (text.len == rg.text.len &&
	    !memcmp(sb_str(&text), sb_str(&rg.text), text.len)) {
		set_msg(false, "nothing changed");
		goto out;
	}
	splice_lines(sb_str(&blob), blob.len, rg.start, rg.count, sb_str(&text),
		     text.len, &content);
	req.how = MOVE_EDIT;
	req.commit = ui.ccommit;
	req.path = ui.path;
	req.content = sb_str(&content);
	req.len = content.len;
	run_move(&req, false, dp);
out:
	region_release(&rg);
	sb_release(&spec);
	sb_release(&blob);
	sb_release(&text);
	sb_release(&content);
}

/* ZZ finishes what is in progress, whatever it is. */
void act_finish(void)
{
	if (ui.split.active)
		act_split(); /* the finish of a split */
	else if (ui.series.op != OP_NONE)
		act_series_continue();
	else
		set_msg(true, "nothing in progress to finish");
}

/*
 * e in the message pane opens the message in your editor. Without one it
 * starts from what c would start the editor with. Left as it was, or with
 * nothing but comments, there is no message.
 */
void act_message_edit(void)
{
	const char *path = repo_git_path(ui.r, DRAFT_FILE);
	struct strbuf t = STRBUF_INIT, m = STRBUF_INIT;
	bool fresh = !ui.draft.len;
	int rc;

	if (!path) {
		msg_repo_err();
		return;
	}
	if (fresh && (commit_template(ui.r, ui.signoff, &t) ||
		      draft_write(ui.r, t.buf, t.len))) {
		msg_repo_err();
		goto out;
	}
	suspend_curses();
	rc = run_editor(ui.r, path, 0);
	resume_curses();
	if (rc)
		msg_repo_err();
	load_draft();
	message_strip(sb_str(&ui.draft), commit_comment_char(ui.r), &m);
	if (!m.len || (fresh && ui.draft.len == t.len &&
		       !memcmp(sb_str(&ui.draft), sb_str(&t), t.len))) {
		draft_remove(ui.r);
		load_draft();
		set_msg(false, "no message written");
	} else if (!rc) {
		set_msg(false, "message kept: {commit} commits with it");
	}
out:
	sb_release(&t);
	sb_release(&m);
}
