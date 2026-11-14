// SPDX-License-Identifier: MIT
/* Undo entries and reverting them (see undo.h). */
#include "undo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "opstate.h"
#include "patch.h"

#define HEAD_MOVED_MSG \
	"HEAD has moved since '%s' (use --force to undo it anyway)"

/* undo entries are git-carve's own bookkeeping, not the user's commits */
static const char *const undo_ident[] = {
	"GIT_AUTHOR_NAME=git-carve",
	"GIT_AUTHOR_EMAIL=git-carve@localhost",
	"GIT_COMMITTER_NAME=git-carve",
	"GIT_COMMITTER_EMAIL=git-carve@localhost",
	NULL,
};

/*
 * The state, with head as HEAD if given. A failed write-tree leaves its
 * reason in r->err when report is set.
 */
static int capture_head(struct repo *r, struct undo_state *st, bool report,
			const char *head)
{
	struct strbuf out = STRBUF_INIT, keep = STRBUF_INIT;
	int tries, rc = -1;

	if (head) {
		st->head = xstrdup(head);
	} else {
		char *h = repo_rev(r, "HEAD");

		st->head = h ? h : xstrdup("-");
	}
	if (r->ix_tree && r->ix_tree_stamp) {
		char *now = repo_index_stamp(r);
		bool same;

		same = now && !strcmp(now, r->ix_tree_stamp);

		free(now);
		if (same) {
			st->index = xstrdup(r->ix_tree);
			return 0;
		}
	}
	sb_add(&keep, sb_str(&r->err), r->err.len);
	/*
	 * this fails while the index has conflicts, and then there is
	 * nothing to record
	 */
	for (tries = 0; tries < 20; tries++) {
		struct timespec ts = { 0, 50L * 1000 * 1000 };

		sb_reset(&out);
		if (!git_out(r, &out, "write-tree", NULL)) {
			sb_rtrim(&out);
			st->index = sb_detach(&out);
			rc = 0;
			break;
		}
		/* another git (an editor's, say) holds the index lock */
		if (!strstr(sb_str(&r->err), "index.lock"))
			break;
		nanosleep(&ts, NULL);
	}
	/* keep the error of the command being recorded */
	if (!rc || !report) {
		sb_reset(&r->err);
		sb_add(&r->err, sb_str(&keep), keep.len);
	}
	sb_release(&out);
	sb_release(&keep);
	return rc;
}

static int capture(struct repo *r, struct undo_state *st, bool report)
{
	return capture_head(r, st, report, NULL);
}

static int undo_capture(struct repo *r, struct undo_state *st)
{
	return capture(r, st, false);
}

/*
 * The names of the undo refs and the pending one, one per line, sorted.
 * That is one git for-each-ref per command, since undo_begin leaves the
 * list for the first load_refs of undo_end and nothing between them
 * touches the refs. load_refs uses it up.
 */
static int list_refs(struct repo *r, struct strbuf *out)
{
	if (!r->undo_refs_valid) {
		sb_reset(&r->undo_refs);
		if (git_out(r, &r->undo_refs, "for-each-ref",
			    "--format=%(refname)", UNDO_REFS, UNDO_PENDING,
			    NULL))
			return -1;
		r->undo_refs_valid = true;
	}
	sb_add(out, sb_str(&r->undo_refs), r->undo_refs.len);
	return 0;
}

/* The newest entry's ref in a list_refs answer, or NULL. */
static char *newest_ref(const struct strbuf *refs)
{
	const char *pos = sb_str(refs), *end = pos + refs->len, *l;
	char *last = NULL;
	size_t len;

	while ((l = next_line(&pos, end, &len)))
		if (len > strlen(UNDO_REFS) &&
		    !strncmp(l, UNDO_REFS, strlen(UNDO_REFS))) {
			free(last);
			last = xstrndup(l, len);
		}
	return last;
}

/* Commands that change only the index, where HEAD stays where it was. */
static bool index_only(const char *op)
{
	static const char *const ops[] = { "stage",	 "unstage",
					   "stage-file", "unstage-file",
					   "set-staged", NULL };
	size_t i;

	for (i = 0; op && ops[i]; i++)
		if (!strcmp(op, ops[i]))
			return true;
	return false;
}

/*
 * The state before a command. The index tree comes from the newest undo
 * entry when the index file has not changed since it was recorded, which
 * spares a git write-tree (a tenth of a second or more on a kernel tree).
 */
static int capture_before(struct repo *r, struct undo_state *st, const char *op)
{
	struct strbuf last = STRBUF_INIT, refs = STRBUF_INIT;
	char *stamp = repo_index_stamp(r);
	char *newest = NULL;
	const char *s, *t;
	char *head;

	/*
	 * The same index file as after this run's last command has its
	 * tree. HEAD comes from the cat-file worker, since it may have
	 * moved meanwhile.
	 */
	if (stamp && r->undo_stamp && !strcmp(stamp, r->undo_stamp)) {
		head = repo_rev(r, "HEAD");
		st->head = head ? head : xstrdup("-");
		st->index = xstrdup(r->undo_index);
		st->stamp = stamp;
		return 0;
	}
	if (stamp && !list_refs(r, &refs) && (newest = newest_ref(&refs)) &&
	    !repo_cat(r, newest, &last, NULL) &&
	    (s = strstr(sb_str(&last), "\nafter-stamp ")) &&
	    !strncmp(s + 13, stamp, strlen(stamp)) &&
	    s[13 + strlen(stamp)] == '\n' &&
	    (t = strstr(sb_str(&last), "\nafter-index ")) &&
	    strncmp(t + 13, "-\n", 2)) {
		head = repo_rev(r, "HEAD");
		st->head = head ? head : xstrdup("-");
		st->index = xstrndup(t + 13, strcspn(t + 13, "\n"));
		st->stamp = stamp;
		sb_release(&last);
		sb_release(&refs);
		free(newest);
		return 0;
	}
	sb_release(&last);
	sb_release(&refs);
	free(newest);
	free(stamp);
	return undo_capture(r, st);
}

/* Blob id of data, stored in the object database if write. */
static char *hash_data(struct repo *r, const char *data, size_t len, bool write)
{
	struct strbuf out = STRBUF_INIT;

	if (write ? git_in(r, data, len, &out, "hash-object", "--no-filters",
			   "-w", "--stdin", NULL) :
		    git_in(r, data, len, &out, "hash-object", "--no-filters",
			   "--stdin", NULL)) {
		sb_release(&out);
		return NULL;
	}
	sb_rtrim(&out);
	return sb_detach(&out);
}

