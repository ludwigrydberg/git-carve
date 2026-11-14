// SPDX-License-Identifier: MIT
/* git carve gc, removing only what is provably dead (gc.h). */
#include "gc.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "series.h"
#include "split.h"
#include "undo.h"
#include "opstate.h"
#include "rebase.h"
#include "cdiff.h"

static bool have_ref(struct repo *r, const char *ref)
{
	return !repo_rev_status(r, ref);
}

/* The process that made a "<prefix><pid><sep>..." name is gone. */
static bool owner_gone(const char *name, const char *prefix, char sep)
{
	const char *p;
	char *end;
	long pid;

	if (strncmp(name, prefix, strlen(prefix)))
		return false;
	p = name + strlen(prefix);
	pid = strtol(p, &end, 10);
	if (end == p || *end != sep || pid <= 0)
		return false;
	return kill((pid_t)pid, 0) && errno == ESRCH;
}

static void say(struct strbuf *out, bool act, const char *what)
{
	sb_addf(out, "%s %s\n", act ? "removed" : "would remove", what);
}

/* State files of git carve when no operation is in progress. */
static void stale_state(struct repo *r, bool act, struct strbuf *out)
{
	struct dirent *e;
	DIR *d;

	if (have_ref(r, SPLIT_REF) || git_path_exists(r, "carve/op") ||
	    rebase_in_progress(r) || git_path_exists(r, "CHERRY_PICK_HEAD"))
		return;
	d = opendir(r->carve_dir);
	if (!d)
		return;
	while ((e = readdir(d))) {
		struct strbuf w = STRBUF_INIT;
		char *p;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		sb_addf(&w, "state file carve/%s (no operation in progress)",
			e->d_name);
		say(out, act, w.buf);
		p = carve_path(r, e->d_name);
		if (act)
			unlink(p);
		free(p);
		sb_release(&w);
	}
	closedir(d);
	if (act)
		rmdir(r->carve_dir);
}

/* Temporary copies of the index whose process is gone. */
static void stale_indexes(struct repo *r, bool act, struct strbuf *out)
{
	char *idx = repo_git_path_dup(r, "index");
	struct strbuf prefix = STRBUF_INIT;
	char *slash, *dir, *base;
	struct dirent *e;
	DIR *d;

	if (!idx)
		return;
	slash = strrchr(idx, '/');
	dir = slash ? xstrndup(idx, (size_t)(slash - idx)) : xstrdup(".");
	base = slash ? slash + 1 : idx;
	sb_addf(&prefix, "%s.carve-", base);
	d = opendir(dir);
	while (d && (e = readdir(d))) {
		struct strbuf p = STRBUF_INIT, w = STRBUF_INIT;

		if (!owner_gone(e->d_name, prefix.buf, '-'))
			continue;
		sb_addf(&p, "%s/%s", dir, e->d_name);
		sb_addf(&w, "temporary index %s (its process is gone)", p.buf);
		say(out, act, w.buf);
		if (act)
			unlink(p.buf);
		sb_release(&p);
		sb_release(&w);
	}
	if (d)
		closedir(d);
	free(dir);
	free(idx);
	sb_release(&prefix);
}

/*
 * Worktrees of an interrupted git carve check whose process is gone.
 */
static void stale_worktrees(struct repo *r, bool act, struct strbuf *out)
{
	struct strbuf wl = STRBUF_INIT;
	const char *pos, *end, *l;
	bool prune = false;
	size_t len;

	if (git_out(r, &wl, "worktree", "list", "--porcelain", NULL))
		return;
	pos = sb_str(&wl);
	end = sb_str(&wl) + wl.len;
	while ((l = next_line(&pos, end, &len))) {
		struct strbuf w = STRBUF_INIT;
		char *path, *base;
		struct stat s;

		if (len < 10 || strncmp(l, "worktree ", 9))
			continue;
		path = xstrndup(l + 9, len - 9);
		base = strrchr(path, '/');
		base = base ? base + 1 : path;
		if (!owner_gone(base, "git-carve-check.", '.')) {
			free(path);
			continue;
		}
		sb_addf(&w, "worktree %s of an interrupted git carve check",
			path);
		say(out, act, w.buf);
		if (act) {
			if (stat(path, &s))
				prune = true;
			else
				git_status_of(r, "worktree", "remove",
					      "--force", path, NULL);
		}
		sb_release(&w);
		free(path);
	}
	if (prune)
		git_status_of(r, "worktree", "prune", NULL);
	sb_release(&wl);
}

