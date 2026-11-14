// SPDX-License-Identifier: MIT
/* Renames, finding them and staging only the move (rename.h). */
#include "rename.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void free_head_blobs(struct repo *r);

size_t parse_renames(const char *buf, size_t len, bool staged,
		     struct rename **out)
{
	const char *p = buf ? buf : "", *end = p + len;
	struct rename *v = NULL;
	size_t n = 0, alloc = 0;

	while (p < end) {
		const char *st = p, *a, *b = NULL;

		p = memchr(p, '\0', (size_t)(end - p));
		if (!p)
			break;
		a = ++p;
		p = memchr(p, '\0', (size_t)(end - p));
		if (!p)
			break;
		p++;
		if (*st == 'R' || *st == 'C') {
			b = p;
			p = memchr(p, '\0', (size_t)(end - p));
			if (!p)
				break;
			p++;
		}
		if (*st != 'R' || !b)
			continue;
		ALLOC_GROW(v, n + 1, alloc);
		v[n].from = xstrdup(a);
		v[n].to = xstrdup(b);
		v[n].staged = staged;
		v[n].score = (int)number_or(st + 1, 0);
		n++;
	}
	*out = v;
	return n;
}

void free_renames(struct rename *v, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		free(v[i].from);
		free(v[i].to);
	}
	free(v);
}

/*
 * A private copy of the index, as an absolute path in $GIT_DIR to keep it
 * on the same file system, or NULL if there is no index yet. The caller
 * unlinks it.
 */
static char *temp_index(struct repo *r)
{
	struct strbuf path = STRBUF_INIT, data = STRBUF_INIT;
	const char *idx = repo_git_path(r, "index");
	/* the pid tells git carve gc whether its owner still runs */
	char suffix[64];
	size_t cut;
	int fd, rc;

	snprintf(suffix, sizeof(suffix), ".carve-%ld-XXXXXX", (long)getpid());
	if (!idx)
		return NULL;
	sb_addf(&path, "%s%s", idx, suffix);
	fd = mkstemp(path.buf);
	if (fd < 0) {
		sb_release(&path);
		return NULL;
	}
	close(fd);
	/* copy the index as it is now, the path without the suffix */
	cut = path.len - strlen(suffix);
	path.buf[cut] = '\0';
	rc = read_file(path.buf, &data);
	path.buf[cut] = '.';
	if (rc || write_file(path.buf, data.buf, data.len)) {
		unlink(path.buf);
		sb_release(&path);
		sb_release(&data);
		return NULL;
	}
	sb_release(&data);
	return sb_detach(&path);
}

static void temp_index_remove(char *path)
{
	struct strbuf lock = STRBUF_INIT;

	if (!path)
		return;
	unlink(path);
	sb_addf(&lock, "%s.lock", path);
	unlink(lock.buf);
	sb_release(&lock);
	free(path);
}

/* git with GIT_INDEX_FILE=index, arguments in a. */
static int git_in_index(struct repo *r, const char *index, struct argv *a,
			const char *in, size_t inlen, struct strbuf *out)
{
	struct strbuf env0 = STRBUF_INIT, e = STRBUF_INIT;
	const char *env[] = { NULL, NULL };
	struct run_opts o = { .env = env, .in = in, .inlen = inlen };
	int rc;

	sb_addf(&env0, "GIT_INDEX_FILE=%s", index);
	env[0] = env0.buf;
	rc = run_git(a, &o, out, &e);
	if (rc)
		git_fail(r, "git", &e);
	sb_release(&env0);
	sb_release(&e);
	return rc ? -1 : 0;
}

static bool in_list(const char *const *v, size_t n, const char *s)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (!strcmp(v[i], s))
			return true;
	return false;
}

