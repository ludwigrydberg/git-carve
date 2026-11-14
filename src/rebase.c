// SPDX-License-Identifier: MIT
/*
 * The rebase plumbing the operations share. It starts one that stops at a
 * commit, tells where it is, continues and aborts it, and checks what it
 * replayed. Rebases are "git rebase -i" with a scripted sequence editor,
 * and user configuration and hooks apply as usual.
 */
#include "rebase.h"
#include "opstate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUTOSTASH_CONFLICT "Applying autostash resulted in conflicts"

bool rebase_in_progress(struct repo *r)
{
	return git_path_exists(r, "rebase-merge") ||
	       git_path_exists(r, "rebase-apply");
}

/*
 * $GIT_DIR/name into out, found from the carve dir, which is
 * $GIT_DIR/carve, without running git.
 */
static void git_dir_path(struct repo *r, const char *name, struct strbuf *out)
{
	sb_addf(out, "%.*s%s",
		(int)(strrchr(r->carve_dir, '/') + 1 - r->carve_dir),
		r->carve_dir, name);
}

int rebase_amend_follow(struct repo *r, const char *was, const char *now)
{
	struct strbuf path = STRBUF_INIT, cur = STRBUF_INIT;
	int rc = 0;

	git_dir_path(r, "rebase-merge/amend", &path);
	if (read_file(path.buf, &cur))
		goto out; /* nothing to follow without an edit stop */
	sb_rtrim(&cur);
	if (!strcmp(sb_str(&cur), was)) {
		sb_reset(&cur);
		sb_addf(&cur, "%s\n", now);
		if (write_file(path.buf, cur.buf, cur.len))
			rc = repo_err(r, "cannot write %s", path.buf);
	}
out:
	sb_release(&path);
	sb_release(&cur);
	return rc;
}

char *rebase_onto(struct repo *r)
{
	static const char *const files[] = { "rebase-merge/onto",
					     "rebase-apply/onto" };
	struct strbuf path = STRBUF_INIT, sb = STRBUF_INIT;
	char *res = NULL;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(files) && !res; i++) {
		sb_reset(&path);
		sb_reset(&sb);
		git_dir_path(r, files[i], &path);
		if (!read_file(path.buf, &sb)) {
			sb_rtrim(&sb);
			if (sb.len)
				res = sb_detach(&sb);
		}
	}
	sb_release(&path);
	sb_release(&sb);
	return res;
}

/* The number in file name of $GIT_DIR, or 0. */
static int git_path_num(struct repo *r, const char *name)
{
	struct strbuf p = STRBUF_INIT, sb = STRBUF_INIT;
	int n = 0;

	git_dir_path(r, name, &p);
	if (!read_file(p.buf, &sb))
		n = (int)number_or(sb_str(&sb), 0);
	sb_release(&p);
	sb_release(&sb);
	return n;
}

bool rebase_step(struct repo *r, int *step, int *total)
{
	*step = git_path_num(r, "rebase-merge/msgnum");
	*total = git_path_num(r, "rebase-merge/end");
	if (!*step) {
		*step = git_path_num(r, "rebase-apply/next");
		*total = git_path_num(r, "rebase-apply/last");
	}
	return *step > 0;
}

bool foreign_rebase(struct repo *r)
{
	struct strbuf mode = STRBUF_INIT;
	bool res;

	if (!rebase_in_progress(r) || git_path_exists(r, "carve/op"))
		return false;
	state_read(r, "mode", &mode);
	res = !starts_with(sb_str(&mode), "older");
	sb_release(&mode);
	return res;
}

int count_conflicts(struct repo *r)
{
	const char *pos, *end, *l, *prev = NULL;
	struct strbuf out = STRBUF_INIT;
	size_t len, plen = 0;
	int n = 0;

	if (git_out(r, &out, "ls-files", "--unmerged", NULL)) {
		sb_release(&out);
		return 0;
	}
	pos = sb_str(&out);
	end = sb_str(&out) + out.len;
	while ((l = next_line(&pos, end, &len))) {
		const char *tab = memchr(l, '\t', len);
		size_t pl;

		if (!tab)
			continue;
		pl = len - (size_t)(tab + 1 - l);
		if (!prev || pl != plen || memcmp(prev, tab + 1, pl))
			n++;
		prev = tab + 1;
		plen = pl;
	}
	sb_release(&out);
	return n;
}