int undo_capture_files(struct repo *r, struct undo_state *st,
		       const char *const *paths, size_t n, bool write)
{
	size_t i;

	st->files = xrealloc(st->files, (st->nfiles + n) * sizeof(*st->files));
	for (i = 0; i < n; i++) {
		struct undo_file *f = &st->files[st->nfiles];
		struct strbuf data = STRBUF_INIT;
		const char *mode = NULL;
		struct stat s;

		f->path = xstrdup(paths[i]);
		f->blob = NULL;
		if (lstat(paths[i], &s)) {
			mode = "-";
		} else if (S_ISLNK(s.st_mode)) {
			char buf[4096];
			ssize_t k = readlink(paths[i], buf, sizeof(buf));

			if (k >= 0) {
				sb_add(&data, buf, (size_t)k);
				mode = "120000";
			}
		} else if (S_ISREG(s.st_mode) && !read_file(paths[i], &data)) {
			mode = s.st_mode & 0111 ? "100755" : "100644";
		}
		if (mode && strcmp(mode, "-"))
			f->blob = hash_data(r, sb_str(&data), data.len, write);
		else if (mode)
			f->blob = xstrdup("-");
		sb_release(&data);
		if (!f->blob) {
			free(f->path);
			return repo_err(r, "cannot save %s for undo", paths[i]);
		}
		f->mode = xstrdup(mode);
		st->nfiles++;
	}
	return 0;
}

void undo_state_release(struct undo_state *st)
{
	size_t i;

	for (i = 0; i < st->nfiles; i++) {
		free(st->files[i].path);
		free(st->files[i].blob);
		free(st->files[i].mode);
	}
	free(st->files);
	free(st->head);
	free(st->index);
	free(st->stamp);
	free(st->stages);
	memset(st, 0, sizeof(*st));
}

static bool same(const char *a, const char *b)
{
	return a && b && !strcmp(a, b);
}

bool undo_state_equal(const struct undo_state *a, const struct undo_state *b)
{
	size_t i;

	if (!same(a->head, b->head) || !same(a->index, b->index) ||
	    ((a->stages || b->stages) && !same(a->stages, b->stages)) ||
	    a->nfiles != b->nfiles)
		return false;
	for (i = 0; i < a->nfiles; i++)
		if (!same(a->files[i].blob, b->files[i].blob) ||
		    !same(a->files[i].mode, b->files[i].mode))
			return false;
	return true;
}

void undo_format(const char *op, const char *what,
		 const struct undo_state *before,
		 const struct undo_state *after, int pending_seq,
		 struct strbuf *out)
{
	size_t i;

	if (!what)
		what = "";
	sb_addf(out, "carve undo: %s%s%s\n\nop %s\nwhat %s\n", op,
		*what ? " " : "", what, op, what);
	sb_addf(out, "before-head %s\n", before->head);
	if (after->stamp)
		sb_addf(out, "after-stamp %s\n", after->stamp);
	sb_addf(out, "after-head %s\nafter-index %s\n",
		after->head ? after->head : "-",
		after->index ? after->index : "-");
	/* file <before blob> <mode> <after blob> <mode> <path> */
	for (i = 0; i < before->nfiles && i < after->nfiles; i++)
		sb_addf(out, "file %s %s %s %s %s\n", before->files[i].blob,
			before->files[i].mode, after->files[i].blob,
			after->files[i].mode, before->files[i].path);
	if (before->stages && after->stages)
		sb_addf(out, "stages-before %s\nstages-after %s\n",
			before->stages, after->stages);
	if (pending_seq >= 0)
		sb_addf(out, "pending-seq %d\n", pending_seq);
}

static char *field(const char *l, size_t len, const char *key)
{
	size_t k = strlen(key);

	if (len > k && !strncmp(l, key, k) && l[k] == ' ')
		return xstrndup(l + k + 1, len - k - 1);
	return NULL;
}

static void set_field(char **dst, char *v)
{
	if (v) {
		free(*dst);
		*dst = v;
	}
}

/* "file <blob> <mode> <blob> <mode> <path>" into both states. */
static int parse_file(char *v, struct undo_entry *e)
{
	char *w[4], *p = v;
	int i;

	for (i = 0; i < 4; i++) {
		char *sp = strchr(p, ' ');

		if (!sp)
			return -1;
		*sp = '\0';
		w[i] = p;
		p = sp + 1;
	}
	e->before.files =
		xrealloc(e->before.files,
			 (e->before.nfiles + 1) * sizeof(struct undo_file));
	e->after.files =
		xrealloc(e->after.files,
			 (e->after.nfiles + 1) * sizeof(struct undo_file));
	e->before.files[e->before.nfiles++] =
		(struct undo_file){ xstrdup(p), xstrdup(w[0]), xstrdup(w[1]) };
	e->after.files[e->after.nfiles++] =
		(struct undo_file){ xstrdup(p), xstrdup(w[2]), xstrdup(w[3]) };
	return 0;
}

int undo_parse(const char *obj, size_t olen, struct undo_entry *e)
{
	const char *pos = obj, *end = obj + olen, *l;
	bool body = false;
	size_t len;
	char *v;

	memset(e, 0, sizeof(*e));
	e->pending_seq = -1;
	while ((l = next_line(&pos, end, &len))) {
		/* "patch:" and the patch itself, to the end */
		if (body && len == 6 && !strncmp(l, "patch:", 6)) {
			e->patch = xstrndup(pos, (size_t)(end - pos));
			break;
		}
		if (!body) {
			if (!len) {
				body = true;
				continue;
			}
			set_field(&e->before.index, field(l, len, "tree"));
			/* in older entries HEAD before is the first parent */
			if (!e->before.head)
				e->before.head = field(l, len, "parent");
			v = field(l, len, "committer");
			if (v) {
				/* "name <email> <time> <tz>" */
				char *gt = strrchr(v, '>');

				e->time = gt ? strtol(gt + 1, NULL, 10) : 0;
				free(v);
			}
			continue;
		}
		set_field(&e->op, field(l, len, "op"));
		set_field(&e->what, field(l, len, "what"));
		set_field(&e->before.head, field(l, len, "before-head"));
		set_field(&e->after.head, field(l, len, "after-head"));
		set_field(&e->after.index, field(l, len, "after-index"));
		set_field(&e->after.stamp, field(l, len, "after-stamp"));
		set_field(&e->before.stages, field(l, len, "stages-before"));
		set_field(&e->after.stages, field(l, len, "stages-after"));
		v = field(l, len, "patch-reverse");
		if (v) {
			e->patch_reverse = number_or(v, 0) != 0;
			free(v);
		}
		v = field(l, len, "patch-zero");
		if (v) {
			e->patch_zero = number_or(v, 0) != 0;
			free(v);
		}
		v = field(l, len, "file");
		if (v) {
			int bad = parse_file(v, e);

			free(v);
			if (bad) {
				undo_entry_release(e);
				return -1;
			}
		}
		v = field(l, len, "pending-seq");
		if (v) {
			e->pending_seq = (int)number_or(v, -1);
			free(v);
		}
	}
	if (!e->what)
		e->what = xstrdup("");
	if (!e->before.head)
		e->before.head = xstrdup("-");
	if (!e->before.index || !e->op || !e->after.head || !e->after.index) {
		undo_entry_release(e);
		return -1;
	}
	if (!strcmp(e->after.index, "-")) {
		free(e->after.index);
		e->after.index = NULL;
	}
	return 0;
}