/* Renames from a deleted to a new file of fs, still in the work tree. */
static int worktree_renames(struct repo *r, const struct file_status *fs,
			    size_t n, struct rename **out, size_t *nout)
{
	const char **dels = NULL, **news = NULL;
	size_t nd = 0, nn = 0, i, nr = 0, k = 0;
	char *tree = NULL, *tmp = NULL;
	struct strbuf o = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	struct rename *v = NULL;
	int rc = 0;

	dels = xcalloc(n + 1, sizeof(*dels));
	news = xcalloc(n + 1, sizeof(*news));
	for (i = 0; i < n; i++) {
		if (fs[i].unmerged)
			continue;
		if (fs[i].y == 'D')
			dels[nd++] = fs[i].path;
		else if (fs[i].y == '?' || (fs[i].y == 'A' && fs[i].x == ' '))
			news[nn++] = fs[i].path;
	}
	if (!nd || !nn || nd > RENAME_MAX_CANDIDATES ||
	    nn > RENAME_MAX_CANDIDATES)
		goto out;
	/*
	 * The real index as a tree. That fails during conflicts, and then
	 * there are no renames.
	 */
	tree = git_line(r, "write-tree", NULL);
	tmp = tree ? temp_index(r) : NULL;
	if (!tmp)
		goto out;
	argv_pushl(&a, "--literal-pathspecs", "add", "-A", "--", NULL);
	for (i = 0; i < nd; i++)
		argv_push(&a, dels[i]);
	for (i = 0; i < nn; i++)
		argv_push(&a, news[i]);
	if (git_in_index(r, tmp, &a, NULL, 0, NULL)) {
		rc = -1;
		goto out;
	}
	argv_clear(&a);
	argv_pushl(&a, GIT_PLAIN, "diff-index", "--cached", "-M",
		   "--name-status", "-z", "--no-color", tree, "--", NULL);
	if (git_in_index(r, tmp, &a, NULL, 0, &o)) {
		rc = -1;
		goto out;
	}
	nr = parse_renames(o.buf ? o.buf : "", o.len, false, &v);
	/* only pairs of our candidates */
	for (i = 0; i < nr; i++) {
		if (in_list(dels, nd, v[i].from) &&
		    in_list(news, nn, v[i].to)) {
			v[k++] = v[i];
		} else {
			free(v[i].from);
			free(v[i].to);
		}
	}
out:
	*out = v;
	*nout = k;
	argv_clear(&a);
	sb_release(&o);
	temp_index_remove(tmp);
	free(tree);
	free(dels);
	free(news);
	return rc;
}

/*
 * What the renames of fs follow from, the index file, HEAD, every entry
 * that can be half of one, and the stat of the new files in the work tree.
 * NULL when the index has no stamp to trust.
 */
static char *renames_key(struct repo *r, const struct file_status *fs, size_t n)
{
	char *stamp = repo_index_stamp(r);
	struct strbuf k = STRBUF_INIT;
	char *head;
	size_t i;

	if (!stamp)
		return NULL;
	head = repo_rev(r, "HEAD");
	sb_addf(&k, "%s %s\n", stamp, head ? head : "-");
	for (i = 0; i < n; i++) {
		const struct file_status *f = &fs[i];
		struct stat st;

		if (!strchr("AD", f->x) && !strchr("AD?", f->y))
			continue;
		sb_addf(&k, "%c%c%d %s", f->x, f->y, f->unmerged, f->path);
		if (strchr("A?", f->y) && !lstat(f->path, &st))
			sb_addf(&k, " %lld %lld %ld", (long long)st.st_size,
				(long long)st.st_mtime,
				(long)ST_MTIME_NSEC(st));
		sb_add(&k, "\n", 1);
	}
	free(stamp);
	free(head);
	return sb_detach(&k);
}

static struct rename *copy_renames(const struct rename *v, size_t n)
{
	struct rename *c = xcalloc(n + 1, sizeof(*c));
	size_t i;

	for (i = 0; i < n; i++) {
		c[i] = v[i];
		c[i].from = xstrdup(v[i].from);
		c[i].to = xstrdup(v[i].to);
	}
	return c;
}

/* Forget the renames find_renames keeps in r. */
static void free_renames_cache(struct repo *r)
{
	free_renames(r->rn, r->nrn);
	free(r->rn_key);
	r->rn = NULL;
	r->rn_key = NULL;
	r->nrn = 0;
}

