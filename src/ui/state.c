// SPDX-License-Identifier: MIT
/*
 * Loading the status and the diffs, and the background scan for untracked
 * files.
 */
#include "state.h"
#include "ui_int.h"
#include "../message.h"
#include "actions.h"
#include "dcache.h"
#include "draw.h"
#include "sview.h"
#include "../conflict.h"
#include "../index.h"
#include "../rebase.h"
#include "../rename.h"
#include "../undo.h"

static void dp_push(struct dpane *dp, enum row_kind k, size_t h, size_t j,
		    const char *note);

/* ---- loading ---------------------------------------------------------- */

void dp_clear(struct dpane *dp)
{
	size_t i;

	fdiff_free(dp->fd);
	free(dp->fd);
	dp->fd = NULL;
	dp->nrows = 0;
	dp->nmarked = 0;
	for (i = 0; i < dp->nctext; i++)
		free(dp->ctext[i]);
	free(dp->ctext);
	dp->ctext = NULL;
	dp->nctext = 0;
}

static const char *ctext_add(struct dpane *dp, char *s)
{
	dp->ctext = xrealloc(dp->ctext, (dp->nctext + 1) * sizeof(char *));
	dp->ctext[dp->nctext++] = s;
	return s;
}

static void crow_add(struct crow **v, size_t *n, size_t *alloc, char *text,
		     int style, size_t num)
{
	ALLOC_GROW(*v, *n + 1, *alloc);
	(*v)[*n].text = text;
	(*v)[*n].style = style;
	(*v)[*n].num = num;
	(*n)++;
}

/* One side of a conflict, a row per line prefixed with mark. */
static void conflict_side(struct crow **v, size_t *n, size_t *alloc, size_t num,
			  int style, char mark, const char *s, size_t len)
{
	const char *pos = s, *end = s + len, *l;
	size_t ll;

	while ((l = next_line(&pos, end, &ll))) {
		struct strbuf sb = STRBUF_INIT;

		if (ll && l[ll - 1] == '\r')
			ll--;
		sb_addf(&sb, "%c %.*s", mark, (int)ll, l);
		crow_add(v, n, alloc, sb_detach(&sb), style, num);
	}
}

/*
 * The numbers conflicts had when the file was first shown. Resolving
 * conflict 1 of 2 leaves the other one "conflict 2 of 2", not a new
 * "conflict 1 of 1". They are kept while the file's count matches what O
 * and T left, and any other change, such as an editor or a new merge,
 * starts afresh.
 */
static struct cnum *cnum_get(const char *path, size_t nc)
{
	struct cnum *c = NULL;
	size_t i, left;

	for (i = 0; i < ui.ncnums; i++)
		if (!strcmp(ui.cnums[i].path, path))
			c = &ui.cnums[i];
	if (!c) {
		ui.cnums =
			xrealloc(ui.cnums, (ui.ncnums + 1) * sizeof(*ui.cnums));
		c = &ui.cnums[ui.ncnums++];
		c->path = xstrdup(path);
		c->total = 0;
		c->done = NULL;
	}
	for (left = 0, i = 0; i < c->total; i++)
		left += !c->done[i];
	if (left != nc || !nc) {
		free(c->done);
		c->total = nc;
		c->done = xcalloc(nc ? nc : 1, sizeof(*c->done));
	}
	return c;
}

size_t conflict_number(const char *path, size_t nc, size_t i, size_t *total)
{
	const struct cnum *c = cnum_get(path, nc);
	size_t k, n = i;

	*total = c->total;
	for (k = 0; k < c->total; k++) {
		if (c->done[k])
			continue;
		if (!n--)
			return k + 1;
	}
	*total = nc;
	return i + 1;
}

void conflict_resolved(const char *path, size_t nc, size_t i)
{
	struct cnum *c = cnum_get(path, nc);
	size_t total;
	size_t k;

	k = conflict_number(path, nc, i, &total);

	if (k && k <= c->total)
		c->done[k - 1] = true;
}

void cnums_free(void)
{
	size_t i;

	for (i = 0; i < ui.ncnums; i++) {
		free(ui.cnums[i].path);
		free(ui.cnums[i].done);
	}
	free(ui.cnums);
	ui.cnums = NULL;
	ui.ncnums = 0;
}

size_t conflict_view(const char *path, struct crow **out)
{
	struct strbuf data = STRBUF_INIT, sb = STRBUF_INIT;
	size_t n = 0, nc = 0, alloc = 0, i, total, num;
	struct conflict *c = NULL;

	*out = NULL;
	if (!read_file(path, &data))
		nc = conflicts_parse(sb_str(&data), data.len, &c);
	if (!nc) {
		/* the keys as bound, since notes are drawn as they are */
		keys_subst(
			&ui.keys,
			"(unmerged, no conflict markers in the "
			"file: {take-ours} / {take-theirs} in the Files pane "
			"take a whole side)",
			&sb, false);
		crow_add(out, &n, &alloc, sb_detach(&sb), CROW_NOTE, 0);
		goto done;
	}
	conflict_number(path, nc, 0, &total);
	if (total > nc)
		sb_addf(&sb, "(unmerged: %zu of %zu conflicts left; ", nc,
			total);
	else
		sb_addf(&sb, "(unmerged: %zu conflict%s; ", nc,
			nc == 1 ? "" : "s");
	keys_subst(&ui.keys,
		   "{take-ours} / {take-theirs} take ours / theirs "
		   "for the one under the cursor)",
		   &sb, false);
	crow_add(out, &n, &alloc, sb_detach(&sb), CROW_NOTE, 0);
	for (i = 0; i < nc; i++) {
		const struct conflict *k = &c[i];
		const char *d = sb_str(&data);

		num = conflict_number(path, nc, i, &total);
		sb_addf(&sb,
			"conflict %zu of %zu, line %d: < ours %s, > "
			"theirs %s",
			num, total, k->line, k->ours_label, k->theirs_label);
		crow_add(out, &n, &alloc, sb_detach(&sb), CROW_HEAD, i + 1);
		conflict_side(out, &n, &alloc, i + 1, CROW_OURS, '<',
			      d + k->ours, k->ours_end - k->ours);
		conflict_side(out, &n, &alloc, i + 1, CROW_BASE, '|',
			      d + k->base, k->base_end - k->base);
		conflict_side(out, &n, &alloc, i + 1, CROW_THEIRS, '>',
			      d + k->theirs, k->theirs_end - k->theirs);
	}
done:
	conflicts_free(c, nc);
	sb_release(&data);
	sb_release(&sb);
	return n;
}

void crows_free(struct crow *v, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		free(v[i].text);
	free(v);
}

/*
 * The Unstaged pane of an unmerged file shows its conflict regions from
 * the work tree, which O and T resolve one at a time.
 */
static void conflict_rows(struct dpane *dp, const char *path)
{
	struct crow *v;
	size_t n;
	size_t i;

	n = conflict_view(path, &v);

	/* the pane keeps the texts (ctext), and the array goes */
	for (i = 0; i < n; i++)
		dp_push(dp, R_NOTE, v[i].num, (size_t)v[i].style,
			ctext_add(dp, v[i].text));
	free(v);
}

static void dp_push(struct dpane *dp, enum row_kind k, size_t h, size_t j,
		    const char *note)
{
	ALLOC_GROW(dp->rows, dp->nrows + 1, dp->arows);
	dp->rows[dp->nrows++] = (struct row){ k, h, j, note, false };
}

/* The old path of a rename of the commit shown, else NULL. */
const char *commit_from(const char *path)
{
	size_t i;

	for (i = 0; path && i < ui.ncfiles; i++)
		if (!strcmp(ui.cfiles[i].path, path))
			return ui.cfiles[i].from;
	return NULL;
}

/* The load in progress, told in the status line once it takes a while. */
static bool load_said;

static void load_progress(void *ctx, size_t bytes)
{
	const char *path = ctx;

	if (bytes >= (1 << 20))
		set_msg(false,
			"loading the diff of %s: %zu MB read "
			"(any key stops it)",
			path, bytes >> 20);
	else
		set_msg(false, "loading the diff of %s (any key stops it)",
			path);
	load_said = true;
	draw();
}

