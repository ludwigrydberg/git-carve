// SPDX-License-Identifier: MIT
/*
 * The diff cache. Once the keys pause, one git diff and one git diff
 * --cached of the whole tree run in the background, and the diff panes
 * take a file's diffs from them instead of running two git per file shown.
 *
 * Both sides hold for the index file they were made with, by its stamp. A
 * command of ours on one file forgets that file and vouches for the rest
 * (dcache_forget), and any other change of the index forgets everything.
 * The unstaged side of a file also holds only while the file is as it was
 * when the diff ran, by size, mtime, ctime and inode as in git's own stat
 * data. ctime catches cp -p, touch -r and tar x. As git does with a racy
 * entry, a file whose mtime is not clearly older than the diff is not
 * trusted, since it may have changed within the timestamp's granularity
 * after the diff read it. Only the default view, with -U, -W and -w
 * unchanged, is cached, and nothing above the cap.
 */
#include "dcache.h"
#include "ui_int.h"

#include <sys/stat.h>
#include <time.h>

/*
 * With more diff output than this there is no cache, and diffs load per
 * file.
 */
#define DCACHE_CAP (32u << 20)
/*
 * With more changed files than this there is no cache either. A kernel
 * release's worth (15,000 files) took 8 s and 650 MB of git only to go
 * over the cap, and per-file diffs are fast anyway.
 */
#define DCACHE_MAX_FILES 3000

/* Seconds a file's mtime must be older than the diff (FAT has 2). */
#define RACY_SECS 2

/*
 * For the unstaged side, the file when the diff ran. ino 0 means not
 * trusted.
 */
struct fstamp {
	long long size, mtime, mtime_ns, ctime, ctime_ns, ino;
};

struct dfile {
	struct fdiff fd;
	struct fstamp st;
};

struct dside {
	struct dfile *f; /* sorted by path */
	size_t n;
	bool valid;
};

static struct {
	struct dside side[2]; /* 0 unstaged, 1 staged */
	struct bg_proc bg;
	int running;	  /* the side being made, -1 none */
	char *stamp;	  /* the index file both sides hold for */
	char *run_stamp;  /* ... and the one under the run in progress */
	time_t run_start; /* when the run in progress started */
	time_t racy_at;	  /* when the files too new to trust no longer are */
	bool too_big;	  /* over the cap, not again ... */
	size_t too_big_n; /* ... until the changed files are half this */
} dc = { .running = -1, .bg = { .fd = -1 } };

static const char *dfile_path(const struct dfile *f)
{
	return f->fd.new_path ? f->fd.new_path : f->fd.old_path;
}

static int dfile_cmp(const void *a, const void *b)
{
	const char *pa = dfile_path(a), *pb = dfile_path(b);

	return strcmp(pa ? pa : "", pb ? pb : "");
}

static void side_free(struct dside *s)
{
	size_t i;

	for (i = 0; i < s->n; i++)
		fdiff_free(&s->f[i].fd);
	free(s->f);
	memset(s, 0, sizeof(*s));
}

void dcache_drop(void)
{
	side_free(&dc.side[0]);
	side_free(&dc.side[1]);
	bg_stop(&dc.bg);
	dc.running = -1;
	free(dc.stamp);
	free(dc.run_stamp);
	dc.stamp = dc.run_stamp = NULL;
	dc.racy_at = 0;
	/*
	 * over the cap once means over it again, unless the changes shrank
	 * a lot
	 */
	if (dc.too_big && ui.ntracked * 2 < dc.too_big_n)
		dc.too_big = false;
}

/* Only the view the cache is made with, with -U, -W and -w unchanged. */
static bool default_view(void)
{
	const struct diff_opts def = DIFF_OPTS_INIT;

	return ui.dopts.context == def.context && !ui.dopts.function_context &&
	       !ui.dopts.ignore_ws;
}

static struct dfile *find(struct dside *s, const char *path)
{
	size_t lo = 0, hi = s->n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = strcmp(dfile_path(&s->f[mid]) ? dfile_path(&s->f[mid]) :
							"",
			       path);

		if (!c)
			return &s->f[mid];
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

/*
 * The stamp of path now, for a diff started at since. ino is 0 when the
 * file is gone or too new to trust.
 */
static struct fstamp stamp_of(const char *path, time_t since)
{
	struct fstamp f = { 0 };
	struct stat st;