static int find_renames_now(struct repo *r, const struct file_status *fs,
			    size_t n, struct rename **out, size_t *nout);

/*
 * The renames, found again only when what they follow from changed. A
 * reload and the untracked scan coming back ask the same question.
 */
int find_renames(struct repo *r, const struct file_status *fs, size_t n,
		 struct rename **out, size_t *nout)
{
	char *key = renames_key(r, fs, n);
	int rc;

	if (key && r->rn_key && !strcmp(key, r->rn_key)) {
		free(key);
		*out = copy_renames(r->rn, r->nrn);
		*nout = r->nrn;
		return 0;
	}
	rc = find_renames_now(r, fs, n, out, nout);
	free_renames_cache(r);
	if (!rc && key) {
		r->rn_key = key;
		repo_on_release(r, free_renames_cache);
		r->rn = copy_renames(*out, *nout);
		r->nrn = *nout;
	} else {
		free(key);
	}
	return rc;
}

static int find_renames_now(struct repo *r, const struct file_status *fs,
			    size_t n, struct rename **out, size_t *nout)
{
	struct rename *v = NULL, *w = NULL;
	bool del = false, add = false;
	struct strbuf o = STRBUF_INIT;
	size_t nv = 0, nw = 0, i;
	int rc = 0;

	*out = NULL;
	*nout = 0;
	for (i = 0; i < n; i++) {
		del |= !fs[i].unmerged && fs[i].x == 'D';
		add |= !fs[i].unmerged && fs[i].x == 'A';
	}
	if (del && add) {
		if (!git_out(r, &o, GIT_PLAIN, "diff-index", "--cached", "-M",
			     "--name-status", "-z", "--no-color", "HEAD", "--",
			     NULL))
			nv = parse_renames(sb_str(&o), o.len, true, &v);
		else
			rc = -1;
	}
	sb_release(&o);
	if (!rc)
		rc = worktree_renames(r, fs, n, &w, &nw);
	if (nw) {
		v = xrealloc(v, (nv + nw) * sizeof(*v));
		memcpy(v + nv, w, nw * sizeof(*w));
		nv += nw;
	}
	free(w);
	*out = v;
	*nout = nv;
	return rc;
}

/* "mode sha" of path's index entry (stage 0). */
static int index_entry(struct repo *r, const char *path, char **mode,
		       char **sha)
{
	char *l = git_line(r, "ls-files", "-s", "--", path, NULL);
	char *sp, *sp2;

	if (!l)
		return repo_err(r, "%s is not in the index", path);
	sp = strchr(l, ' ');
	sp2 = sp ? strchr(sp + 1, ' ') : NULL;
	if (!sp2 || sp2[1] != '0') {
		free(l);
		return repo_err(r, "%s: unexpected index entry", path);
	}
	*mode = xstrndup(l, (size_t)(sp - l));
	*sha = xstrndup(sp + 1, (size_t)(sp2 - sp - 1));
	free(l);
	return 0;
}

/* stage_move in the given index file, or the real one for NULL. */
static int move_in_index(struct repo *r, const char *index, const char *from,
			 const char *to)
{
	struct strbuf info = STRBUF_INIT;
	char *mode = NULL, *sha = NULL;
	struct argv a = ARGV_INIT;
	int rc;

	if (index_entry(r, from, &mode, &sha))
		return -1;
	sb_addf(&info, "%s,%s,%s", mode, sha, to);
	argv_pushl(&a, "update-index", "--add", "--cacheinfo", info.buf,
		   "--force-remove", "--", from, NULL);
	if (index)
		rc = git_in_index(r, index, &a, NULL, 0, NULL);
	else if (run_git(&a, NULL, NULL, NULL))
		rc = repo_err(r, "cannot stage the move of %s", from);
	else
		rc = 0;
	argv_clear(&a);
	sb_release(&info);
	free(mode);
	free(sha);
	return rc;
}