void undo_entry_release(struct undo_entry *e)
{
	free(e->patch);
	free(e->ref);
	free(e->op);
	free(e->what);
	undo_state_release(&e->before);
	undo_state_release(&e->after);
	memset(e, 0, sizeof(*e));
}

void undo_list_free(struct undo_entry *e, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		undo_entry_release(&e[i]);
	free(e);
}

/*
 * All of refs/worktree/carve/, the entries oldest first and the pending
 * one, with their commits. One git for-each-ref lists them, and the
 * commits come through the running cat-file.
 */
static int flush(struct repo *r, bool keep_list);

static int load_all(struct repo *r, struct undo_entry **out, size_t *n,
		    struct undo_entry *pending, bool *have_pending)
{
	struct strbuf refs = STRBUF_INIT, obj = STRBUF_INIT;
	size_t len, nv = 0, alloc = 0;
	struct undo_entry *v = NULL;
	const char *pos, *end, *l;

	*have_pending = false;
	if (list_refs(r, &refs)) {
		sb_release(&refs);
		return -1;
	}
	/* used once, as in load_refs */
	r->undo_refs_valid = false;
	/* the commits through the running cat-file */
	pos = sb_str(&refs);
	end = sb_str(&refs) + refs.len;
	while ((l = next_line(&pos, end, &len))) {
		char *ref = xstrndup(l, len);
		struct undo_entry e;

		sb_reset(&obj);
		if (repo_cat(r, ref, &obj, NULL) ||
		    undo_parse(obj.buf, obj.len, &e)) {
			free(ref);
			continue;
		}
		e.ref = ref;
		if (!strcmp(e.ref, UNDO_PENDING)) {
			*pending = e;
			*have_pending = true;
			continue;
		}
		e.seq = (int)number_or(e.ref + strlen(UNDO_REFS), 0);
		ALLOC_GROW(v, nv + 1, alloc);
		v[nv++] = e;
	}
	sb_release(&refs);
	sb_release(&obj);
	*out = v;
	*n = nv;
	return 0;
}

int undo_list(struct repo *r, struct undo_entry **out, size_t *n)
{
	struct undo_entry pending;
	bool have;
	int rc;

	rc = flush(r, true) ? -1 : load_all(r, out, n, &pending, &have);

	if (!rc && have)
		undo_entry_release(&pending);
	return rc;
}

static int undo_limit(struct repo *r)
{
	return (int)repo_config_int(r, "carve.undoLimit", 50);
}

/* The state file of an operation spanning several commands exists. */
static bool op_file(struct repo *r)
{
	char *p = carve_path(r, "op");
	struct stat s;
	bool res;

	res = stat(p, &s) == 0;

	free(p);
	return res;
}

/* A git-carve operation spanning several commands is in progress. */
static bool op_active(struct repo *r)
{
	char *orig;
	bool res;

	if (op_file(r))
		return true;
	orig = repo_rev(r, SPLIT_REF);
	res = orig != NULL;
	free(orig);
	return res;
}

/*
 * For record, only the entries' refs and numbers (from list_refs), the
 * pending entry in full, and whether SPLIT_REF exists, which means a split
 * does.
 */
static int load_refs(struct repo *r, struct undo_entry **out, size_t *n,
		     struct undo_entry *pending, bool *have_pending, bool *orig)
{
	struct strbuf refs = STRBUF_INIT, obj = STRBUF_INIT;
	size_t len, nv = 0, alloc = 0;
	struct undo_entry *v = NULL;
	const char *pos, *end, *l;
	int rc = 0;

	*have_pending = *orig = false;
	if (list_refs(r, &refs)) {
		sb_release(&refs);
		return -1;
	}
	/* used once, since what follows may write refs (undo_flush, record) */
	r->undo_refs_valid = false;
	/*
	 * a split the command began or ended is asked for now, not listed
	 * before
	 */
	*orig = !repo_rev_status(r, SPLIT_REF);
	pos = sb_str(&refs);
	end = sb_str(&refs) + refs.len;
	while ((l = next_line(&pos, end, &len))) {
		struct undo_entry e = { 0 };

		if (len == strlen(UNDO_PENDING) &&
		    !strncmp(l, UNDO_PENDING, len)) {
			if (repo_cat(r, UNDO_PENDING, &obj, NULL) ||
			    undo_parse(obj.buf, obj.len, pending)) {
				rc = -1;
				break;
			}
			pending->ref = xstrdup(UNDO_PENDING);
			*have_pending = true;
			continue;
		}
		e.ref = xstrndup(l, len);
		e.seq = (int)number_or(e.ref + strlen(UNDO_REFS), 0);
		ALLOC_GROW(v, nv + 1, alloc);
		v[nv++] = e;
	}
	sb_release(&refs);
	sb_release(&obj);
	if (rc) {
		undo_list_free(v, nv);
		if (*have_pending)
			undo_entry_release(pending);
		*have_pending = false;
		return rc;
	}
	*out = v;
	*n = nv;
	return 0;
}

/*
 * A commit holding the saved work-tree files by position (f0, f1, ...),
 * which makes git gc keep their blobs. NULL when there are none.
 */
static char *files_commit(struct repo *r, const struct undo_state *before,
			  int *rc)
{
	struct strbuf in = STRBUF_INIT, tree = STRBUF_INIT, out = STRBUF_INIT;
	size_t i;

	*rc = 0;
	for (i = 0; i < before->nfiles; i++)
		if (strcmp(before->files[i].blob, "-"))
			sb_addf(&in, "%s blob %s\tf%zu\n",
				before->files[i].mode, before->files[i].blob,
				i);
	if (!in.len)
		return NULL;
	if (git_in(r, in.buf, in.len, &tree, "mktree", NULL)) {
		*rc = -1;
		goto out;
	}
	sb_rtrim(&tree);
	if (git_env(r, undo_ident, &out, "commit-tree", "--no-gpg-sign", "-m",
		    "carve undo: saved work-tree files", tree.buf, NULL)) {
		*rc = -1;
		goto out;
	}
	sb_rtrim(&out);
out:
	sb_release(&in);
	sb_release(&tree);
	if (*rc) {
		sb_release(&out);
		return NULL;
	}
	return sb_detach(&out);
}