/* "48.2 MB" */
static void human_size(long n, char *buf, size_t len)
{
	if (n >= 1 << 20)
		snprintf(buf, len, "%.1f MB", (double)n / (1 << 20));
	else
		snprintf(buf, len, "%ld KB", (n + 1023) / 1024);
}

/*
 * Why the diff of path is not loaded, as a note into buf, or false. Its
 * name matches carve.skipDiff, or the diff is a whole file (new,
 * untracked, deleted) larger than carve.maxFileSize. Only the size of the
 * file is looked at, never its content. load-diff overrides both.
 */
static bool skip_note(const char *path, bool cached, char *buf, size_t len)
{
	struct strbuf t = STRBUF_INIT, spec = STRBUF_INIT;
	const struct file_status *f;
	char size[32];
	long n = -1;
	size_t i;

	if (ui.force_path && !strcmp(ui.force_path, path))
		return false;
	if (path_matches(path, ui.cfg.skip, ui.cfg.nskip)) {
		keys_subst(&ui.keys,
			   "(not loaded: carve.skipDiff names it; "
			   "{load-diff} loads it, {ignore} ignores the file)",
			   &t, false);
		goto out;
	}
	if (ui.cfg.max_file_size <= 0)
		return false;
	if (ui.path_commit && !cached) {
		for (i = 0; i < ui.ncfiles; i++) {
			if (strcmp(ui.cfiles[i].path, path))
				continue;
			if (ui.cfiles[i].status == 'A')
				sb_addf(&spec, "%s:%s", ui.ccommit, path);
			else if (ui.cfiles[i].status == 'D')
				sb_addf(&spec, "%s^:%s", ui.ccommit, path);
		}
	} else if ((f = cur_fs())) {
		if (!cached && (f->y == '?' || f->y == 'A'))
			n = worktree_size(path);
		else if ((!cached && f->y == 'D') || (cached && f->x == 'A'))
			sb_addf(&spec, ":%s", path);
		else if (cached && f->x == 'D')
			sb_addf(&spec, "HEAD:%s", path);
	}
	if (spec.len)
		n = repo_size(ui.r, spec.buf);
	sb_release(&spec);
	if (n <= ui.cfg.max_file_size)
		return false;
	human_size(n, size, sizeof(size));
	sb_addf(&spec,
		"(a whole file of %s, over carve.maxFileSize: "
		"{load-diff} loads it, {stage} takes the whole file)",
		size);
	keys_subst(&ui.keys, spec.buf, &t, false);
	sb_release(&spec);
out:
	snprintf(buf, len, "%s", t.buf);
	sb_release(&t);
	return true;
}

/* The note for a load stopped early. */
static void stopped_note(struct dpane *dp, enum run_stop stop)
{
	struct strbuf t = STRBUF_INIT, f = STRBUF_INIT;

	if (stop == RUN_LIMIT) {
		dp->too_big = true;
		sb_addf(&f,
			"(diff of more than %ld lines, over "
			"carve.diffLimit; {stage} takes the whole file, "
			"{load-diff} loads it anyway)",
			ui.cfg.diff_limit);
	} else {
		dp->cancelled = true;
		if (dp->fd == NULL && ui.path &&
		    (!ui.stopped_path || strcmp(ui.stopped_path, ui.path))) {
			free(ui.stopped_path);
			ui.stopped_path = xstrdup(ui.path);
		}
		sb_adds(&f, "(loading stopped by a key: {load-diff} loads it)");
		ui.stopped_by_key = true;
	}
	keys_subst(&ui.keys, f.buf, &t, false);
	snprintf(dp->big_note, sizeof(dp->big_note), "%s", t.buf);
	dp_push(dp, R_NOTE, 0, 0, dp->big_note);
	sb_release(&t);
	sb_release(&f);
}

/*
 * The rows of dp's diff of path. Those are a conflict's own, or the hunks
 * and lines with the notes that go with them, such as a copy or no line
 * operations.
 */
static void dp_rows(struct dpane *dp, const char *path, bool cached,
		    bool renamed)
{
	const char *why;
	size_t h, j;

	if (dp->fd->combined && !cached) {
		conflict_rows(dp, path);
		return;
	}
	if (dp->fd->is_new && !renamed && !dp->commit) {
		/*
		 * an exact copy of a file in HEAD is still a new file,
		 * with a note
		 */
		char *c = copy_source(ui.r, path, cached, dp->fd);

		if (c) {
			struct strbuf sb = STRBUF_INIT;

			sb_addf(&sb, "(new file, a copy of %s)", c);
			dp_push(dp, R_NOTE, 0, CROW_NOTE,
				ctext_add(dp, sb_detach(&sb)));
			free(c);
		}
	}
	if (!fdiff_line_ops(dp->fd, &why))
		dp_push(dp, R_NOTE, 0, 0, why);
	for (h = 0; h < dp->fd->nhunks; h++) {
		dp_push(dp, R_HUNK, h, 0, NULL);
		for (j = 0; j < dp->fd->hunks[h].nlines; j++)
			dp_push(dp, R_LINE, h, j, NULL);
	}
}

void dp_push_note(struct dpane *dp, const char *note)
{
	dp_push(dp, R_NOTE, 0, 0, note);
}

bool dp_preview(struct dpane *dp, const char *path, bool cached)
{
	const char *why;
	size_t h, j;

	if (!dcache_get(path, cached, &dp->fd))
		return false;
	if (dp->fd->combined) {
		dp_push(dp, R_NOTE, 0, 0, "(conflicts)");
		return true;
	}
	if (!fdiff_line_ops(dp->fd, &why))
		dp_push(dp, R_NOTE, 0, 0, why);
	for (h = 0; h < dp->fd->nhunks; h++) {
		dp_push(dp, R_HUNK, h, 0, NULL);
		for (j = 0; j < dp->fd->hunks[h].nlines; j++)
			dp_push(dp, R_LINE, h, j, NULL);
	}
	return true;
}