/*
 * Refs for comments on a series (refs/carve/review, remote/, kept/,
 * versions/), which git carve no longer uses. They go in one transaction.
 */
static void comment_refs(struct repo *r, bool act, struct strbuf *out)
{
	struct strbuf refs = STRBUF_INIT, tx = STRBUF_INIT;
	const char *pos, *end, *l;
	size_t len;

	if (git_out(r, &refs, "for-each-ref", "--format=%(refname)",
		    "refs/carve/review", "refs/carve/remote/",
		    "refs/carve/kept/", "refs/carve/versions/", NULL))
		goto out;
	pos = sb_str(&refs);
	end = pos + refs.len;
	while ((l = next_line(&pos, end, &len))) {
		struct strbuf w = STRBUF_INIT;

		sb_addf(&w, "%.*s: no longer used by git carve", (int)len, l);
		say(out, act, w.buf);
		sb_addf(&tx, "delete %.*s\n", (int)len, l);
		sb_release(&w);
	}
	if (act && tx.len)
		git_in(r, tx.buf, tx.len, NULL, "update-ref", "--stdin", NULL);
out:
	sb_reset(&r->err);
	sb_release(&refs);
	sb_release(&tx);
}

/* What may still hold the user's work is kept, with a hint. */
static void kept(struct repo *r, struct strbuf *out)
{
	struct strbuf parks = STRBUF_INIT;

	if (have_ref(r, WT_REF) && !git_path_exists(r, "carve/op")) {
		char *id = repo_rev(r, WT_REF);

		sb_addf(out,
			"kept " WT_REF ": a work-tree snapshot of an "
			"interrupted fixup-to or defer (git stash apply %s "
			"restores it; git update-ref -d " WT_REF " drops "
			"it)\n",
			id ? id : "?");
		free(id);
	}
	/* parked changes may be the only copy */
	if (!git_out(r, &parks, "for-each-ref", "--format=%(refname)",
		     "refs/worktree/carve/park/", NULL) &&
	    parks.len)
		sb_addf(out,
			"kept parked changes "
			"(git cat-file blob <ref>:patch | git apply puts "
			"one back; git update-ref -d <ref> drops it):\n%s",
			parks.buf);
	sb_release(&parks);
}

/* An entry of dir whose name the test says is dead. */
static bool dead_entry(const char *dir, const char *prefix, char sep)
{
	DIR *d = opendir(dir);
	bool found = false;
	struct dirent *e;

	while (d && !found && (e = readdir(d)))
		found = owner_gone(e->d_name, prefix, sep);
	if (d)
		closedir(d);
	return found;
}

bool gc_leftovers(struct repo *r)
{
	bool op = git_path_exists(r, "carve/op");
	char *idx, *slash, *wt;
	bool found = false;
	struct dirent *e;
	DIR *d;

	/*
	 * state files without an operation (the split's ref looked up only
	 * when there are some)
	 */
	d = op ? NULL : opendir(r->carve_dir);
	while (d && !found && (e = readdir(d)))
		found = strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
	if (d)
		closedir(d);
	if (found &&
	    (rebase_in_progress(r) || git_path_exists(r, "CHERRY_PICK_HEAD") ||
	     have_ref(r, SPLIT_REF)))
		found = false;
	/* temporary indexes and worktrees whose process is gone */
	idx = repo_git_path_dup(r, "index");
	slash = idx ? strrchr(idx, '/') : NULL;
	if (!found && slash) {
		struct strbuf prefix = STRBUF_INIT;

		*slash = '\0';
		sb_addf(&prefix, "%s.carve-", slash + 1);
		found = dead_entry(idx, prefix.buf, '-');
		sb_release(&prefix);
	}
	free(idx);
	wt = repo_git_path_dup(r, "worktrees");
	if (!found && wt)
		found = dead_entry(wt, "git-carve-check.", '.');
	free(wt);
	return found;
}

bool gc_snapshot_kept(struct repo *r)
{
	return !git_path_exists(r, "carve/op") && have_ref(r, WT_REF);
}

int carve_gc(struct repo *r, bool act, struct strbuf *out)
{
	size_t before = out->len;

	stale_state(r, act, out);
	stale_indexes(r, act, out);
	stale_worktrees(r, act, out);
	comment_refs(r, act, out);
	if (undo_gc(r, act, out))
		return -1;
	kept(r, out);
	if (out->len == before)
		sb_adds(out, "nothing to clean up\n");
	else if (!act && strstr(out->buf + before, "would "))
		sb_adds(out, "(a dry run: --force does it)\n");
	return 0;
}