static char *make_commit(struct repo *r, const struct undo_state *before,
			 const char *msg)
{
	struct strbuf out = STRBUF_INIT, e = STRBUF_INIT;
	struct run_opts o = { .env = undo_ident };
	struct argv a = ARGV_INIT;
	char *files;
	int rc;

	files = files_commit(r, before, &rc);
	if (rc)
		return NULL;
	argv_pushl(&a, "commit-tree", "--no-gpg-sign", NULL);
	if (strcmp(before->head, "-"))
		argv_pushl(&a, "-p", before->head, NULL);
	if (files)
		argv_pushl(&a, "-p", files, NULL);
	argv_pushl(&a, "-m", msg, before->index, NULL);
	rc = run_git(&a, &o, &out, &e);
	if (rc)
		git_fail(r, "git commit-tree", &e);
	argv_clear(&a);
	sb_release(&e);
	free(files);
	if (rc) {
		sb_release(&out);
		return NULL;
	}
	sb_rtrim(&out);
	return sb_detach(&out);
}

static int max_seq(const struct undo_entry *v, size_t n)
{
	return n ? v[n - 1].seq : 0;
}

/*
 * Write the entry (op, what, before and after) as number seq, delete the
 * refs in drop, and trim the history to limit, all in one transaction.
 */
static int write_entry(struct repo *r, const char *op, const char *what,
		       const struct undo_state *before,
		       const struct undo_state *after, int seq,
		       struct strbuf *tx, const struct undo_entry *v, size_t n,
		       int limit)
{
	struct strbuf msg = STRBUF_INIT;
	size_t i, keep = 0;
	char *id;
	int rc;

	undo_format(op, what, before, after, -1, &msg);
	id = make_commit(r, before, msg.buf);
	sb_release(&msg);
	if (!id)
		return -1;
	sb_addf(tx, "update %s%08d %s\n", UNDO_REFS, seq, id);
	free(id);
	/* entries below seq that survive tx, and the new one counts too */
	for (i = n; i > 0; i--) {
		const struct undo_entry *e = &v[i - 1];

		if (e->seq >= seq)
			continue;
		if (++keep >= (size_t)limit)
			sb_addf(tx, "delete %s\n", e->ref);
	}
	rc = git_in(r, tx->buf, tx->len, NULL, "update-ref", "--stdin", NULL);
	return rc;
}

/* The state after the last command, for the next one's before state. */
static void remember(struct repo *r, const struct undo_state *after)
{
	free(r->undo_stamp);
	free(r->undo_index);
	r->undo_stamp = r->undo_index = NULL;
	if (after && after->index && after->stamp) {
		r->undo_stamp = xstrdup(after->stamp);
		r->undo_index = xstrdup(after->index);
	}
}

/* An entry kept back for undo_flush. */
struct undo_queued {
	char *op, *what;
	struct undo_state before, after;
	long time;
	char *patch; /* a line stage's patch, else NULL */
	bool reverse, zero;
};

static void undo_queue_free(struct repo *r)
{
	size_t i;

	for (i = 0; i < r->undo_nq; i++) {
		free(r->undo_q[i].op);
		free(r->undo_q[i].what);
		free(r->undo_q[i].patch);
		undo_state_release(&r->undo_q[i].before);
		undo_state_release(&r->undo_q[i].after);
	}
	free(r->undo_q);
	r->undo_q = NULL;
	r->undo_nq = r->undo_aq = 0;
}

/* A new queued entry of op on what, from the state before (taken over). */
static struct undo_queued *queue_push(struct repo *r, const char *op,
				      const char *what,
				      const struct undo_state *before)
{
	struct undo_queued *q;

	ALLOC_GROW(r->undo_q, r->undo_nq + 1, r->undo_aq);
	repo_on_release(r, undo_queue_free);
	q = &r->undo_q[r->undo_nq++];
	memset(q, 0, sizeof(*q));
	q->op = xstrdup(op);
	q->what = xstrdup(what);
	q->before = *before;
	return q;
}

bool undo_queued(const struct repo *r)
{
	return r->undo_nq > 0;
}

/*
 * The entry of a line stage begun as_patch is the patch apply_selection
 * applied, when nothing changed the index before it in this command, which
 * makes the index file under the patch the one at undo_begin. The index's
 * tree is never written. What follows the patch (the diffs shown) may
 * write the index again, but only the stat data git refreshes.
 */
static void defer_patch(struct repo *r, struct undo_state *before,
			const char *op, const char *what)
{
	char *now = repo_index_stamp(r);
	struct undo_queued *q;

	if (!r->last_patch.len || !now || !before->stamp ||
	    !same(r->last_patch_from, before->stamp)) {
		if (!same(now, before->stamp))
			repo_warn(r,
				  "'%s' not recorded for undo: the index "
				  "changed in more ways than its lines",
				  op);
		free(now);
		undo_state_release(before);
		return;
	}
	q = queue_push(r, op, what, before);
	q->after.head = xstrdup(before->head);
	q->after.stamp = now;
	q->time = (long)time(NULL);
	q->patch = sb_detach(&r->last_patch);
	q->reverse = r->last_patch_reverse;
	q->zero = r->last_patch_zero;
	memset(before, 0, sizeof(*before));
	/* no tree of the index now, the next command finds its own */
	remember(r, NULL);
}

/*
 * A command that changes only the index, while no operation is in progress
 * and entries are deferred. Capture the state after it with one write-tree
 * and keep the entry for undo_flush, taking over before. Returns false
 * when that does not apply.
 */
static bool defer(struct repo *r, struct undo_state *before, const char *op,
		  const char *what)
{
	struct undo_state after = { 0 };
	struct undo_queued *q;

	if (before->as_patch) {
		defer_patch(r, before, op, what);
		return true;
	}

	if (!r->undo_defer || !r->undo_quiet || !index_only(op) ||
	    !before->index || before->nfiles || undo_limit(r) <= 0 ||
	    op_file(r))
		return false;
	capture_head(r, &after, false, before->head);
	after.stamp = repo_index_stamp(r);
	remember(r, &after);
	if (!after.index || undo_state_equal(before, &after)) {
		undo_state_release(&after);
		undo_state_release(before);
		return true;
	}
	q = queue_push(r, op, what, before);
	q->after = after;
	q->time = (long)time(NULL);
	memset(before, 0, sizeof(*before));
	return true;
}

/* The commit object of a kept entry, as git commit-tree would write it. */
static void queued_commit(struct repo *r, const struct undo_queued *q,
			  struct strbuf *out)
{
	/* a patch entry keeps no index and gets the empty tree */
	sb_addf(out, "tree %s\n",
		q->patch ? repo_empty_tree(r) : q->before.index);
	if (strcmp(q->before.head, "-"))
		sb_addf(out, "parent %s\n", q->before.head);
	sb_addf(out,
		"author git-carve <git-carve@localhost> %ld +0000\n"
		"committer git-carve <git-carve@localhost> %ld +0000\n\n",
		q->time, q->time);
	undo_format(q->op, q->what, &q->before, &q->after, -1, out);
	if (q->patch)
		sb_addf(out, "patch-reverse %d\npatch-zero %d\npatch:\n%s",
			q->reverse, q->zero, q->patch);
}