void dp_load(struct dpane *dp, const char *path, bool cached)
{
	bool forced = path && ui.force_path && !strcmp(ui.force_path, path);
	/* while loading, git stops at the line limit or at a key */
	struct diff_opts lo = ui.dopts;
	enum run_stop stop = RUN_DONE;
	const struct file_status *f;
	const char *from = NULL;
	size_t h;
	int rc;

	lo.lim.max_lines = ui.cfg.diff_limit > 0 && !forced ?
				   (size_t)ui.cfg.diff_limit :
				   0;
	lo.lim.cancel_on_tty = true;
	lo.lim.progress = load_progress;
	lo.lim.progress_ctx = (void *)path;
	lo.lim.stopped = &stop;
	dp_clear(dp);
	dp->cached = cached;
	dp->too_big = false;
	dp->cancelled = false;
	dp->commit = false;
	dp->letter = 0;
	if (!path) {
		dp_push(dp, R_NOTE, 0, 0, "(no file selected)");
		goto clamp;
	}
	if (!forced && ui.stopped_path && !strcmp(ui.stopped_path, path)) {
		stopped_note(dp, RUN_CANCELLED);
		goto clamp;
	}
	/* the commit's diff of the file, titled as one also when not loaded */
	dp->commit = ui.path_commit && !cached;
	if (skip_note(path, cached, dp->big_note, sizeof(dp->big_note))) {
		dp->too_big = true;
		dp_push(dp, R_NOTE, 0, 0, dp->big_note);
		goto clamp;
	}
	if (ui.path_commit && !cached) {
		if (load_commit_diff_from(
			    ui.r, ui.ccommit ? ui.ccommit : "HEAD", path,
			    commit_from(path), &lo, &dp->fd)) {
			msg_repo_err();
			dp_push(dp, R_NOTE, 0, 0, "(cannot load diff)");
			goto clamp;
		}
		if (stop != RUN_DONE) {
			stopped_note(dp, stop);
			goto clamp;
		}
		if (!dp->fd) {
			dp_push(dp, R_NOTE, 0, 0,
				"(the commit does not change this file)");
			goto clamp;
		}
		goto rows;
	}
	/*
	 * An untracked file just deleted (discarded) is listed until the
	 * background scan comes back, which is not an error.
	 */
	if (ui.path_untracked && !path_in_worktree(path)) {
		dp_push(dp, R_NOTE, 0, 0,
			cached ? "(no staged changes)" : "(deleted)");
		goto clamp;
	}
	f = cur_fs();
	from = !f ? NULL : cached ? f->from_x : f->from_y;
	/*
	 * What the status just read says, unless a command changed the
	 * file since (reload_path loads the diffs to learn its status):
	 * nothing staged, nothing unstaged, untracked, tracked
	 */
	if (f && !ui.status_stale && !f->unmerged && !from &&
	    (cached ? f->x == ' ' : f->y == ' ') && status_still()) {
		dp->letter = ' ';
		dp_push(dp, R_NOTE, 0, 0,
			cached ? "(no staged changes)" :
				 "(no unstaged changes)");
		goto clamp;
	}
	lo.untracked = f && !ui.status_stale && f->y == '?';
	lo.in_index = f && f->y != '?' && f->x != 'D';
	/* a rename's content changes, as a diff of the new path */
	if (from) {
		rc = cached ? staged_rename_diff(ui.r, from, path, &lo,
						 &dp->fd) :
			      rename_diff(ui.r, from, path, &lo, &dp->fd);
	} else if (!lo.untracked && !ui.status_stale &&
		   dcache_get(path, cached, &dp->fd)) {
		rc = 0; /* from the diff cache, no git */
	} else {
		time_t since = time(NULL);

		rc = load_diff_opts(ui.r, path, cached, &lo, &dp->fd);
		if (!rc && stop == RUN_DONE && !lo.untracked &&
		    !ui.status_stale)
			dcache_put(path, cached, dp->fd, since);
	}
	if (rc) {
		msg_repo_err();
		dp_push(dp, R_NOTE, 0, 0, "(cannot load diff)");
		goto clamp;
	}
	if (stop != RUN_DONE) {
		stopped_note(dp, stop);
		goto clamp;
	}
	if (!from)
		dp->letter = !dp->fd		? ' ' :
			     dp->fd->is_new	? 'A' :
			     dp->fd->is_deleted ? 'D' :
						  'M';
	if (!dp->fd) {
		if (from)
			dp_push(dp, R_NOTE, 0, 0,
				cached ?
					"(renamed, no content changes: Enter in "
					"Files unstages both halves)" :
					"(renamed, no content changes: Space "
					"stages the move)");
		else
			dp_push(dp, R_NOTE, 0, 0,
				cached ? "(no staged changes)" :
					 "(no unstaged changes)");
		goto clamp;
	}
rows:
	if (ui.cfg.diff_limit > 0 && !forced) {
		long total = 0;

		for (h = 0; h < dp->fd->nhunks; h++)
			total += (long)dp->fd->hunks[h].nlines;
		if (total > ui.cfg.diff_limit) {
			fdiff_free(dp->fd);
			free(dp->fd);
			dp->fd = NULL;
			stopped_note(dp, RUN_LIMIT);
			goto clamp;
		}
	}
	dp_rows(dp, path, cached, from != NULL);
clamp:
	if (dp->cur >= dp->nrows)
		dp->cur = dp->nrows ? dp->nrows - 1 : 0;
	if (dp->anchor >= dp->nrows)
		dp->anchor = dp->cur;
	if (load_said && starts_with(ui.msg.text, "loading the diff of"))
		msg_clear();
	load_said = false;
}

/* The section of the selected file. */
static enum sec path_sec(void)
{
	return ui.path_commit ? SEC_COMMIT :
	       ui.path_staged ? SEC_STAGED :
				SEC_UNSTAGED;
}

/* "unstaged", "staged": in messages */
const char *sec_name(enum sec s)
{
	return s == SEC_STAGED ? "staged" :
	       s == SEC_COMMIT ? "commit" :
				 "unstaged";
}

static void push_item(bool header, enum sec sec, size_t fi, size_t dirlen,
		      bool untracked)
{
	ALLOC_GROW(ui.items, ui.nitems + 1, ui.aitems);
	ui.items[ui.nitems++] =
		(struct fitem){ header, sec, fi, dirlen, untracked };
}

/* The path of entry fi of a section. */
static const char *sec_path(enum sec sec, size_t fi)
{
	return sec == SEC_COMMIT ? ui.cfiles[fi].path : ui.fs[fi].path;
}

/* The length of path's directory with its "/", 0 at the top. */
static size_t dir_len(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? (size_t)(slash - path) + 1 : 0;
}

static enum sec sort_sec;

/* By directory, the top first, then by name, for one row per directory. */
static int dir_cmp(const void *pa, const void *pb)
{
	const char *a = sec_path(sort_sec, *(const size_t *)pa);
	const char *b = sec_path(sort_sec, *(const size_t *)pb);
	size_t la = dir_len(a), lb = dir_len(b);
	int c;

	if (!la != !lb)
		return la ? 1 : -1;
	c = strncmp(a, b, la < lb ? la : lb);
	if (c || la == lb)
		return c ? c : strcmp(a + la, b + lb);
	return la < lb ? -1 : 1;
}

/*
 * A section, its row and then its files grouped by directory, each group
 * under a row naming the directory. Files at the top come first, without
 * one. The folded Untracked section has its row only.
 */
static void push_section(enum sec sec, const size_t *fi, size_t n,
			 bool untracked)
{
	size_t *v = xcalloc(n + 1, sizeof(*v)), i;
	const char *last = NULL;
	size_t lastlen = 0;

	if (!n) {
		free(v);
		return;
	}
	memcpy(v, fi, n * sizeof(*v));
	sort_sec = sec;
	qsort(v, n, sizeof(*v), dir_cmp);
	push_item(true, sec, 0, 0, untracked);
	for (i = 0; i < n && (!untracked || untracked_shown()); i++) {
		const char *p = sec_path(sec, v[i]);
		size_t dl = dir_len(p);

		if (dl && (!last || dl != lastlen || strncmp(p, last, dl)))
			push_item(true, sec, v[i], dl, untracked);
		push_item(false, sec, v[i], dl, untracked);
		last = p;
		lastlen = dl;
	}
	free(v);
}

static bool split_adds(const char *path);

/* Whether a file, or either path of a rename, passes the Files filter. */
static bool filter_match(const char *path, const char *from)
{
	if (!ui.filter || !*ui.filter)
		return true;
	return smart_match(path, strlen(path), ui.filter) ||
	       (from && smart_match(from, strlen(from), ui.filter));
}

/*
 * The Files list in git status's order. The commit shown, Staged, Unstaged
 * (tracked files, and the new files of a commit being split), and
 * Untracked, folded at first.
 */
static void build_items(void)
{
	size_t *fi = xcalloc(ui.nfs + ui.ncfiles + 1, sizeof(*fi));
	size_t i, n;
	int pass;

	ui.nitems = 0;
	/* the commit shown first */
	for (i = 0, n = 0; i < ui.ncfiles; i++)
		if (filter_match(ui.cfiles[i].path, ui.cfiles[i].from))
			fi[n++] = i;
	push_section(SEC_COMMIT, fi, n, false);
	for (pass = 0; pass < 3; pass++) {
		for (i = 0, n = 0; i < ui.nfs; i++) {
			const struct file_status *f = &ui.fs[i];
			bool untracked;

			untracked = f->y == '?' && !f->unmerged &&
				    !(ui.nsfiles && split_adds(f->path));

			if (!filter_match(f->path,
					  f->from_x ? f->from_x : f->from_y))
				continue;
			if (pass == 0 ? fs_staged(f) :
			    pass == 1 ? fs_unstaged(f) && !untracked :
					untracked)
				fi[n++] = i;
		}
		if (pass == 2)
			ui.nuntracked = n;
		push_section(pass == 0 ? SEC_STAGED : SEC_UNSTAGED, fi, n,
			     pass == 2);
	}
	free(fi);
}

/*
 * The files (indexes into fs) of the section sec, or of the Untracked one
 * with untracked, that pass the filter, also when the section is folded.
 */
size_t section_files(enum sec sec, bool untracked, size_t **out)
{
	size_t *v = xcalloc(ui.nfs + 1, sizeof(*v)), i, n = 0;

	for (i = 0; i < ui.nfs; i++) {
		const struct file_status *f = &ui.fs[i];
		bool u = f->y == '?' && !f->unmerged &&
			 !(ui.nsfiles && split_adds(f->path));

		if (!filter_match(f->path, f->from_x ? f->from_x : f->from_y))
			continue;
		if (sec == SEC_STAGED ? fs_staged(f) :
		    untracked	      ? u :
					fs_unstaged(f) && !u)
			v[n++] = i;
	}
	*out = v;
	return n;
}