/*
 * In the output of rev-list --parents --boundary <id>..HEAD, whether id is
 * a boundary commit, or HEAD itself when nothing is listed, and whether a
 * listed commit has more than one parent. Returns true.
 */
static bool series_check(const char *list, const char *id, bool *ancestor,
			 bool *merges)
{
	const char *pos = list, *end = list + strlen(list), *l;
	size_t len, idl = strlen(id);

	*ancestor = !*list;
	*merges = false;
	while ((l = next_line(&pos, end, &len))) {
		const char *sp = memchr(l, ' ', len);

		if (*l == '-') {
			*ancestor |= len - 1 >= idl &&
				     !strncmp(l + 1, id, idl) &&
				     (len - 1 == idl || l[1 + idl] == ' ');
			continue;
		}
		/* "<commit> <parent> <parent>": a merge */
		if (sp && memchr(sp + 1, ' ', len - (size_t)(sp + 1 - l)))
			*merges = true;
	}
	return true;
}

int check_series_commit(struct repo *r, const char *commit, char **full)
{
	struct strbuf range = STRBUF_INIT, out = STRBUF_INIT;
	struct strbuf obj = STRBUF_INIT;
	bool ancestor, merges;
	int np = 0, rc = -1;
	const char *p;
	char *id;

	*full = NULL;
	if (foreign_rebase(r))
		return repo_err(r, YOUR_REBASE_MSG);
	if (rebase_in_progress(r))
		return repo_err(r, REBASE_BUSY_MSG);
	if (op_in_progress(r))
		return repo_err(r, OP_BUSY_MSG);
	id = resolve_commit(r, commit);
	if (!id)
		return repo_err(r, "unknown commit '%s'", commit);
	/* its own parents from the commit object, through cat-file */
	if (repo_cat(r, id, &obj, NULL))
		goto out;
	for (p = sb_str(&obj); (p = strstr(p, "\nparent ")); p++)
		np++;
	if (np > 1) {
		repo_err(r, "%.7s is a merge commit", id);
		goto out;
	}
	/*
	 * One git for the rest, the commits above it with their parents
	 * and the boundary, which shows up when it is an ancestor.
	 */
	sb_addf(&range, "%s..HEAD", id);
	if (git_out(r, &out, "rev-list", "--parents", "--boundary", range.buf,
		    NULL))
		goto out;
	if (!series_check(sb_str(&out), id, &ancestor, &merges))
		goto out;
	if (!ancestor) {
		repo_err(r, "%.7s is not an ancestor of HEAD", id);
		goto out;
	}
	if (merges) {
		repo_err(r,
			 "there are merges between %.7s and HEAD; "
			 "linearize the series first",
			 id);
		goto out;
	}
	/* the commits above it are kept for list_commits while HEAD stays */
	free(r->sl_id);
	free(r->sl_head);
	r->sl_id = xstrdup(id);
	r->sl_head = repo_rev(r, "HEAD");
	sb_release(&r->sl);
	sb_add(&r->sl, sb_str(&out), out.len);
	*full = id;
	id = NULL;
	rc = 0;
out:
	free(id);
	sb_release(&range);
	sb_release(&out);
	sb_release(&obj);
	return rc;
}

/*
 * lo..tip, oldest first and lo itself first if incl, from what
 * check_series_commit listed. lo is its commit, and tip and HEAD are still
 * the HEAD it listed from. The series is linear, and that listing has
 * every commit after its child.
 */
static bool listed(struct repo *r, const char *lo, const char *tip, bool incl,
		   struct strbuf *out)
{
	char **ids = NULL, *head, *t;
	const char *pos, *end, *l;
	size_t len, n = 0, i;
	bool ok;

	if (!r->sl_id || !r->sl_head || strcmp(lo, r->sl_id))
		return false;
	head = repo_rev(r, "HEAD");
	t = !strcmp(tip, "HEAD") ? NULL : repo_rev(r, tip);
	ok = head && !strcmp(head, r->sl_head) &&
	     (!t || !strcmp(t, r->sl_head));
	free(head);
	free(t);
	if (!ok)
		return false;
	pos = sb_str(&r->sl);
	end = pos + r->sl.len;
	while ((l = next_line(&pos, end, &len))) {
		if (*l == '-')
			continue; /* the boundary is lo */
		ids = xrealloc(ids, (n + 1) * sizeof(*ids));
		ids[n++] = xstrndup(l, strcspn(l, " \n"));
	}
	if (incl)
		sb_addf(out, "%s\n", lo);
	for (i = n; i-- > 0;) {
		sb_addf(out, "%s\n", ids[i]);
		free(ids[i]);
	}
	free(ids);
	return true;
}