int stage_move(struct repo *r, const char *from, const char *to)
{
	return move_in_index(r, NULL, from, to);
}

int unstage_move(struct repo *r, const char *from, const char *to)
{
	return git_quiet(r, "--literal-pathspecs", "reset", "-q", "--", from,
			 to, NULL) ?
		       -1 :
		       0;
}

int rename_diff(struct repo *r, const char *from, const char *to,
		const struct diff_opts *opts, struct fdiff **out)
{
	char *tmp = temp_index(r);
	int rc;

	*out = NULL;
	if (!tmp)
		return repo_err(r, "cannot make a temporary index");
	rc = move_in_index(r, tmp, from, to);
	if (!rc)
		rc = load_diff_index(r, tmp, to, false, opts, out);
	temp_index_remove(tmp);
	return rc;
}

static struct file_status *find_path(struct file_status *fs, size_t n,
				     const char *path)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (!strcmp(fs[i].path, path))
			return &fs[i];
	return NULL;
}

size_t apply_renames(struct file_status *fs, size_t n, const struct rename *v,
		     size_t nv)
{
	size_t i, k = 0;

	for (i = 0; i < nv; i++) {
		struct file_status *from = find_path(fs, n, v[i].from);
		struct file_status *to = find_path(fs, n, v[i].to);

		if (!from || !to || from == to || from->unmerged ||
		    to->unmerged)
			continue;
		if (v[i].staged) {
			to->x = 'R';
			free(to->from_x);
			to->from_x = xstrdup(v[i].from);
			from->x = ' ';
		} else {
			to->y = 'R';
			free(to->from_y);
			to->from_y = xstrdup(v[i].from);
			from->y = ' ';
		}
	}
	/* a source with nothing else to show goes */
	for (i = 0; i < n; i++) {
		struct file_status *f = &fs[i];

		if (f->x == ' ' && f->y == ' ' && !f->unmerged) {
			free(f->path);
			free(f->from_x);
			free(f->from_y);
			continue;
		}
		if (k != i)
			fs[k] = *f;
		k++;
	}
	return k;
}

int staged_rename_diff(struct repo *r, const char *from, const char *to,
		       const struct diff_opts *opts, struct fdiff **out)
{
	struct strbuf o = STRBUF_INIT, e = STRBUF_INIT, perr = STRBUF_INIT,
		      src = STRBUF_INIT, dst = STRBUF_INIT;
	struct run_opts ro = { 0 };
	struct argv a = ARGV_INIT;
	struct fdiff *fds = NULL;
	size_t n = 0;
	int rc;

	*out = NULL;
	push_diff_args(&a);
	push_diff_opts(&a, opts);
	/* blob to blob, a plain diff of the new path against the old one */
	sb_addf(&src, "HEAD:%s", from);
	sb_addf(&dst, ":%s", to);
	argv_pushl(&a, src.buf, dst.buf, NULL);
	diff_run_opts(opts, &ro);
	rc = run_git(&a, &ro, &o, &e);
	if (diff_stopped(opts))
		rc = 0;
	else if (rc)
		git_fail(r, "git diff", &e);
	else if (diff_parse(sb_str(&o), o.len, &fds, &n, &perr))
		rc = repo_err(r, "%s", perr.buf);
	if (!rc && n == 1) {
		*out = xmalloc(sizeof(**out));
		**out = fds[0];
		(*out)->zero_context = opts && !opts->context;
		(*out)->view_only = opts && opts->ignore_ws;
		free(fds);
		fds = NULL;
		n = 0;
	}
	fdiff_free_array(fds, n);
	argv_clear(&a);
	sb_release(&o);
	sb_release(&e);
	sb_release(&perr);
	sb_release(&src);
	sb_release(&dst);
	return rc ? -1 : 0;
}