/* The files that pass the filter (each once, staged and unstaged too). */
size_t filter_count(void)
{
	size_t i, n = 0;

	for (i = 0; i < ui.nfs; i++)
		n += filter_match(ui.fs[i].path, ui.fs[i].from_x ?
							 ui.fs[i].from_x :
							 ui.fs[i].from_y);
	for (i = 0; i < ui.ncfiles; i++)
		n += filter_match(ui.cfiles[i].path, ui.cfiles[i].from);
	return n;
}

/* Whether the Untracked section lists its files, when unfolded or filtered. */
bool untracked_shown(void)
{
	return ui.untracked_open || (ui.filter && *ui.filter);
}

/*
 * Fold (how < 0), unfold (how > 0) or toggle (0) the Untracked section.
 * The cursor stays on its row, which does not move.
 */
void fold_untracked(int how)
{
	bool open = how > 0 || (!how && !ui.untracked_open);

	if (open == ui.untracked_open)
		return;
	ui.untracked_open = open;
	build_items();
}

/* Whether an untracked file's path matches pattern (smart case). */
bool untracked_match(const char *pattern)
{
	size_t i;

	for (i = 0; i < ui.nfs; i++)
		if (ui.fs[i].y == '?' && !ui.fs[i].unmerged &&
		    smart_match(ui.fs[i].path, strlen(ui.fs[i].path), pattern))
			return true;
	return false;
}

/* Whether item k is the Untracked section's row (it folds). */
bool item_is_fold(size_t k)
{
	return k < ui.nitems && ui.items[k].header && ui.items[k].untracked &&
	       !ui.items[k].dirlen;
}

/*
 * Whether item k is a section's row (Staged, Unstaged, Untracked): Enter
 * or Space there takes all its files. Not the Commit section's.
 */
bool item_is_section(size_t k)
{
	return k < ui.nitems && ui.items[k].header && !ui.items[k].dirlen &&
	       ui.items[k].sec != SEC_COMMIT;
}

/* Whether the Files cursor can stop on item k, a file or a section row. */
bool item_stop(size_t k)
{
	return k < ui.nitems && (!ui.items[k].header || item_is_section(k));
}

/* The row of the section sec, or of the Untracked one with untracked, or -1. */
static size_t section_row(enum sec sec, bool untracked)
{
	size_t k;

	for (k = 0; k < ui.nitems; k++)
		if (item_is_section(k) && ui.items[k].sec == sec &&
		    ui.items[k].untracked == untracked)
			return k;
	return (size_t)-1;
}

/* Whether item k is a directory's row (not a section's, not a file). */
bool item_is_dir(size_t k)
{
	return ui.items[k].header && ui.items[k].dirlen;
}

const char *item_path(size_t k)
{
	if (ui.items[k].sec == SEC_COMMIT)
		return ui.cfiles[ui.items[k].fi].path;
	return ui.fs[ui.items[k].fi].path;
}

void select_item(void)
{
	char *old = ui.path;

	ui.dp[0].lead = ui.dp[1].lead = 0;
	ui.path = NULL;
	ui.path_untracked = false;
	ui.path_commit = false;
	if (ui.fcur < ui.nitems && !ui.items[ui.fcur].header &&
	    ui.items[ui.fcur].sec == SEC_COMMIT) {
		ui.path = xstrdup(item_path(ui.fcur));
		ui.path_staged = false;
		ui.path_commit = true;
	} else if (ui.fcur < ui.nitems && !ui.items[ui.fcur].header) {
		const struct file_status *f = &ui.fs[ui.items[ui.fcur].fi];

		ui.path = xstrdup(f->path);
		ui.path_staged = ui.items[ui.fcur].sec == SEC_STAGED;
		ui.path_untracked = f->y == '?';
	}
	/* a new file starts at the left edge */
	if (!old || !ui.path || strcmp(old, ui.path)) {
		ui.dp[0].hscroll = ui.dp[1].hscroll = 0;
		/*
		 * a file whose loading was stopped is tried again when it
		 * is back
		 */
		free(ui.stopped_path);
		ui.stopped_path = NULL;
	}
	free(old);
}

/*
 * rev as "abc1234 subject", the subject in quotes with quote, or NULL when
 * there is no such commit.
 */
char *commit_line(const char *rev, bool quote)
{
	struct strbuf spec = STRBUF_INIT, l = STRBUF_INIT;
	char *oid, *sub = NULL;

	if (!ui.head_abbrev)
		return repo_commit_oneline(ui.r, rev, quote);
	sb_addf(&spec, "%s^{commit}", rev);
	oid = repo_rev(ui.r, spec.buf);
	if (oid)
		sub = repo_commit_subject(ui.r, oid);
	if (sub)
		sb_addf(&l, quote ? "%.*s \"%s\"" : "%.*s %s", ui.head_abbrev,
			oid, sub);
	free(oid);
	free(sub);
	sb_release(&spec);
	if (!l.len) {
		sb_release(&l);
		return NULL;
	}
	return sb_detach(&l);
}

/*
 * The title, branch and HEAD from one git log, the branch from HEAD's
 * decoration ("HEAD -> main, ..." or "HEAD, ..." when detached). Before
 * the first commit it comes from git symbolic-ref.
 */
void load_head(void)
{
	char *branch = NULL, *head = NULL, *oid = repo_rev(ui.r, "HEAD");

	/*
	 * HEAD where it was keeps the title, and a branch switched outside
	 * needs F5
	 */
	if (!ui.full && *ui.head &&
	    (oid ? ui.head_oid && !strcmp(oid, ui.head_oid) : !ui.head_oid)) {
		free(oid);
		return;
	}
	free(ui.head_oid);
	ui.head_oid = oid;
	if (!ui.full && ui.head_abbrev && oid) {
		/* HEAD moved (a commit): the title without git */
		bool known;
		char *sub;
		char *b;

		b = repo_head_branch(ui.r, &known);
		sub = known ? repo_commit_subject(ui.r, oid) : NULL;

		if (sub) {
			snprintf(ui.head, sizeof(ui.head),
				 "git-carve  %s  %.*s %s", b ? b : "(detached)",
				 ui.head_abbrev, oid, sub);
			free(ui.branch);
			ui.branch = b;
			free(sub);
			return;
		}
		free(b);
	}
	repo_head_info(ui.r, &branch, &head);
	if (head)
		ui.head_abbrev = (int)strcspn(head, " ");
	snprintf(ui.head, sizeof(ui.head), "git-carve  %s  %s",
		 branch ? branch : "(detached)", head ? head : "(no commits)");
	free(ui.branch);
	ui.branch = branch;
	free(head);
}

/*
 * The commit of the Commit section, the one selected in the Series pane.
 * There is none on the work tree row or during a split. It is loaded again
 * when it changes.
 */
static void load_commit_view(void)
{
	const char *sel = ui.split.active ? NULL : sv_commit(ui.sv.sel);
	char *head = sel ? xstrdup(sel) : NULL;
	/* the distance from the tip */
	size_t pos;

	pos = ui.sv.sel - (ui.sv.own ? 1 : 0);

	if (head && ui.ccommit && !strcmp(head, ui.ccommit)) {
		free(head);
		/* the same commit, another place in the list */
		ui.cpos = pos;
		return;
	}
	cfiles_free(ui.cfiles, ui.ncfiles);
	ui.cfiles = NULL;
	ui.ncfiles = 0;
	free(ui.ccommit);
	ui.ccommit = head;
	if (!head) {
		/* on the work tree row the next commit shown is a new stop */
		ui.cpos = (size_t)-1;
		return;
	}
	if (commit_files(ui.r, head, &ui.cfiles, &ui.ncfiles))
		msg_repo_err();
	/* the same place (the commit rewritten) keeps the selection */
	if (pos != ui.cpos)
		ui.commit_changed = true;
	ui.cpos = pos;
}

/*
 * The status line of what is in progress (a split, your rebase, a move,
 * the commit shown), from the states load_split() loaded.
 */