int list_commits(struct repo *r, const char *range, struct strbuf *out)
{
	const char *dots = strstr(range, "..");

	if (dots && dots[2] != '.' && r->sl_id) {
		char *lo = xstrndup(range, (size_t)(dots - range));
		const char *tip = dots[2] ? dots + 2 : "HEAD";
		bool done = listed(r, lo, tip, false, out);

		if (!done) {
			/* from its parent, the commit itself first */
			char *p = parent_of(r, r->sl_id);

			done = p && !strcmp(p, lo) &&
			       listed(r, r->sl_id, tip, true, out);
			free(p);
		}
		free(lo);
		if (done)
			return 0;
	}
	return git_out(r, out, "rev-list", "--reverse", range, NULL) ? -1 : 0;
}

/* Trees of the given commits (one id per line), one per line. */
static int trees_of(struct repo *r, const char *ids, struct strbuf *out)
{
	const char *end = ids + strlen(ids);
	struct strbuf e = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	const char *pos = ids;
	const char *l;
	size_t len;
	int rc;

	argv_pushl(&a, "log", "--no-walk=unsorted", "--format=%T", NULL);
	while ((l = next_line(&pos, end, &len))) {
		char *id = xstrndup(l, len);

		argv_push(&a, id);
		free(id);
	}
	if (a.n == 3) {
		argv_clear(&a);
		return 0;
	}
	rc = run_git(&a, NULL, out, &e);
	if (rc)
		git_fail(r, "git log", &e);
	argv_clear(&a);
	sb_release(&e);
	return rc ? -1 : 0;
}

static const char *nth_line(const char *s, size_t n)
{
	while (n-- && s) {
		s = strchr(s, '\n');
		if (s)
			s++;
	}
	return s;
}

int check_replayed(struct repo *r, const char *old_ids, const char *range)
{
	struct strbuf ids = STRBUF_INIT, ot = STRBUF_INIT, nt = STRBUF_INIT;
	size_t no, nn, i;
	int rc = -1;

	if (!old_ids || list_commits(r, range, &ids))
		goto unknown;
	no = count_lines(old_ids, strlen(old_ids));
	nn = count_lines(ids.buf, ids.len);
	if (no != nn) {
		repo_err(r,
			 "the replayed series differs: %zu commits became %zu",
			 no, nn);
		goto out;
	}
	if (trees_of(r, old_ids, &ot) || trees_of(r, sb_str(&ids), &nt))
		goto unknown;
	for (i = 0; i < no; i++) {
		const char *a = nth_line(sb_str(&ot), i);
		const char *b = nth_line(sb_str(&nt), i);

		if (!a || !b || strncmp(a, b, strcspn(a, "\n"))) {
			repo_err(
				r,
				"the replayed series differs: %.7s became %.7s "
				"with a different tree",
				nth_line(old_ids, i),
				nth_line(sb_str(&ids), i));
			goto out;
		}
	}
	rc = 0;
	goto out;
unknown:
	sb_reset(&r->err);
	repo_warn(r, "warning: the series is replayed, but git could not "
		     "list it to check its trees");
	rc = 0;
out:
	sb_release(&ids);
	sb_release(&ot);
	sb_release(&nt);
	return rc;
}

int series_revs(struct repo *r, struct argv *a, char **name)
{
	char *up = repo_upstream(r);
	char num[32];

	if (name)
		*name = NULL;
	if (up) {
		argv_push(a, "HEAD");
		argv_push(a, "--not");
		argv_push(a, up);
		if (name)
			*name = xstrdup(up +
					(starts_with(up, "refs/remotes/") ? 13 :
					 starts_with(up, "refs/heads/")	  ? 11 :
									    0));
		free(up);
		return 0;
	}
	if (repo_has_remotes(r)) {
		argv_pushl(a, "HEAD", "--not", "--remotes", NULL);
		if (name)
			*name = xstrdup("pushed");
		return 0;
	}
	snprintf(num, sizeof(num), "-n%d", SERIES_MAX);
	argv_pushl(a, num, "HEAD", NULL);
	return 1;
}