/*
 * The list load_refs used, as the transaction tx left it, with its deleted
 * refs out and its new ones in (the newest, sorting last). The record that
 * follows a flush then needs no second git for-each-ref.
 */
static void refs_after(struct repo *r, const struct strbuf *tx)
{
	struct strbuf fresh = STRBUF_INIT, del = STRBUF_INIT;
	const char *pos = sb_str(&r->undo_refs);
	const char *end;
	const char *l;
	size_t len;

	end = pos + r->undo_refs.len;

	while ((l = next_line(&pos, end, &len))) {
		sb_reset(&del);
		sb_addf(&del, "delete %.*s\n", (int)len, l);
		if (!strstr(sb_str(tx), del.buf))
			sb_addf(&fresh, "%.*s\n", (int)len, l);
	}
	pos = sb_str(tx);
	end = pos + tx->len;
	while ((l = next_line(&pos, end, &len)))
		if (len > 7 && !strncmp(l, "update ", 7))
			sb_addf(&fresh, "%.*s\n", (int)strcspn(l + 7, " \n"),
				l + 7);
	sb_release(&r->undo_refs);
	r->undo_refs = fresh;
	r->undo_refs_valid = true;
	sb_release(&del);
}

/* keep_list, since a record follows at once and may use the list as left */
static int flush(struct repo *r, bool keep_list)
{
	struct strbuf paths = STRBUF_INIT, ids = STRBUF_INIT;
	struct strbuf tx = STRBUF_INIT, obj = STRBUF_INIT;
	struct undo_entry *v = NULL, pending;
	size_t n = 0, i, skip, len, keep;
	const char *pos, *end, *l;
	int limit = undo_limit(r);
	bool have_pending, orig;
	char **tmp;
	int rc = 0;
	int seq;

	if (!r->undo_nq)
		return 0;
	if (load_refs(r, &v, &n, &pending, &have_pending, &orig)) {
		undo_queue_free(r);
		return -1;
	}
	tmp = xcalloc(r->undo_nq, sizeof(*tmp));
	if (mkdir(r->carve_dir, 0777) && errno != EEXIST) {
		rc = repo_err(r, "cannot create %s: %s", r->carve_dir,
			      strerror(errno));
		goto out;
	}
	/* beyond the limit already, not written at all */
	skip = r->undo_nq > (size_t)limit ? r->undo_nq - (size_t)limit : 0;
	/* the commits, all in one git hash-object */
	for (i = skip; i < r->undo_nq; i++) {
		char name[64];

		snprintf(name, sizeof(name), "undo-%ld-%zu", (long)getpid(), i);
		sb_reset(&obj);
		queued_commit(r, &r->undo_q[i], &obj);
		tmp[i] = carve_path(r, name);
		if (write_file(tmp[i], obj.buf, obj.len)) {
			rc = repo_err(r, "cannot write %s", tmp[i]);
			goto out;
		}
		sb_addf(&paths, "%s\n", tmp[i]);
	}
	if (git_in(r, paths.buf, paths.len, &ids, "hash-object", "-t", "commit",
		   "-w", "--stdin-paths", NULL)) {
		rc = -1;
		goto out;
	}
	/* one transaction for the new refs and the oldest beyond the limit */
	seq = n ? v[n - 1].seq : 0;
	pos = sb_str(&ids);
	end = pos + ids.len;
	for (i = skip; i < r->undo_nq && (l = next_line(&pos, end, &len)); i++)
		sb_addf(&tx, "update %s%08d %.*s\n", UNDO_REFS, ++seq, (int)len,
			l);
	keep = r->undo_nq - skip;
	for (i = n; i > 0; i--)
		if (++keep > (size_t)limit)
			sb_addf(&tx, "delete %s\n", v[i - 1].ref);
	rc = git_in(r, tx.buf, tx.len, NULL, "update-ref", "--stdin", NULL);
	if (!rc && keep_list)
		refs_after(r, &tx);
out:
	for (i = 0; i < r->undo_nq; i++) {
		if (tmp[i])
			unlink(tmp[i]);
		free(tmp[i]);
	}
	free(tmp);
	undo_queue_free(r);
	undo_list_free(v, n);
	if (have_pending)
		undo_entry_release(&pending);
	sb_release(&paths);
	sb_release(&ids);
	sb_release(&tx);
	sb_release(&obj);
	return rc;
}

int undo_flush(struct repo *r)
{
	return flush(r, false);
}

/*
 * The work of undo_end. With before NULL it only settles a finished
 * operation.
 */
static int record(struct repo *r, const struct undo_state *before,
		  const char *op, const char *what)
{
	struct undo_entry *v = NULL, pending;
	struct undo_state after = { 0 };
	bool have_pending, active, orig;
	struct strbuf tx = STRBUF_INIT;
	int limit = undo_limit(r);
	size_t n = 0, i;
	int rc = 0;

	if (limit <= 0)
		return 0;
	/* entries kept back come first */
	if (flush(r, true))
		return -1;
	if (load_refs(r, &v, &n, &pending, &have_pending, &orig))
		return -1;
	capture_head(r, &after, false,
		     before && before->head && index_only(op) ? before->head :
								NULL);
	if (before && before->nfiles) {
		const char **paths = xcalloc(before->nfiles, sizeof(*paths));

		for (i = 0; i < before->nfiles; i++)
			paths[i] = before->files[i].path;
		/* a state that cannot be captured is not recorded */
		if (undo_capture_files(r, &after, paths, before->nfiles,
				       false)) {
			free(after.index);
			after.index = NULL;
		}
		free(paths);
	}
	/* the index as it is now, after the write-tree above */
	after.stamp = repo_index_stamp(r);
	active = op_file(r) || orig;

	if (have_pending && !active) {
		/* the operation ended, fold it and what happened inside */
		/* seq pending_seq + 1 is overwritten by the folded entry */
		for (i = 0; i < n; i++)
			if (v[i].seq > pending.pending_seq + 1 ||
			    (v[i].seq == pending.pending_seq + 1 &&
			     (!after.index ||
			      undo_state_equal(&pending.before, &after))))
				sb_addf(&tx, "delete %s\n", v[i].ref);
		sb_addf(&tx, "delete %s\n", UNDO_PENDING);
		if (after.index && !undo_state_equal(&pending.before, &after))
			rc = write_entry(r, pending.op, pending.what,
					 &pending.before, &after,
					 pending.pending_seq + 1, &tx, v, n,
					 limit);
		else
			rc = git_in(r, tx.buf, tx.len, NULL, "update-ref",
				    "--stdin", NULL);
	} else if (!before || !before->index) {
		/* nothing to record */
	} else if (!have_pending && active) {
		/* an operation began, remember where it started */
		struct strbuf msg = STRBUF_INIT;
		char *id;

		undo_format(op, what, before, &after, max_seq(v, n), &msg);
		id = make_commit(r, before, msg.buf);
		sb_release(&msg);
		rc = id ? git_quiet(r, "update-ref", UNDO_PENDING, id, NULL) :
			  -1;
		free(id);
	} else if (after.index && !undo_state_equal(before, &after)) {
		rc = write_entry(r, op, what, before, &after, max_seq(v, n) + 1,
				 &tx, v, n, limit);
	}
	/* the next command's before state, while the index file stays */
	remember(r, rc ? NULL : &after);
	r->undo_quiet = !rc && !active;
	sb_release(&tx);
	undo_state_release(&after);
	undo_list_free(v, n);
	if (have_pending)
		undo_entry_release(&pending);
	return rc;
}