static void build_status(struct status_line *s)
{
	memset(s, 0, sizeof(*s));
	if (ui.split.active) {
		snprintf(s->head, sizeof(s->head), "SPLIT");
		snprintf(s->keys, sizeof(s->keys),
			 "{finish}: finish  {abort}: abort");
		if (ui.split.done)
			snprintf(s->info, sizeof(s->info),
				 "%d commit%s made, "
				 "nothing left",
				 ui.split.commits_made,
				 ui.split.commits_made == 1 ? "" : "s");
		else if (ui.split.removed < 0)
			/* a huge commit counts files, not lines (split_big) */
			snprintf(s->info, sizeof(s->info),
				 "%d commit%s made, "
				 "%d file%s left%s",
				 ui.split.commits_made,
				 ui.split.commits_made == 1 ? "" : "s",
				 ui.split.added, ui.split.added == 1 ? "" : "s",
				 ui.split.older	    ? " (older commit)" :
				 ui.split.in_rebase ? " (in your rebase)" :
						      "");
		else
			snprintf(s->info, sizeof(s->info),
				 "%d commit%s made, "
				 "remaining diff +%d/-%d%s",
				 ui.split.commits_made,
				 ui.split.commits_made == 1 ? "" : "s",
				 ui.split.added, ui.split.removed,
				 ui.split.older	    ? " (older commit)" :
				 ui.split.in_rebase ? " (in your rebase)" :
						      "");
		snprintf(s->id, sizeof(s->id), "%.7s", ui.split.orig);
		snprintf(s->subject, sizeof(s->subject), "%s",
			 ui.split.subject ? ui.split.subject : "");
	} else if (ui.rebase_stop) {
		/* where the rebase stands is HEAD, whatever commit is shown */
		char *line = commit_line("HEAD", false);
		char *subj;

		subj = line ? strchr(line, ' ') : NULL;

		if (ui.rtotal)
			snprintf(s->head, sizeof(s->head), "REBASE %d/%d",
				 ui.rstep, ui.rtotal);
		else
			snprintf(s->head, sizeof(s->head), "REBASE");
		snprintf(s->keys, sizeof(s->keys),
			 "{split}: split  "
			 "{fixup-to}: move lines");
		snprintf(s->info, sizeof(s->info),
			 "yours, stopped here; "
			 "git rebase --continue goes on");
		snprintf(s->id, sizeof(s->id), "%.*s",
			 subj ? (int)(subj - line) : 0, line ? line : "");
		snprintf(s->subject, sizeof(s->subject), "%s",
			 subj ? subj + 1 : "");
		free(line);
	} else if (ui.series.foreign_rebase) {
		snprintf(s->head, sizeof(s->head), "REBASE");
		snprintf(s->info, sizeof(s->info),
			 "in progress (not started "
			 "by git carve): finish it with git rebase --continue "
			 "/ --abort");
	} else if (ui.series.op != OP_NONE) {
		snprintf(s->head, sizeof(s->head), "%s",
			 ui.series.op == OP_FIXUP  ? "FIXUP-TO" :
			 ui.series.op == OP_MOVE   ? "MOVE" :
			 ui.series.op == OP_SQUASH ? "SQUASH" :
						     "DEFER");
		snprintf(s->keys, sizeof(s->keys),
			 "{finish}: continue  {abort}: abort");
		if (ui.series.conflicts)
			snprintf(s->info, sizeof(s->info),
				 "%d conflicted "
				 "file%s: resolve, stage",
				 ui.series.conflicts,
				 ui.series.conflicts == 1 ? "" : "s");
		else if (ui.series.rebasing)
			snprintf(s->info, sizeof(s->info), "stopped");
		snprintf(s->id, sizeof(s->id), "%.7s", ui.series.target);
		snprintf(s->subject, sizeof(s->subject), "%s",
			 ui.series.subject ? ui.series.subject : "");
	}
	/* which commit of the Series pane is shown */
	if (!*s->head && ui.ccommit && sv_row_of(ui.ccommit) != (size_t)-1) {
		size_t k = sv_row_of(ui.ccommit) - (ui.sv.own ? 1 : 0);
		const char *line = ui.sv.lines[k];
		const char *sp;
		char l[32];

		sp = strchr(line, ' ');

		sv_label(k, l, sizeof(l));
		snprintf(s->head, sizeof(s->head), "%s", l);
		/* s splits the commit shown, on a branch checked out here */
		snprintf(
			s->keys, sizeof(s->keys),
			"{next-commit}/{prev-commit}: commits  {split}: split");
		s->optional_keys = true;
		snprintf(s->id, sizeof(s->id), "%.*s",
			 sp ? (int)(sp - line) : 7, line);
		snprintf(s->subject, sizeof(s->subject), "%s",
			 sp ? sp + 1 : "");
	}
}

/*
 * The files the commit being split changes. Its new files are untracked
 * now, yet the work being split, and are listed under Unstaged. They are
 * read once per split with one git, not on every reload.
 */
static void load_split_files(void)
{
	const char *orig = ui.split.active ? ui.split.orig : NULL;

	if (orig && ui.sfiles_of && !strcmp(orig, ui.sfiles_of))
		return;
	cfiles_free(ui.sfiles, ui.nsfiles);
	ui.sfiles = NULL;
	ui.nsfiles = 0;
	free(ui.sfiles_of);
	ui.sfiles_of = NULL;
	if (!orig)
		return;
	ui.sfiles_of = xstrdup(orig);
	if (commit_files(ui.r, orig, &ui.sfiles, &ui.nsfiles))
		msg_repo_err();
}

/* Whether path is a file the commit being split adds. */
static bool split_adds(const char *path)
{
	size_t i;

	for (i = 0; i < ui.nsfiles; i++)
		if ((ui.sfiles[i].status == 'A' ||
		     ui.sfiles[i].status == 'R') &&
		    !strcmp(ui.sfiles[i].path, path))
			return true;
	return false;
}

static void load_split(void)
{
	split_state_release(&ui.split);
	/* the status of a huge split counts files (SPLIT_BIG_FILES) */
	ui.r->split_big = ui.ntracked + ui.nscan > SPLIT_BIG_FILES;
	split_state_load_clean(ui.r, &ui.split, tracked_clean_now());
	series_state_release(&ui.series);
	series_state_load(ui.r, &ui.series);
	/* stopped at a commit (edit, break), not in a conflict */
	ui.rebase_stop = ui.series.foreign_rebase && !ui.series.conflicts &&
			 !ui.split.active && !staged_any();
	ui.rstep = ui.rtotal = 0;
	if (ui.series.foreign_rebase)
		rebase_step(ui.r, &ui.rstep, &ui.rtotal);
	load_commit_view();
	load_split_files();
	/* the Series pane goes when it has no rows */
	if (ui.active == P_SERIES && !series_mode())
		ui.active = P_FILES;
	build_status(&ui.st);
}

/* Whether the last untracked scan found untracked files. */
static bool scan_untracked(void)
{
	size_t i;

	for (i = 0; i < ui.nscan; i++)
		if (ui.scan[i].y == '?')
			return true;
	return false;
}

/*
 * A path the last scan found untracked that got tracked changes after it,
 * which were staged, may be in the index now. Once committed it must not
 * come back as untracked until the next scan, and unstaged again it must.
 * Mark such entries, and the file list asks the index about them. Both
 * lists are sorted by path.
 */
static void mark_scan(void)
{
	size_t i, j = 0;

	for (i = 0; i < ui.nscan; i++) {
		int c = 1;

		if (ui.scan[i].y != '?')
			continue; /* the scan lists tracked changes too */
		while (j < ui.ntracked &&
		       (c = strcmp(ui.tracked[j].path, ui.scan[i].path)) < 0)
			j++;
		if (j < ui.ntracked && !c)
			ui.scan[i].seen_tracked = true;
	}
}