long series_count(struct repo *r, const char *tip, const char *base)
{
	struct strbuf spec = STRBUF_INIT;
	char *cnt;
	long n;

	if (base) {
		sb_addf(&spec, "^%s", base);
		cnt = git_line(r, "rev-list", "--count", "--first-parent", tip,
			       spec.buf, NULL);
	} else {
		cnt = git_line(r, "rev-list", "--count", "--first-parent", tip,
			       "--not", "--remotes", NULL);
	}
	n = number_or(cnt, -1);
	free(cnt);
	sb_release(&spec);
	return n;
}

int rebase_start(struct repo *r, const char *base, const char *seq,
		 bool autosquash, struct strbuf *err)
{
	const char *env[] = { NULL, GIT_EDITOR_QUIET, NULL };
	struct run_opts o = { .env = env };
	struct strbuf envs = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	int rc;

	sb_addf(&envs, "GIT_SEQUENCE_EDITOR=%s", seq ? seq : "true");
	env[0] = envs.buf;
	argv_pushl(&a, "rebase", "-i", "--no-autostash",
		   autosquash ? "--autosquash" : "--no-autosquash",
		   base ? base : "--root", NULL);
	rc = run_git(&a, &o, NULL, err);
	argv_clear(&a);
	sb_release(&envs);
	return rc;
}

int rebase_edit_at(struct repo *r, const char *commit, const char *break_after)
{
	struct strbuf ed = STRBUF_INIT, e = STRBUF_INIT;
	char *parent = parent_of(r, commit);
	char *head = NULL;
	int rc = -1;

	/*
	 * The sequence editor marks commit "edit" and adds "break" after
	 * break_after. Git runs it as sh -c '<editor> "$@"', and the
	 * trailing '#' swallows the "$@" it appends.
	 */
	sb_addf(&ed,
		"awk -v c=%s -v b=%s '"
		"($1 == \"pick\" || $1 == \"p\") && index(c, $2) == 1 && !e "
		"{ $1 = \"edit\"; e = 1 } "
		"{ print } "
		"b != \"\" && ($1 == \"pick\" || $1 == \"p\" || $1 == \"edit\") "
		"&& index(b, $2) == 1 { print \"break\" }"
		"' \"$1\" >\"$1.carve\" && mv \"$1.carve\" \"$1\" #",
		commit, break_after ? break_after : "\"\"");
	rebase_start(r, parent, ed.buf, false, &e);
	head = repo_rev(r, "HEAD");
	if (!rebase_in_progress(r) || !head || strcmp(head, commit)) {
		git_fail(r, "git rebase", &e);
		if (rebase_in_progress(r))
			rebase_abort(r);
		if (!r->err.len)
			repo_err(r, "rebase did not stop at %.7s", commit);
		rc = -1;
	} else {
		rc = 0;
	}
	free(parent);
	free(head);
	sb_release(&ed);
	sb_release(&e);
	return rc;
}

int rebase_continue(struct repo *r)
{
	struct strbuf e = STRBUF_INIT, out = STRBUF_INIT;
	struct run_opts o = { .env = git_quiet_editor };
	struct argv a = ARGV_INIT;
	int rc;

	argv_pushl(&a, "rebase", "--continue", NULL);
	rc = run_git(&a, &o, &out, &e);
	argv_clear(&a);
	if (strstr(sb_str(&e), AUTOSTASH_CONFLICT) ||
	    strstr(sb_str(&out), AUTOSTASH_CONFLICT))
		repo_warn(r, "warning: re-applying your uncommitted changes "
			     "conflicted; they are kept in the stash list");
	if (rebase_in_progress(r)) {
		if (rc)
			git_fail(r, "git rebase --continue", &e);
		rc = 1;
	} else if (rc) {
		git_fail(r, "git rebase --continue", &e);
		rc = -1;
	}
	sb_release(&e);
	sb_release(&out);
	return rc;
}

int rebase_abort(struct repo *r)
{
	return git_quiet(r, "rebase", "--abort", NULL) ? -1 : 0;
}