char *staged_rename_source(struct repo *r, const char *path)
{
	struct strbuf spec = STRBUF_INIT, o = STRBUF_INIT;
	struct rename *v = NULL;
	char *src = NULL;
	size_t n = 0, i;

	/* only a path new since HEAD can be a rename target */
	sb_addf(&spec, "HEAD:%s", path);
	if (!repo_rev_status(r, spec.buf) || repo_rev_status(r, "HEAD"))
		goto out;
	if (git_out(r, &o, GIT_PLAIN, "diff-index", "--cached", "-M",
		    "--name-status", "-z", "--no-color", "HEAD", "--", NULL))
		goto out;
	n = parse_renames(sb_str(&o), o.len, true, &v);
	for (i = 0; i < n && !src; i++)
		if (!strcmp(v[i].to, path))
			src = xstrdup(v[i].from);
out:
	free_renames(v, n);
	sb_release(&spec);
	sb_release(&o);
	return src;
}

char *worktree_rename_source(struct repo *r, const char *path)
{
	size_t n = 0, alloc = 0, nv = 0, i;
	struct strbuf o = STRBUF_INIT;
	struct file_status *fs = NULL;
	struct rename *v = NULL;
	const char *p, *end;
	char *src = NULL;

	if (path_in_index(r, path) != 0 ||
	    git_out(r, &o, GIT_PLAIN, "ls-files", "-d", "-z", NULL) || !o.len)
		goto out;
	for (p = sb_str(&o), end = p + o.len; p < end; p += strlen(p) + 1) {
		ALLOC_GROW(fs, n + 1, alloc);
		fs[n++] = (struct file_status){ xstrdup(p), ' ',   'D',
						false,	    NULL,  NULL,
						false,	    false, false };
	}
	ALLOC_GROW(fs, n + 1, alloc);
	fs[n++] = (struct file_status){ xstrdup(path), ' ',   '?',
					false,	       NULL,  NULL,
					false,	       false, false };
	if (worktree_renames(r, fs, n, &v, &nv))
		goto out;
	for (i = 0; i < nv && !src; i++)
		if (!strcmp(v[i].to, path))
			src = xstrdup(v[i].from);
out:
	free_renames(v, nv);
	free_status(fs, n);
	sb_release(&o);
	return src;
}

/* The new side's blob id in the diff's "index <old>..<new>" line. */
static char *new_blob_of(const struct fdiff *fd)
{
	size_t i;

	for (i = 0; fd && i < fd->nhdr; i++) {
		const char *h = fd->hdr[i], *dots;

		if (strncmp(h, "index ", 6) || !(dots = strstr(h, "..")))
			continue;
		dots += 2;
		return xstrndup(dots, strcspn(dots, " \n"));
	}
	return NULL;
}

static void free_hb(struct repo *r);

static int blob_cmp(const void *a, const void *b)
{
	return strcmp(((const struct head_blob *)a)->oid,
		      ((const struct head_blob *)b)->oid);
}

/*
 * HEAD's files sorted by blob id, kept in struct repo while HEAD's tree
 * stays the same. That is one git ls-tree -r per HEAD, not one per new
 * file shown.
 */
static int head_blobs(struct repo *r)
{
	char *tree = repo_rev(r, "HEAD^{tree}");
	struct strbuf o = STRBUF_INIT;
	const char *pos, *end, *l;
	size_t len, alloc = 0;

	if (!tree)
		return -1;
	if (r->hb_tree && !strcmp(r->hb_tree, tree)) {
		free(tree);
		return 0;
	}
	free_hb(r);
	if (git_out(r, &o, GIT_PLAIN, "ls-tree", "-r", "-z", tree, NULL)) {
		free(tree);
		sb_release(&o);
		return -1;
	}
	/* "<mode> blob <id>\t<path>\0" */
	for (pos = sb_str(&o), end = pos + o.len; pos < end; pos += len + 1) {
		const char *tab, *sp;

		l = pos;
		len = strlen(l);
		tab = memchr(l, '\t', len);
		if (!tab ||
		    strncmp(strchr(l, ' ') ? strchr(l, ' ') : "", " blob ", 6))
			continue;
		sp = strchr(l, ' ') + 6;
		ALLOC_GROW(r->hb, r->nhb + 1, alloc);
		r->hb[r->nhb].oid = xstrndup(sp, (size_t)(tab - sp));
		r->hb[r->nhb++].path = xstrdup(tab + 1);
	}
	qsort(r->hb, r->nhb, sizeof(*r->hb), blob_cmp);
	r->hb_tree = tree;
	repo_on_release(r, free_head_blobs);
	sb_release(&o);
	return 0;
}