int undo_begin(struct repo *r, struct undo_state *before, const char *op)
{
	/* a patch from a command before this one is no part of it */
	sb_reset(&r->last_patch);
	free(r->last_patch_from);
	r->last_patch_from = NULL;
	/*
	 * A line stage the caller vouches for, kept back as its patch.
	 * HEAD and the index file's stamp now are all it needs, without a
	 * tree.
	 */
	if (before->as_patch && r->undo_defer && r->undo_quiet &&
	    index_only(op) && undo_limit(r) > 0 && !op_file(r)) {
		char *head = repo_rev(r, "HEAD");

		before->head = head ? head : xstrdup("-");
		before->stamp = repo_index_stamp(r);
		return 0;
	}
	before->as_patch = false;
	return capture_before(r, before, op);
}

/* The index entries of path, "<mode> <blob> <stage>" separated by ",". */
static char *path_stages(struct repo *r, const char *path)
{
	struct strbuf out = STRBUF_INIT, v = STRBUF_INIT;
	const char *p, *end;

	if (git_out(r, &out, "ls-files", "-s", "-z", "--", path, NULL)) {
		sb_release(&out);
		return NULL;
	}
	p = sb_str(&out);
	end = p + out.len;
	while (p < end) {
		size_t len = strlen(p), meta = strcspn(p, "\t");

		sb_addf(&v, "%s%.*s", v.len ? "," : "", (int)meta, p);
		p += len + 1;
	}
	sb_release(&out);
	if (!v.len)
		sb_adds(&v, "-");
	return sb_detach(&v);
}

int undo_conflict_begin(struct repo *r, struct undo_state *before,
			const char *path)
{
	char *head = repo_rev(r, "HEAD");

	memset(before, 0, sizeof(*before));
	before->head = head ? head : xstrdup("-");
	/*
	 * the entry's commit needs a tree, and HEAD's will do since no
	 * tree holds a conflict
	 */
	before->index = head ? repo_rev(r, "HEAD^{tree}") :
			       xstrdup(repo_empty_tree(r));
	before->stages = path_stages(r, path);
	if (!before->index || !before->stages ||
	    undo_capture_files(r, before, &path, 1, true)) {
		undo_state_release(before);
		return repo_err(r, "cannot save %s for undo; nothing changed",
				path);
	}
	return 0;
}

void undo_conflict_end(struct repo *r, struct undo_state *before,
		       const char *path)
{
	struct strbuf tx = STRBUF_INIT, keep = STRBUF_INIT;
	struct undo_entry *v = NULL, pending;
	char *head = repo_rev(r, "HEAD");
	struct undo_state after = { 0 };
	int limit = undo_limit(r);
	bool have_pending, orig;
	size_t n = 0;

	/* the error of the command, if any, stays */
	sb_add(&keep, sb_str(&r->err), r->err.len);
	after.head = head ? head : xstrdup("-");
	after.stages = path_stages(r, path);
	if (!before->stages || limit <= 0 || !after.stages ||
	    undo_capture_files(r, &after, &path, 1, false) ||
	    undo_state_equal(before, &after) ||
	    load_refs(r, &v, &n, &pending, &have_pending, &orig))
		goto out;
	if (write_entry(r, "resolve", path, before, &after, max_seq(v, n) + 1,
			&tx, v, n, limit))
		repo_warn(r, "could not record 'resolve' for undo: %s",
			  sb_str(&r->err));
	undo_list_free(v, n);
	if (have_pending)
		undo_entry_release(&pending);
out:
	sb_reset(&r->err);
	sb_add(&r->err, sb_str(&keep), keep.len);
	r->undo_refs_valid = false;
	remember(r, NULL);
	sb_release(&tx);
	sb_release(&keep);
	undo_state_release(&after);
	undo_state_release(before);
}

void undo_end(struct repo *r, struct undo_state *before, const char *op,
	      const char *what)
{
	bool deferred = defer(r, before, op, what);

	/* the list undo_begin read is not good beyond this command */
	if (!deferred && record(r, before, op, what))
		repo_warn(r, "could not record '%s' for undo: %s", op,
			  sb_str(&r->err));
	r->undo_refs_valid = false;
	undo_state_release(before);
}

/* Create the directories leading to path. */
static void make_parents(const char *path)
{
	char *p = xstrdup(path), *s;

	for (s = strchr(p, '/'); s; s = strchr(s + 1, '/')) {
		*s = '\0';
		mkdir(p, 0777);
		*s = '/';
	}
	free(p);
}

/* Put a saved work-tree file back (or remove it if it did not exist). */
static int restore_file(struct repo *r, const struct undo_file *f)
{
	struct strbuf data = STRBUF_INIT;
	struct stat s;

	if (!strcmp(f->blob, "-")) {
		if (unlink(f->path) && errno != ENOENT)
			return repo_err(r, "cannot remove %s: %s", f->path,
					strerror(errno));
		return 0;
	}
	if (git_out(r, &data, "cat-file", "blob", f->blob, NULL)) {
		sb_release(&data);
		return -1;
	}
	make_parents(f->path);
	if (!lstat(f->path, &s) &&
	    (S_ISLNK(s.st_mode) || !strcmp(f->mode, "120000")))
		unlink(f->path);
	if (!strcmp(f->mode, "120000")) {
		if (symlink(sb_str(&data), f->path))
			goto fail;
	} else {
		if (write_file(f->path, sb_str(&data), data.len))
			goto fail;
		if (!stat(f->path, &s)) {
			mode_t m = s.st_mode & 0777;

			/*
			 * x where r is, like git checkout, with the umask
			 * applied
			 */
			m = !strcmp(f->mode, "100755") ? m | (m & 0444) >> 2 :
							 m & ~(mode_t)0111;
			chmod(f->path, m);
		}
	}
	sb_release(&data);
	return 0;
fail:
	sb_release(&data);
	return repo_err(r, "cannot restore %s: %s", f->path, strerror(errno));
}