/* ui.fs = tracked entries + untracked entries of the last scan */
static void merge_status(void)
{
	size_t ns = 0, na = 0, i, k, *at;
	struct file_status *scan;
	const char **ask;
	char *stamp;
	bool *in;

	mark_scan();
	/*
	 * The scan without the entries that are in the index by now. Those
	 * the tracked status lists are, and the others are asked once
	 * while the index file stays the same.
	 */
	stamp = repo_index_stamp(ui.r);
	if (!stamp || !ui.scan_ix_stamp || strcmp(stamp, ui.scan_ix_stamp))
		for (i = 0; i < ui.nscan; i++)
			ui.scan[i].ix_known = false;
	free(ui.scan_ix_stamp);
	ui.scan_ix_stamp = stamp;
	scan = xcalloc(ui.nscan + 1, sizeof(*scan));
	ask = xcalloc(ui.nscan + 1, sizeof(*ask));
	at = xcalloc(ui.nscan + 1, sizeof(*at));
	in = xcalloc(ui.nscan + 1, sizeof(*in));
	for (i = 0; i < ui.nscan; i++) {
		struct file_status *f = &ui.scan[i];
		const struct file_status *t;

		if (!f->seen_tracked || f->ix_known)
			continue;
		t = ui.ntracked ? bsearch(f, ui.tracked, ui.ntracked,
					  sizeof(*ui.tracked), status_cmp) :
				  NULL;
		if (t && (t->unmerged || t->x != 'D')) {
			f->ix_known = f->in_index = true;
			continue;
		}
		at[na] = i;
		ask[na++] = f->path;
	}
	/* after a failed git, ask again next time */
	if (paths_in_index(ui.r, ask, na, in))
		na = 0;
	for (k = 0; k < na; k++) {
		ui.scan[at[k]].ix_known = true;
		ui.scan[at[k]].in_index = in[k];
	}
	for (i = 0; i < ui.nscan; i++)
		if (!ui.scan[i].seen_tracked || !ui.scan[i].in_index)
			scan[ns++] = ui.scan[i];
	free(ask);
	free(at);
	free(in);
	free_status(ui.fs, ui.nfs);
	ui.fs = NULL;
	ui.nfs = merge_untracked(ui.tracked, ui.ntracked, scan, ns, &ui.fs);
	free(scan);
	/* renames are one entry each, and a failure only costs the pairing */
	if (!ui.keep_renames) {
		free_renames(ui.rv, ui.nrv);
		ui.rv = NULL;
		ui.nrv = 0;
		if (find_renames(ui.r, ui.fs, ui.nfs, &ui.rv, &ui.nrv)) {
			free_renames(ui.rv, ui.nrv);
			ui.rv = NULL;
			ui.nrv = 0;
		}
	}
	ui.keep_renames = false;
	ui.nfs = apply_renames(ui.fs, ui.nfs, ui.rv, ui.nrv);
	build_items();
}

/*
 * Whether the path of f is untracked in the first n entries of the scan.
 * The scan lists tracked changes too, and a path staged after it is not
 * untracked.
 */
static bool untracked_in_scan(const struct file_status *f, size_t n)
{
	const struct file_status *s =
		n ? bsearch(f, ui.scan, n, sizeof(*ui.scan), status_cmp) : NULL;

	return s && s->y == '?';
}

/*
 * Paths with tracked changes before that have none now, are not in the
 * index and are in the work tree became untracked (a new file unstaged):
 * list them as the scan would, not only once it runs again. Only a file
 * new in the index before (A) can; the index is asked about those in one
 * git ls-files (paths_in_index).
 */
static void newly_untracked(const struct file_status *before, size_t n)
{
	size_t i, nc = 0, alloc = 0, nscan = ui.nscan;
	const char **cand = NULL;
	bool added = false, *in;

	/* both lists are sorted by path */
	for (i = 0; i < n; i++) {
		if (before[i].x != 'A' ||
		    (ui.ntracked && bsearch(&before[i], ui.tracked, ui.ntracked,
					    sizeof(*ui.tracked), status_cmp)) ||
		    untracked_in_scan(&before[i], nscan) ||
		    !path_in_worktree(before[i].path))
			continue;
		ALLOC_GROW(cand, nc + 1, alloc);
		cand[nc++] = before[i].path;
	}
	in = xcalloc(nc + 1, sizeof(*in));
	/* after a failed git nothing is added */
	if (paths_in_index(ui.r, cand, nc, in))
		nc = 0;
	alloc = ui.nscan;
	for (i = 0; i < nc; i++) {
		if (in[i])
			continue;
		ALLOC_GROW(ui.scan, ui.nscan + 1, alloc);
		ui.scan[ui.nscan++] = file_status_untracked(xstrdup(cand[i]));
		added = true;
	}
	if (added)
		qsort(ui.scan, ui.nscan, sizeof(*ui.scan), status_cmp);
	free(in);
	free(cand);
}

/* What reload_from reads again. */
enum reload_how {
	RL_VIEW, /* not the work tree's status (a Series commit shown) */
	RL_ALL,	 /* everything */
	RL_PATH, /* only the status of one path, whose index entry changed */
};

/* A status letter that can make the path one half of a rename. */
static bool rename_letters(const struct file_status *f)
{
	return f->x == 'A' || f->x == 'D' || f->y == 'A' || f->y == 'D' ||
	       f->y == '?';
}

/* ui.tracked holds for the index file as it is now (or cannot tell). */
bool status_still(void)
{
	char *now;
	bool ok;

	if (!ui.status_stamp)
		return true;
	now = repo_index_stamp(ui.r);
	ok = now && !strcmp(now, ui.status_stamp);
	free(now);
	return ok;
}

void status_restamp(void)
{
	free(ui.status_stamp);
	ui.status_stamp = repo_index_stamp(ui.r);
}

/*
 * With nothing staged and no conflict the index's tree is HEAD's, and
 * stays HEAD's while the index file does. Undo's after-state then needs no
 * write-tree.
 */
static void index_tree_known(void)
{
	size_t i;

	free(ui.r->ix_tree);
	free(ui.r->ix_tree_stamp);
	ui.r->ix_tree = ui.r->ix_tree_stamp = NULL;
	for (i = 0; i < ui.ntracked; i++)
		if (ui.tracked[i].unmerged ||
		    (ui.tracked[i].x != ' ' && ui.tracked[i].x != '?'))
			return;
	if (!ui.status_stamp)
		return;
	ui.r->ix_tree = repo_rev(ui.r, "HEAD^{tree}");
	if (ui.r->ix_tree)
		ui.r->ix_tree_stamp = xstrdup(ui.status_stamp);
}

/* No tracked changes at all, from the status read, or -1 to ask git. */
int tracked_clean_now(void)
{
	size_t i;

	if (!ui.status_stamp || !status_still())
		return -1;
	for (i = 0; i < ui.ntracked; i++)
		if (ui.tracked[i].unmerged || ui.tracked[i].x != ' ' ||
		    (ui.tracked[i].y != ' ' && ui.tracked[i].y != '?'))
			return 0;
	return 1;
}

/* Anything staged, from the status read while it holds, else from git. */
bool staged_any(void)
{
	size_t i;

	if (!ui.status_stamp || !status_still())
		return have_staged(ui.r);
	for (i = 0; i < ui.ntracked; i++)
		if (ui.tracked[i].unmerged ||
		    (ui.tracked[i].x != ' ' && ui.tracked[i].x != '?'))
			return true;
	return false;
}

int flush_undo(void)
{
	bool st = status_still(), dc = dcache_holds();
	int rc = undo_flush(ui.r);

	if (st)
		status_restamp();
	if (dc)
		dcache_restamp();
	return rc;
}

/*
 * For RL_PATH, load path's diffs and replace its entry in ui.tracked by
 * its status now. That is what the two diffs say, the staged one for the
 * index and the unstaged one for the work tree, else git status. The
 * renames found before stay valid unless path is or was a candidate.
 */
static int reload_path_status(const char *path)
{
	struct file_status *fresh = NULL, *old = NULL;
	size_t nfresh = 0, nold = 0, i;
	bool cand = false;
	char x, y;

	/* the entry's letters are from before the command, no hints */
	ui.status_stale = true;
	dp_load(&ui.dp[0], path, false);
	dp_load(&ui.dp[1], path, true);
	ui.status_stale = false;
	x = ui.dp[1].letter;
	y = ui.dp[0].letter;
	/*
	 * Staged as deleted, the unstaged diff may be of an untracked file
	 * of that name, which git status lists apart.
	 */
	if (x && y && x != 'D') {
		if (x != ' ' || y != ' ') {
			fresh = xcalloc(1, sizeof(*fresh));
			fresh->path = xstrdup(path);
			fresh->x = x;
			fresh->y = y;
			nfresh = 1;
		}
	} else if (load_status_paths(ui.r, &path, 1, &fresh, &nfresh)) {
		return -1;
	}
	for (i = 0; i < nfresh; i++)
		cand |= rename_letters(&fresh[i]);
	ui.ntracked = status_splice(&ui.tracked, ui.ntracked, &path, 1, fresh,
				    nfresh, &old, &nold);
	for (i = 0; i < nold; i++)
		cand |= rename_letters(&old[i]);
	/* a new file unstaged (A before, nothing now) is untracked again */
	newly_untracked(old, nold);
	free_status(old, nold);
	ui.keep_renames = !cand;
	return 0;
}