	if (lstat(path, &st) ||
	    (long long)st.st_mtime > (long long)since - RACY_SECS)
		return f;
	f.size = (long long)st.st_size;
	f.mtime = (long long)st.st_mtime;
	f.mtime_ns = (long long)ST_MTIME_NSEC(st);
	f.ctime = (long long)st.st_ctime;
	f.ctime_ns = (long long)ST_CTIME_NSEC(st);
	/* an inode of 0 is not a real one, trusted all the same */
	f.ino = st.st_ino ? (long long)st.st_ino : 1;
	return f;
}

static bool same_file(const char *path, const struct fstamp *was)
{
	struct stat st;

	return was->ino && !lstat(path, &st) &&
	       (long long)st.st_size == was->size &&
	       (long long)st.st_mtime == was->mtime &&
	       (long long)ST_MTIME_NSEC(st) == was->mtime_ns &&
	       (long long)st.st_ctime == was->ctime &&
	       (long long)ST_CTIME_NSEC(st) == was->ctime_ns &&
	       (st.st_ino ? (long long)st.st_ino : 1) == was->ino;
}

bool dcache_holds(void)
{
	char *now;
	bool ok;

	if (!dc.stamp)
		return false;
	now = repo_index_stamp(ui.r);
	ok = now && !strcmp(now, dc.stamp);
	free(now);
	return ok;
}

/* The index file is not the one the cache holds for. Forget it all. */
static bool stamp_holds(void)
{
	char *now = repo_index_stamp(ui.r);
	bool ok;

	ok = now && dc.stamp && !strcmp(now, dc.stamp);

	free(now);
	if (!ok && (dc.side[0].valid || dc.side[1].valid))
		dcache_drop();
	return ok;
}

/*
 * Every file's diff of a side, when the side holds for all of them. That
 * needs the index unchanged and, for the unstaged side, every file with
 * unstaged changes in it and as it was then, since a file changed after
 * would be missing or stale. fn gets each. False when the side does not
 * hold, and then nothing is called.
 */
bool dcache_each(bool cached, void (*fn)(const struct fdiff *, void *),
		 void *ctx)
{
	struct dside *s = &dc.side[cached];
	size_t i;

	if (!s->valid || !default_view() || !stamp_holds())
		return false;
	for (i = 0; !cached && i < ui.nfs; i++) {
		const struct file_status *f = &ui.fs[i];
		struct dfile *d;

		if (!fs_unstaged(f) || f->y == '?' || f->unmerged)
			continue;
		d = find(s, f->path);
		if (!d || !same_file(f->path, &d->st))
			return false;
	}
	for (i = 0; i < s->n; i++)
		fn(&s->f[i].fd, ctx);
	return true;
}

bool dcache_get(const char *path, bool cached, struct fdiff **out)
{
	struct dside *s = &dc.side[cached];
	struct dfile *f;

	if (!s->valid || !default_view() || !stamp_holds())
		return false;
	f = find(s, path);
	if (!f)
		return false; /* not in it, the per-file diff tells */
	if (!cached && !same_file(path, &f->st))
		return false; /* changed after, or too new, not this */
	*out = xmalloc(sizeof(**out));
	fdiff_dup(&f->fd, *out);
	return true;
}

void dcache_put(const char *path, bool cached, const struct fdiff *fd,
		time_t since)
{
	struct dside *s = &dc.side[cached];
	struct fstamp st = { 0 };
	struct dfile *f;
	size_t i;

	if (!fd || !s->valid || !default_view() || !stamp_holds())
		return;
	if (!cached) {
		st = stamp_of(path, since);
		if (!st.ino)
			return;
	}
	f = find(s, path);
	if (f) {
		/* there, but too new to trust when the run read it */
		if (!cached && !f->st.ino) {
			fdiff_free(&f->fd);
			fdiff_dup(fd, &f->fd);
			f->st = st;
		}
		return;
	}
	/* in order, after the last file sorting before it */
	for (i = 0;
	     i < s->n &&
	     strcmp(dfile_path(&s->f[i]) ? dfile_path(&s->f[i]) : "", path) < 0;
	     i++)
		;
	s->f = xrealloc(s->f, (s->n + 1) * sizeof(*s->f));
	memmove(&s->f[i + 1], &s->f[i], (s->n - i) * sizeof(*s->f));
	s->n++;
	f = &s->f[i];
	memset(f, 0, sizeof(*f));
	fdiff_dup(fd, &f->fd);
	f->st = st;
}

void dcache_forget(const char *path)
{
	size_t k;

	for (k = 0; k < 2; k++) {
		struct dfile *f = find(&dc.side[k], path);

		if (f) {
			struct dside *s = &dc.side[k];
			size_t i = (size_t)(f - s->f);

			fdiff_free(&f->fd);
			memmove(f, f + 1, (s->n - i - 1) * sizeof(*f));
			s->n--;
		}
	}
	/* a run in progress may have read the file before the command */
	if (dc.running >= 0) {
		bg_stop(&dc.bg);
		dc.running = -1;
	}
	/* the rest holds for the index as the command left it */
	free(dc.stamp);
	dc.stamp = repo_index_stamp(ui.r);
}

void dcache_restamp(void)
{
	if (!dc.stamp)
		return;
	free(dc.stamp);
	dc.stamp = repo_index_stamp(ui.r);
}

/*
 * The index file is one a status that saw nothing staged was read with,
 * and its tree is HEAD's. The staged side is empty, without git diff
 * --cached.
 */
static bool index_is_head(const char *stamp)
{
	char *head;
	bool same;

	if (!ui.r->ix_tree || !ui.r->ix_tree_stamp ||
	    strcmp(ui.r->ix_tree_stamp, stamp))
		return false;
	head = repo_rev(ui.r, "HEAD^{tree}");
	same = head && !strcmp(head, ui.r->ix_tree);
	free(head);
	return same;
}

/*
 * The all files view shows the files after the current one only from the
 * cache, so there the unstaged side runs again once the files that were
 * too new to trust are old enough. Elsewhere such a file loads when it is
 * shown.
 */
static bool racy_wait(void)
{
	return ui.all_files && dc.racy_at && dc.side[0].valid;
}

/* Start the git diff of a side that is not made yet. */
static void start(void)
{
	struct argv a = ARGV_INIT;
	int k = -1;

	if (dc.side[0].valid && !dc.side[1].valid)
		k = 1;
	else if (!dc.side[0].valid || (racy_wait() && time(NULL) >= dc.racy_at))
		k = 0;
	if (k < 0 || !dcache_wanted())
		return;
	free(dc.run_stamp);
	dc.run_stamp = repo_index_stamp(ui.r);
	if (!dc.run_stamp)
		return;
	if (k == 1 && index_is_head(dc.run_stamp) &&
	    (!dc.stamp || !strcmp(dc.stamp, dc.run_stamp))) {
		side_free(&dc.side[1]);
		dc.side[1].valid = true;
		if (!dc.stamp)
			dc.stamp = xstrdup(dc.run_stamp);
		return;
	}
	dc.run_start = time(NULL);
	/* no optional lock, it must not write the index under the cache */
	argv_pushl(&a, "--no-optional-locks", "--literal-pathspecs", NULL);
	push_diff_args(&a);
	push_diff_opts(&a, &ui.dopts);
	if (k)
		argv_push(&a, "--cached");
	if (!bg_start(&dc.bg, &a))
		dc.running = k;
	argv_clear(&a);
}

/*
 * The run of side k is done. Keep its files and the stat of the unstaged
 * ones.
 */
static void run_done(int k)
{
	char *now = repo_index_stamp(ui.r);
	struct dside *s = &dc.side[k];
	struct fdiff *fds = NULL;
	size_t n = 0, i;

	/* the index changed during the run, or git failed, try again later */
	if (dc.bg.status || !now || !dc.run_stamp ||
	    strcmp(now, dc.run_stamp) ||
	    (dc.stamp && strcmp(dc.stamp, dc.run_stamp)) ||
	    diff_parse(sb_str(&dc.bg.out), dc.bg.out.len, &fds, &n, NULL)) {
		free(now);
		bg_stop(&dc.bg);
		if (dc.stamp && now && strcmp(dc.stamp, dc.run_stamp))
			dcache_drop();
		return;
	}
	free(now);
	side_free(s);
	s->f = xcalloc(n + 1, sizeof(*s->f));
	for (i = 0; i < n; i++) {
		fdiff_dup(&fds[i], &s->f[i].fd);
		if (!k && dfile_path(&s->f[i]))
			s->f[i].st =
				stamp_of(dfile_path(&s->f[i]), dc.run_start);
	}
	fdiff_free_array(fds, n);
	s->n = n;
	if (!k) {
		dc.racy_at = 0;
		for (i = 0; i < n; i++)
			if (dfile_path(&s->f[i]) && !s->f[i].st.ino)
				dc.racy_at = dc.run_start + RACY_SECS + 1;
	}
	qsort(s->f, s->n, sizeof(*s->f), dfile_cmp);
	s->valid = true;
	if (!dc.stamp)
		dc.stamp = xstrdup(dc.run_stamp);
	bg_stop(&dc.bg);
}

bool dcache_busy(void)
{
	return dc.running >= 0;
}

bool dcache_wanted(void)
{
	return !dc.too_big && default_view() &&
	       ui.ntracked <= DCACHE_MAX_FILES &&
	       (!dc.side[0].valid || !dc.side[1].valid || racy_wait());
}

void dcache_idle(void)
{
	int k = dc.running;

	if (k < 0) {
		start();
		return;
	}
	if (bg_pump(&dc.bg) == 0) {
		if (dc.bg.out.len > DCACHE_CAP) {
			bg_stop(&dc.bg);
			dc.running = -1;
			dc.too_big = true;
			dc.too_big_n = ui.ntracked;
		}
		return;
	}
	dc.running = -1;
	run_done(k);
	start();
}