/*
 * Make the index the tree want, touching only the paths that differ from
 * the tree it is now (cur): git read-tree would drop the stat data of
 * every entry, and the next status would hash the whole tree again.
 */
static int restore_index(struct repo *r, const char *cur, const char *want)
{
	struct strbuf raw = STRBUF_INIT, info = STRBUF_INIT;
	const char *p, *end;
	int rc = -1;

	if (!strcmp(cur, want))
		return 0;
	if (git_out(r, &raw, "diff-tree", "-r", "-z", "--raw", "--no-renames",
		    cur, want, NULL))
		goto out;
	/* ":<mode> <mode> <sha> <sha> <status>\0<path>\0" */
	p = sb_str(&raw);
	end = p + raw.len;
	while (p < end && *p == ':') {
		const char *meta = p, *path;
		char mode[8], sha[65], st;

		p = memchr(p, '\0', (size_t)(end - p));
		if (!p)
			break;
		path = ++p;
		p = memchr(p, '\0', (size_t)(end - p));
		if (!p)
			break;
		p++;
		if (sscanf(meta, ":%*s %7s %*s %64s %c", mode, sha, &st) != 3)
			goto out;
		if (st == 'D')
			sb_addf(&info, "0 %s\t%s", sha, path);
		else
			sb_addf(&info, "%s %s\t%s", mode, sha, path);
		sb_addch(&info, '\0');
	}
	rc = git_in(r, info.buf, info.len, NULL, "update-index", "-z",
		    "--index-info", NULL) ?
		     -1 :
		     0;
out:
	sb_release(&raw);
	sb_release(&info);
	return rc;
}

/*
 * Undo a line stage kept as its patch by applying the patch the other way
 * round. git apply changes all or nothing, and a patch that does not fit
 * any more because the lines changed leaves the index as it is.
 */
static int undo_patch_entry(struct repo *r, const struct undo_entry *e,
			    bool force, struct strbuf *done)
{
	struct strbuf patch = STRBUF_INIT, tx = STRBUF_INIT;
	char *head = repo_rev(r, "HEAD");
	int rc = -1;

	if (!force && !same(head ? head : "-", e->after.head)) {
		repo_err(r, HEAD_MOVED_MSG, e->op);
		goto out;
	}
	sb_adds(&patch, e->patch);
	if (apply_patch_zero(r, &patch, !e->patch_reverse, e->patch_zero)) {
		repo_err(r,
			 "the index has changed since '%s': its lines are "
			 "no longer as it left them",
			 e->op);
		goto out;
	}
	sb_addf(&tx, "delete %s\n", e->ref);
	if (git_in(r, tx.buf, tx.len, NULL, "update-ref", "--stdin", NULL))
		goto out;
	if (done)
		sb_addf(done, "%s%s%s", e->op, *e->what ? " " : "", e->what);
	remember(r, NULL);
	rc = 0;
out:
	free(head);
	sb_release(&patch);
	sb_release(&tx);
	return rc;
}

/*
 * Undo taking a side of a conflict. The file and the path's index entries,
 * the unmerged stages, come back as they were.
 */
static int undo_conflict_entry(struct repo *r, const struct undo_entry *e,
			       bool force, struct strbuf *done)
{
	const struct undo_file *f = e->before.nfiles ? &e->before.files[0] :
						       NULL;
	struct undo_state cur = { 0 };
	struct strbuf info = STRBUF_INIT, tx = STRBUF_INIT;
	char *stages = NULL;
	const char *p;
	int rc = -1;

	if (!f || !e->after.nfiles)
		return repo_err(r, "the undo entry of '%s' is damaged", e->op);
	p = f->path;
	stages = path_stages(r, p);
	if (!force && (!same(stages, e->after.stages) ||
		       undo_capture_files(r, &cur, &p, 1, false) ||
		       !same(cur.files[0].blob, e->after.files[0].blob))) {
		repo_err(r,
			 "%s has changed since '%s' (use --force to undo it "
			 "anyway, which overwrites it)",
			 p, e->op);
		goto out;
	}
	/* mode 0 drops every entry of the path, then the old ones go in */
	sb_addf(&info, "0 %s\t%s", repo_empty_tree(r), p);
	sb_addch(&info, '\0');
	if (strcmp(e->before.stages, "-")) {
		const char *s = e->before.stages;

		while (*s) {
			/* "<mode> <blob> <stage>" */
			const char *st, *blob = strchr(s, ' ');
			size_t len = strcspn(s, ",");
			int stage;

			st = blob ? strchr(blob + 1, ' ') : NULL;

			if (!st || st > s + len ||
			    parse_int(st + 1, NULL, &stage))
				goto out;
			sb_addf(&info, "%.*s %.*s %d\t%s", (int)(blob - s), s,
				(int)(st - blob - 1), blob + 1, stage, p);
			sb_addch(&info, '\0');
			s += len + (s[len] == ',');
		}
	}
	if (restore_file(r, f) ||
	    git_in(r, info.buf, info.len, NULL, "update-index", "-z",
		   "--index-info", NULL))
		goto out;
	sb_addf(&tx, "delete %s\n", e->ref);
	if (git_in(r, tx.buf, tx.len, NULL, "update-ref", "--stdin", NULL))
		goto out;
	if (done)
		sb_addf(done, "%s %s", e->op, e->what);
	remember(r, NULL);
	rc = 0;
out:
	free(stages);
	undo_state_release(&cur);
	sb_release(&info);
	sb_release(&tx);
	return rc;
}

/* Wait up to a second for another git's index.lock to go away. */
static void wait_index_lock(struct repo *r)
{
	const char *idx = repo_git_path(r, "index");
	struct strbuf lock = STRBUF_INIT;
	struct stat st;
	int tries;

	if (!idx)
		return;
	sb_addf(&lock, "%s.lock", idx);
	for (tries = 0; tries < 20 && !stat(lock.buf, &st); tries++) {
		struct timespec ts = { 0, 50L * 1000 * 1000 };

		nanosleep(&ts, NULL);
	}
	sb_release(&lock);
}