/* Forget the list of HEAD's blobs copy_source keeps in r. */
static void free_head_blobs(struct repo *r)
{
	size_t i;

	for (i = 0; i < r->nnc; i++)
		free(r->nc[i]);
	free(r->nc);
	free(r->nc_tree);
	r->nc = NULL;
	r->nc_tree = NULL;
	r->nnc = 0;
	free_hb(r);
}

static void free_hb(struct repo *r)
{
	size_t i;

	for (i = 0; i < r->nhb; i++) {
		free(r->hb[i].oid);
		free(r->hb[i].path);
	}
	free(r->hb);
	free(r->hb_tree);
	r->hb = NULL;
	r->hb_tree = NULL;
	r->nhb = 0;
}

/* Ids as the diffs print them, abbreviated and maybe to other lengths. */
static bool same_id(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);

	return !strncmp(a, b, la < lb ? la : lb);
}

/*
 * sha is no copy of a file in HEAD's tree. That is known from before git
 * had the blob (an untracked file shown, then staged), or remembered now
 * (add).
 */
static bool known_no_copy(struct repo *r, const char *sha, bool add)
{
	char *tree = repo_rev(r, "HEAD^{tree}");
	size_t i;

	if (!tree)
		return false;
	if (!r->nc_tree || strcmp(r->nc_tree, tree) || r->nnc >= 256) {
		for (i = 0; i < r->nnc; i++)
			free(r->nc[i]);
		r->nnc = 0;
		free(r->nc_tree);
		r->nc_tree = tree;
		repo_on_release(r, free_head_blobs);
	} else {
		free(tree);
	}
	for (i = 0; i < r->nnc; i++)
		if (same_id(r->nc[i], sha))
			return true;
	if (add) {
		r->nc = xrealloc(r->nc, (r->nnc + 1) * sizeof(*r->nc));
		r->nc[r->nnc++] = xstrdup(sha);
	}
	return false;
}

char *copy_source(struct repo *r, const char *path, bool cached,
		  const struct fdiff *fd)
{
	struct strbuf spec = STRBUF_INIT;
	char *sha = new_blob_of(fd);
	size_t lo = 0, hi, k, sl;
	char *src = NULL;

	/* the diff gives the blob, abbreviated, else ask git */
	if (!sha || strspn(sha, "0") == strlen(sha)) {
		free(sha);
		if (cached) {
			sb_addf(&spec, ":%s", path);
			sha = repo_rev(r, spec.buf);
		} else {
			sha = git_line(r, "hash-object", "--", path, NULL);
		}
	}
	/*
	 * A blob git does not have, usually an untracked file's, is in no
	 * tree. HEAD's need not be listed, not even once git add has
	 * written it.
	 */
	if (!sha || known_no_copy(r, sha, false))
		goto out;
	if (repo_rev_status(r, sha)) {
		known_no_copy(r, sha, true);
		goto out;
	}
	if (head_blobs(r))
		goto out;
	/* the first id starting with sha, then all that do */
	sl = strlen(sha);
	hi = r->nhb;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (strncmp(r->hb[mid].oid, sha, sl) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (k = lo; k < r->nhb && !strncmp(r->hb[k].oid, sha, sl); k++) {
		/* an abbreviation another blob shares is not sure */
		if (strcmp(r->hb[k].oid, r->hb[lo].oid)) {
			free(src);
			src = NULL;
			break;
		}
		if (!src && strcmp(r->hb[k].path, path))
			src = xstrdup(r->hb[k].path);
	}
out:
	free(sha);
	sb_release(&spec);
	return src;
}