/*
 * Item off rows below the section's row at row, kept within the section
 * (its last file when it got shorter).
 */
static size_t stay_place(size_t row, size_t off)
{
	size_t k, last = row;

	for (k = row + 1; k < ui.nitems && !item_is_section(k) &&
			  !(ui.items[k].header && !ui.items[k].dirlen);
	     k++) {
		if (!ui.items[k].header)
			last = k;
		if (k - row >= off && !ui.items[k].header)
			return k;
	}
	return last;
}

/*
 * Put the Files cursor on a file, past the section and directory rows it
 * is on, or back to the last file when there is none below. With no file
 * at all it goes on a section row, the folded Untracked one.
 */
static void fcur_on_file(void)
{
	size_t k;

	if (ui.fcur >= ui.nitems)
		ui.fcur = ui.nitems ? ui.nitems - 1 : 0;
	for (k = ui.fcur; k < ui.nitems; k++)
		if (!ui.items[k].header) {
			ui.fcur = k;
			return;
		}
	for (k = ui.fcur + 1; k-- > 0;)
		if (k < ui.nitems && !ui.items[k].header) {
			ui.fcur = k;
			return;
		}
	while (ui.fcur + 1 < ui.nitems && !item_stop(ui.fcur))
		ui.fcur++;
	while (ui.fcur > 0 && !item_stop(ui.fcur))
		ui.fcur--;
}

/*
 * Filter the Files list, all files for NULL or "". The list is built again
 * without git, and the cursor stays on its file if it passes, else goes to
 * the first one.
 */
void set_filter(const char *pattern)
{
	char *keep = ui.path ? xstrdup(ui.path) : NULL;
	enum sec keep_sec = path_sec();
	size_t k, found = (size_t)-1;

	free(ui.filter);
	ui.filter = pattern && *pattern ? xstrdup(pattern) : NULL;
	build_items();
	for (k = 0; keep && k < ui.nitems; k++) {
		if (ui.items[k].header || strcmp(item_path(k), keep))
			continue;
		if (found == (size_t)-1 || ui.items[k].sec == keep_sec)
			found = k;
	}
	/* else the first file (not the Untracked row) */
	for (k = 0; found == (size_t)-1 && k < ui.nitems; k++)
		if (!ui.items[k].header)
			found = k;
	ui.fcur = found != (size_t)-1 ? found : 0;
	fcur_on_file();
	/* the view stays where it was where it can (the cursor in it) */
	if (ui.ftop >= ui.nitems || ui.fcur < ui.ftop)
		ui.ftop = 0;
	select_item();
	if (!keep || !ui.path || strcmp(keep, ui.path)) {
		ui.dp[0].cur = ui.dp[0].top = ui.dp[1].cur = ui.dp[1].top = 0;
		ui.dp[0].ranging = ui.dp[1].ranging = false;
		diffs_stale();
	}
	free(keep);
}

/*
 * After a commit of everything staged on a huge change set. The work
 * tree's side of every file is as it was, and the staged side is empty
 * once HEAD's tree is the index's. git diff-index --cached --quiet tells
 * that without refreshing 15,000 stat-dirty entries, in 0.06 s instead of
 * a 1.5 s status. The new list is the old one without the staged side.
 * False, and nothing changed, when the commit left something staged, as a
 * hook can.
 */
static bool after_commit_status(const struct file_status *was, size_t n)
{
	struct file_status *v;
	size_t i, k = 0;

	ui.after_commit = false;
	if (n <= AFTER_COMMIT_FILES || have_staged(ui.r))
		return false;
	v = xcalloc(n + 1, sizeof(*v));
	for (i = 0; i < n; i++) {
		const struct file_status *f = &was[i];

		/* committed entirely, or a conflict (none can be committed) */
		if (!f->unmerged && (f->y == ' ' || f->y == '?'))
			continue;
		memset(&v[k], 0, sizeof(v[k]));
		v[k].path = xstrdup(f->path);
		v[k].x = ' ';
		v[k].y = f->y;
		v[k].unmerged = f->unmerged;
		v[k].from_y = f->from_y ? xstrdup(f->from_y) : NULL;
		k++;
	}
	ui.tracked = v;
	ui.ntracked = k;
	return true;
}

/*
 * After a split of HEAD in a big change set the status is the commit's
 * files, without the git status that hashes them all (split_start_status).
 */
static bool after_split_status(void)
{
	struct file_status *v;
	size_t n;

	ui.after_split = false;
	if (split_start_status(ui.r, &v, &n))
		return false;
	if (n <= AFTER_COMMIT_FILES) {
		free_status(v, n);
		return false;
	}
	ui.tracked = v;
	ui.ntracked = n;
	return true;
}

/*
 * The status that follows from what was just done, without git, when it
 * can. Each shortcut is good for the next reload only.
 */
static bool status_known(const struct file_status *before, size_t n)
{
	bool known = (ui.after_commit && after_commit_status(before, n)) ||
		     (ui.after_split && after_split_status());

	ui.after_commit = ui.after_split = false;
	return known;
}

/*
 * What the message pane shows. The message of the commit shown, read
 * through the running cat-file once per commit, else the one being
 * written.
 */
const struct strbuf *pane_message(void)
{
	if (!ui.ccommit)
		return &ui.draft;
	if (!ui.cmsg_of || strcmp(ui.cmsg_of, ui.ccommit)) {
		sb_reset(&ui.cmsg);
		if (repo_commit_message(ui.r, ui.ccommit, &ui.cmsg))
			sb_reset(&ui.cmsg);
		free(ui.cmsg_of);
		ui.cmsg_of = xstrdup(ui.ccommit);
		ui.draft_top = 0;
	}
	return &ui.cmsg;
}

/* The commit message being written, from its file (no git). */
void load_draft(void)
{
	sb_reset(&ui.draft);
	if (draft_read(ui.r, &ui.draft))
		sb_reset(&ui.draft);
	ui.draft_top = 0;
}

/*
 * Reload (how says what), keeping the selected path (in the same section
 * if possible) and the diff cursors.
 */