int undo_last(struct repo *r, bool force, struct strbuf *done)
{
	struct strbuf tx = STRBUF_INIT, m = STRBUF_INIT;
	struct undo_entry *v = NULL, pending, *e;
	struct undo_state cur = { 0 };
	char *stamp = NULL;
	bool have_pending;
	size_t n = 0, i;
	int rc = -1;

	if (flush(r, true) || load_all(r, &v, &n, &pending, &have_pending))
		return -1;
	/* fold an operation that ended outside git carve first */
	if (have_pending && !op_active(r)) {
		undo_list_free(v, n);
		undo_entry_release(&pending);
		v = NULL;
		n = 0;
		if (record(r, NULL, NULL, NULL) ||
		    load_all(r, &v, &n, &pending, &have_pending))
			return -1;
	}
	if (!n || (have_pending && v[n - 1].seq <= pending.pending_seq)) {
		if (have_pending)
			repo_err(r,
				 "nothing to undo since the %s in progress "
				 "began; {abort} undoes it",
				 pending.op);
		else
			repo_err(r, "nothing to undo");
		goto out;
	}
	e = &v[n - 1];
	/* another git (an editor's, a background status) may hold the index */
	wait_index_lock(r);
	if (e->patch) {
		rc = undo_patch_entry(r, e, force, done);
		goto out;
	}
	if (e->before.stages) {
		rc = undo_conflict_entry(r, e, force, done);
		goto out;
	}
	stamp = repo_index_stamp(r);
	if (stamp && e->after.stamp && e->after.index &&
	    !strcmp(stamp, e->after.stamp)) {
		/*
		 * the index file the entry left behind has its tree,
		 * without git
		 */
		char *head = repo_rev(r, "HEAD");

		cur.head = head ? head : xstrdup("-");
		cur.index = xstrdup(e->after.index);
	} else if (capture(r, &cur, true)) {
		struct strbuf u = STRBUF_INIT;

		/* say why, a conflict or git's own error such as a lock */
		if (!git_out(r, &u, "ls-files", "-u", NULL) && u.len)
			repo_err(r, "the index has conflicts; resolve them "
				    "first");
		sb_release(&u);
		goto out;
	}
	if (!force && !same(cur.head, e->after.head)) {
		repo_err(r, HEAD_MOVED_MSG, e->op);
		goto out;
	}
	if (!force && !same(cur.index, e->after.index)) {
		repo_err(r,
			 "the index has changed since '%s' (use --force to "
			 "undo it anyway)",
			 e->op);
		goto out;
	}
	for (i = 0; !force && i < e->after.nfiles; i++) {
		const struct undo_file *f = &e->after.files[i];
		const char *p = f->path;

		if (undo_capture_files(r, &cur, &p, 1, false))
			goto out;
		if (!same(cur.files[i].blob, f->blob) ||
		    !same(cur.files[i].mode, f->mode)) {
			repo_err(r,
				 "%s has changed since '%s' (use --force to "
				 "undo it anyway, which overwrites it)",
				 p, e->op);
			goto out;
		}
	}
	if (restore_index(r, cur.index, e->before.index))
		goto out;
	for (i = 0; i < e->before.nfiles; i++)
		if (restore_file(r, &e->before.files[i]))
			goto out;
	/* HEAD back and the entry gone in one transaction */
	if (strcmp(cur.head, e->before.head)) {
		if (!strcmp(e->before.head, "-"))
			sb_adds(&tx, "delete HEAD\n");
		else
			sb_addf(&tx, "update HEAD %s\n", e->before.head);
	}
	sb_addf(&tx, "delete %s\n", e->ref);
	sb_addf(&m, "carve: undo %s", e->op);
	if (git_in(r, tx.buf, tx.len, NULL, "update-ref", "-m", m.buf,
		   "--stdin", NULL))
		goto out;
	if (done)
		sb_addf(done, "%s%s%s", e->op, *e->what ? " " : "", e->what);
	{
		/* the next command starts from the state undone to */
		struct undo_state now = { 0 };

		now.head = xstrdup(e->before.head);
		now.index = xstrdup(e->before.index);
		now.stamp = repo_index_stamp(r);
		remember(r, &now);
		undo_state_release(&now);
	}
	rc = 0;
out:
	undo_state_release(&cur);
	sb_release(&tx);
	sb_release(&m);
	free(stamp);
	undo_list_free(v, n);
	if (have_pending)
		undo_entry_release(&pending);
	return rc;
}

int undo_gc(struct repo *r, bool act, struct strbuf *out)
{
	struct undo_entry *v = NULL, pending = { 0 };
	struct strbuf tx = STRBUF_INIT;
	size_t n = 0, i, keep = 0;
	int limit = undo_limit(r);
	bool have_pending;
	int rc = 0;

	if (flush(r, true) || load_all(r, &v, &n, &pending, &have_pending))
		return -1;
	if (have_pending && !op_active(r)) {
		sb_addf(out,
			"%s the undo steps of the finished %s into one "
			"entry\n",
			act ? "folded" : "would fold", pending.op);
		if (act)
			rc = record(r, NULL, NULL, NULL);
	}
	if (limit > 0) {
		for (i = n; i > 0; i--) {
			if (++keep <= (size_t)limit)
				continue;
			sb_addf(&tx, "delete %s\n", v[i - 1].ref);
			sb_addf(out,
				"%s undo entry %d (%s), beyond "
				"carve.undoLimit %d\n",
				act ? "removed" : "would remove", v[i - 1].seq,
				v[i - 1].op, limit);
		}
		if (act && tx.len && !rc)
			rc = git_in(r, tx.buf, tx.len, NULL, "update-ref",
				    "--stdin", NULL);
	}
	sb_release(&tx);
	undo_list_free(v, n);
	if (have_pending)
		undo_entry_release(&pending);
	return rc;
}

static const struct {
	const char *op;
	enum undo_kind kind;
} undo_ops[] = {
	{ "abort", UNDO_INDEX },	{ "absorb", UNDO_INDEX },
	{ "commit", UNDO_INDEX },	{ "commit --amend", UNDO_INDEX },
	{ "continue", UNDO_INDEX },	{ "defer", UNDO_INDEX },
	{ "discard", UNDO_WORKTREE },	{ "discard-file", UNDO_WORKTREE },
	{ "drop", UNDO_WORKTREE },	{ "edit", UNDO_INDEX },
	{ "fixup", UNDO_INDEX },	{ "fixup-to", UNDO_INDEX },
	{ "move", UNDO_INDEX },		{ "remove", UNDO_WORKTREE },
	{ "reword", UNDO_INDEX },	{ "set-staged", UNDO_INDEX },
	{ "split", UNDO_INDEX },	{ "squash", UNDO_INDEX },
	{ "stage", UNDO_INDEX },	{ "stage-file", UNDO_INDEX },
	{ "stage-move", UNDO_INDEX },	{ "substitute", UNDO_WORKTREE },
	{ "unstage", UNDO_INDEX },	{ "unstage-file", UNDO_INDEX },
	{ "unstage-move", UNDO_INDEX },
};

enum undo_kind undo_kind(const char *op)
{
	size_t i;

	for (i = 0; i < sizeof(undo_ops) / sizeof(undo_ops[0]); i++)
		if (!strcmp(undo_ops[i].op, op))
			return undo_ops[i].kind;
	return UNDO_NONE;
}