static void reload_from(enum reload_how how)
{
	char *keep = ui.path ? xstrdup(ui.path) : NULL;
	bool status = how == RL_ALL, same;
	enum sec keep_sec = path_sec();
	/*
	 * after a whole file was staged or unstaged the cursor stays in
	 * its section, at the same place in it (the next file)
	 */
	bool stay = ui.stay_in_section;
	struct file_status *before;
	size_t found = (size_t)-1;
	size_t other = (size_t)-1;
	size_t old = ui.fcur;
	bool sec_untracked;
	enum sec sec_was;
	size_t nbefore;
	/* on a section's row, back there while it exists */
	bool on_sec;
	size_t k;
	on_sec = item_is_section(ui.fcur);
	sec_was = on_sec ? ui.items[ui.fcur].sec : SEC_STAGED;
	sec_untracked = on_sec && ui.items[ui.fcur].untracked;

	enum sec cur_sec = ui.fcur < ui.nitems ? ui.items[ui.fcur].sec :
						 SEC_STAGED;
	bool cur_untr = ui.fcur < ui.nitems && ui.items[ui.fcur].untracked;
	size_t sec_at = section_row(cur_sec, cur_untr);
	size_t sec_off =
		sec_at != (size_t)-1 && ui.fcur > sec_at ? ui.fcur - sec_at : 0;

	/* the diffs may have changed, search them again */
	ui.hits_valid = false;
	load_draft();
	if (how == RL_PATH && reload_path_status(keep)) {
		msg_repo_err();
		status = true;
	}
	if (status) {
		/*
		 * after F5 the configuration is read as it is now, when it
		 * is asked for
		 */
		if (ui.full) {
			repo_config_drop(ui.r);
			read_view_config();
		}
		before = ui.tracked;
		nbefore = ui.ntracked;
		ui.tracked = NULL;
		ui.ntracked = 0;
		/* tracked changes now, untracked ones from the last scan */
		/* after the status, which may write the refreshed index */
		if (!status_known(before, nbefore) &&
		    load_status_mode(ui.r, false, &ui.tracked, &ui.ntracked)) {
			msg_repo_err();
			free(ui.status_stamp);
			ui.status_stamp = NULL;
		} else {
			status_restamp();
		}
		index_tree_known();
		newly_untracked(before, nbefore);
		free_status(before, nbefore);
		/*
		 * HEAD may have moved, and the title and the Series pane
		 * with it
		 */
		load_head();
		sv_refresh(ui.full);
		sv_follow(ui.ntracked > 0 || scan_untracked());
	}
	/* first, since the Commit section needs to know where HEAD is */
	if (how != RL_PATH || status)
		load_split();
	merge_status();

	for (k = 0; keep && k < ui.nitems; k++) {
		if (ui.items[k].header || strcmp(item_path(k), keep))
			continue;
		if (ui.items[k].sec == keep_sec)
			found = k;
		else if (other == (size_t)-1)
			other = k;
	}
	ui.stay_in_section = false;
	ui.freveal = true;
	if (on_sec && section_row(sec_was, sec_untracked) != (size_t)-1)
		ui.fcur = section_row(sec_was, sec_untracked);
	else if (found != (size_t)-1)
		ui.fcur = found;
	else if (stay && sec_off &&
		 section_row(cur_sec, cur_untr) != (size_t)-1)
		ui.fcur = stay_place(section_row(cur_sec, cur_untr), sec_off);
	else if (other != (size_t)-1 && !stay)
		ui.fcur = other;
	else
		ui.fcur = old;
	if (!on_sec || !item_is_section(ui.fcur))
		fcur_on_file();
	if (ui.commit_changed) {
		/* another commit shown (a stop, J / K): its first file */
		ui.commit_changed = false;
		if (ui.ncfiles) {
			ui.fcur = 0;
			fcur_on_file();
			ui.ftop = 0;
			/*
			 * When zoomed, stay in the diff to read the next
			 * commit's. Otherwise go to the Files pane, unless
			 * the Series pane is where the cursor moves.
			 */
			if (ui.lay.zoom)
				ui.active = P_UNSTAGED;
			else if (ui.active != P_SERIES)
				ui.active = P_FILES;
			free(keep);
			keep = NULL;
		}
	}
	select_item();
	same = keep && ui.path && !strcmp(keep, ui.path);
	if (!same) {
		ui.dp[0].cur = ui.dp[0].top = 0;
		ui.dp[1].cur = ui.dp[1].top = 0;
	}
	ui.dp[0].ranging = ui.dp[1].ranging = false;
	/*
	 * The diff cache forgets that file, or starts anew after anything
	 * else, before the diffs are shown from it (F5 after an edit it
	 * missed).
	 */
	if (how == RL_PATH && !status) {
		dcache_forget(keep);
		ui.dc_vouched = true;
		status_restamp();
	} else if (status) {
		dcache_drop();
	}
	/* RL_PATH loaded the diffs of the path already */
	if (!(how == RL_PATH && !status && same)) {
		dp_load(&ui.dp[0], ui.path, false);
		dp_load(&ui.dp[1], ui.path, true);
	}
	/* a commit leaves the untracked files as they were */
	if (status && !ui.skip_scan)
		start_scan();
	ui.full = ui.skip_scan = false;
	free(keep);
}

void reload(void)
{
	reload_from(RL_ALL);
}

void reload_full(void)
{
	ui.full = true;
	reload_from(RL_ALL);
}

/*
 * Only the index entry of path changed. HEAD, refs, the operation in
 * progress and the untracked files are as they were, and its status and
 * diffs are all there is to read again. That does not hold while a rebase
 * or a conflict is in progress, whose state follows the index, or when
 * path is a rename with two paths.
 */
void reload_path(const char *path)
{
	const struct file_status *f = cur_fs();

	if (!path || !ui.path || strcmp(path, ui.path) || ui.path_commit ||
	    ui.series.op != OP_NONE || ui.series.foreign_rebase ||
	    ui.series.conflicts ||
	    (f && (f->unmerged || f->from_x || f->from_y))) {
		reload();
		return;
	}
	reload_from(RL_PATH);
}

/* The Series pane's cursor settled on another row. Show it. */
void sv_show(void)
{
	ui.sv.stale = false;
	ui.diff_stale = false;
	reload_from(RL_VIEW);
}

/*
 * The untracked scan finished. Merge its result, keeping the selection.
 * Diffs are loaded again only if the selected file changed.
 */
void scan_done(void)
{
	char *keep = ui.path ? xstrdup(ui.path) : NULL;
	enum sec keep_sec = path_sec();
	size_t k;

	if (ui.bg.status != 0) {
		/* the scan failed or was cut short, keep the old list */
		bg_stop(&ui.bg);
		free(keep);
		return;
	}
	free_status(ui.scan, ui.nscan);
	if (ui.scan_lsfiles)
		ui.nscan = parse_untracked(sb_str(&ui.bg.out), ui.bg.out.len,
					   &ui.scan);
	else
		ui.nscan = parse_status(sb_str(&ui.bg.out), ui.bg.out.len,
					&ui.scan);
	bg_stop(&ui.bg);
	{
		/*
		 * with untracked files, show the work tree if the pane
		 * follows it
		 */
		size_t sel = ui.sv.sel;

		sv_follow(ui.ntracked > 0 || scan_untracked());
		if (ui.sv.sel != sel)
			ui.sv.stale = true;
	}
	merge_status();
	for (k = 0; k < ui.nitems; k++) {
		if (!ui.items[k].header && keep &&
		    !strcmp(item_path(k), keep) &&
		    ui.items[k].sec == keep_sec) {
			ui.fcur = k;
			break;
		}
	}
	fcur_on_file();
	select_item();
	if (!keep || !ui.path || strcmp(keep, ui.path)) {
		ui.dp[0].cur = ui.dp[0].top = 0;
		ui.dp[1].cur = ui.dp[1].top = 0;
		dp_load(&ui.dp[0], ui.path, false);
		dp_load(&ui.dp[1], ui.path, true);
	}
	free(keep);
}

/*
 * Whether git's status has a cache for untracked files to use here
 * (core.untrackedCache, or core.fsmonitor, which keeps it fresh). Without
 * one, status walks the same directories as ls-files and also refreshes
 * the index, which after a reset of a kernel tree is 1.5 s more.
 */
static bool untracked_cached(void)
{
	const char *uc = repo_config(ui.r, "core.untrackedCache");
	const char *fm = repo_config(ui.r, "core.fsmonitor");

	return (uc && strcasecmp(uc, "false") && strcasecmp(uc, "no") &&
		strcmp(uc, "0") && strcasecmp(uc, "off")) ||
	       (fm && *fm && strcasecmp(fm, "false") && strcasecmp(fm, "no") &&
		strcmp(fm, "0") && strcasecmp(fm, "off"));
}

/* Start (or restart) the background scan for untracked files. */
void start_scan(void)
{
	struct argv a = ARGV_INIT;

	if (!ui.untracked_async)
		return;
	ui.scan_lsfiles = !untracked_cached();
	if (ui.scan_lsfiles)
		/* the untracked files only, as status lists them */
		argv_pushl(&a, "--no-optional-locks", GIT_PLAIN, "ls-files",
			   "-z", "-o", "--exclude-standard", NULL);
	else
		status_args(&a, true);
	if (bg_start(&ui.bg, &a))
		set_msg(true, "cannot start the untracked-file scan");
	argv_clear(&a);
}

/* ---- movement --------------------------------------------------------- */

/* Show a placeholder until the diffs of the selected file are loaded. */
void diffs_stale(void)
{
	int i;

	for (i = 0; i < 2; i++) {
		dp_clear(&ui.dp[i]);
		dp_push(&ui.dp[i], R_NOTE, 0, 0, "...");
	}
	ui.diff_stale = true;
}

void diffs_load(void)
{
	if (!ui.diff_stale)
		return;
	ui.diff_stale = false;
	dp_load(&ui.dp[0], ui.path, false);
	dp_load(&ui.dp[1], ui.path, true);
}
