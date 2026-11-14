// SPDX-License-Identifier: MIT
#include "diff.h"

#include <fnmatch.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char *unquote_path(const char *s, size_t len)
{
	struct strbuf sb = STRBUF_INIT;
	size_t i;

	if (len < 2 || s[0] != '"' || s[len - 1] != '"')
		return xstrndup(s, len);
	for (i = 1; i + 1 < len; i++) {
		char c = s[i];

		if (c != '\\' || i + 2 >= len) {
			sb_addch(&sb, c);
			continue;
		}
		c = s[++i];
		switch (c) {
		case 'a':
			sb_addch(&sb, '\a');
			break;
		case 'b':
			sb_addch(&sb, '\b');
			break;
		case 't':
			sb_addch(&sb, '\t');
			break;
		case 'n':
			sb_addch(&sb, '\n');
			break;
		case 'v':
			sb_addch(&sb, '\v');
			break;
		case 'f':
			sb_addch(&sb, '\f');
			break;
		case 'r':
			sb_addch(&sb, '\r');
			break;
		case '0':
		case '1':
		case '2':
		case '3':
			if (i + 2 < len - 1 + 1 && s[i + 1] >= '0' &&
			    s[i + 1] <= '7' && s[i + 2] >= '0' &&
			    s[i + 2] <= '7') {
				sb_addch(&sb, (char)(((c - '0') << 6) |
						     ((s[i + 1] - '0') << 3) |
						     (s[i + 2] - '0')));
				i += 2;
				break;
			}
			sb_addch(&sb, c);
			break;
		default:
			sb_addch(&sb, c);
			break;
		}
	}
	return sb_detach(&sb);
}

static bool needs_quote(const char *path)
{
	const unsigned char *p;

	for (p = (const unsigned char *)path; *p; p++)
		if (*p < 0x20 || *p == 0x7f || *p == '"' || *p == '\\')
			return true;
	return false;
}

void quote_path(struct strbuf *sb, const char *path)
{
	const unsigned char *p;

	if (!needs_quote(path)) {
		sb_adds(sb, path);
		return;
	}
	sb_addch(sb, '"');
	for (p = (const unsigned char *)path; *p; p++) {
		switch (*p) {
		case '"':
			sb_adds(sb, "\\\"");
			break;
		case '\\':
			sb_adds(sb, "\\\\");
			break;
		case '\a':
			sb_adds(sb, "\\a");
			break;
		case '\b':
			sb_adds(sb, "\\b");
			break;
		case '\t':
			sb_adds(sb, "\\t");
			break;
		case '\n':
			sb_adds(sb, "\\n");
			break;
		case '\v':
			sb_adds(sb, "\\v");
			break;
		case '\f':
			sb_adds(sb, "\\f");
			break;
		case '\r':
			sb_adds(sb, "\\r");
			break;
		default:
			if (*p < 0x20 || *p == 0x7f)
				sb_addf(sb, "\\%03o", *p);
			else
				sb_addch(sb, (char)*p);
		}
	}
	sb_addch(sb, '"');
}

/*
 * Parse the name in a "--- a/foo" / "+++ b/foo" line. Returns NULL for
 * /dev/null. Git appends a TAB when the name contains a space.
 */
static char *parse_name(const char *s, size_t len)
{
	char *name, *res;

	if (len && s[len - 1] == '\t')
		len--;
	if (len == 9 && !memcmp(s, "/dev/null", 9))
		return NULL;
	name = unquote_path(s, len);
	if ((name[0] == 'a' || name[0] == 'b') && name[1] == '/') {
		res = xstrdup(name + 2);
		free(name);
		return res;
	}
	return name;
}

/* Names from "diff --git a/X b/Y" when both are equal (no rename). */
static void parse_git_line(struct fdiff *fd, const char *s, size_t len)
{
	size_t n;

	if (len && s[0] == '"') {
		/* "a/..." "b/..." or mixed. Find the end of the first. */
		size_t i = 1;

		while (i < len && s[i] != '"') {
			if (s[i] == '\\')
				i++;
			i++;
		}
		if (i >= len)
			return;
		free(fd->old_path);
		fd->old_path = parse_name(s, i + 1);
		if (i + 2 < len) {
			free(fd->new_path);
			fd->new_path = parse_name(s + i + 2, len - i - 2);
		}
		return;
	}
	if (len < 5 || (len - 5) % 2)
		return;
	n = (len - 5) / 2;
	if (memcmp(s, "a/", 2) || memcmp(s + 2 + n, " b/", 3) ||
	    memcmp(s + 2, s + 5 + n, n))
		return;
	free(fd->old_path);
	free(fd->new_path);
	fd->old_path = xstrndup(s + 2, n);
	fd->new_path = xstrndup(s + 2, n);
}

static int parse_range(const char **p, int *start, int *count)
{
	if (parse_int(*p, p, start))
		return -1;
	*count = 1;
	if (**p == ',') {
		(*p)++;
		if (parse_int(*p, p, count))
			return -1;
	}
	return 0;
}

static int parse_hunk_header(const char *s, size_t len, struct hunk *h)
{
	char *line = xstrndup(s, len);
	const char *p = line + 3, *end;
	int rc = -1;

	/* "@@ -a[,b] +c[,d] @@[ section]" */
	if (p[0] != '-')
		goto out;
	p++;
	if (parse_range(&p, &h->old_start, &h->old_count))
		goto out;
	if (p[0] != ' ' || p[1] != '+')
		goto out;
	p += 2;
	if (parse_range(&p, &h->new_start, &h->new_count))
		goto out;
	if (strncmp(p, " @@", 3))
		goto out;
	p += 3;
	if (*p == ' ')
		p++;
	end = line + len;
	h->section = xstrndup(p, (size_t)(end - p));
	rc = 0;
out:
	free(line);
	return rc;
}

static void hdr_push(struct fdiff *fd, size_t *alloc, const char *s, size_t len)
{
	ALLOC_GROW(fd->hdr, fd->nhdr + 1, *alloc);
	fd->hdr[fd->nhdr++] = xstrndup(s, len);
}

void fdiff_free(struct fdiff *fd)
{
	size_t i;

	if (!fd)
		return;
	for (i = 0; i < fd->nhdr; i++)
		free(fd->hdr[i]);
	free(fd->hdr);
	for (i = 0; i < fd->nhunks; i++) {
		free(fd->hunks[i].section);
		free(fd->hunks[i].lines);
	}
	free(fd->hunks);
	free(fd->old_path);
	free(fd->new_path);
	free(fd->raw);
	memset(fd, 0, sizeof(*fd));
}

void fdiff_dup(const struct fdiff *src, struct fdiff *dst)
{
	size_t i, j, total = 0;
	char *p;

	*dst = *src;
	dst->hdr = xcalloc(src->nhdr + 1, sizeof(*dst->hdr));
	for (i = 0; i < src->nhdr; i++)
		dst->hdr[i] = xstrdup(src->hdr[i]);
	dst->old_path = src->old_path ? xstrdup(src->old_path) : NULL;
	dst->new_path = src->new_path ? xstrdup(src->new_path) : NULL;
	/* the lines' text in a buffer of its own (src's may be shared) */
	for (i = 0; i < src->nhunks; i++)
		for (j = 0; j < src->hunks[i].nlines; j++)
			total += src->hunks[i].lines[j].len + 1;
	dst->raw = p = xmalloc(total + 1);
	dst->hunks = xcalloc(src->nhunks + 1, sizeof(*dst->hunks));
	for (i = 0; i < src->nhunks; i++) {
		const struct hunk *h = &src->hunks[i];
		struct hunk *d = &dst->hunks[i];

		*d = *h;
		d->section = xstrdup(h->section ? h->section : "");
		d->lines = xcalloc(h->nlines + 1, sizeof(*d->lines));
		for (j = 0; j < h->nlines; j++) {
			d->lines[j] = h->lines[j];
			memcpy(p, h->lines[j].text, h->lines[j].len);
			d->lines[j].text = p;
			p += h->lines[j].len;
			*p++ = '\0';
		}
	}
	*p = '\0';
}

void fdiff_free_array(struct fdiff *fd, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		fdiff_free(&fd[i]);
	free(fd);
}

static int perr(struct strbuf *err, int lineno, const char *msg)
{
	if (err)
		sb_addf(err, "diff parse error at line %d: %s", lineno, msg);
	return -1;
}

int diff_parse(const char *buf, size_t len, struct fdiff **out, size_t *nout,
	       struct strbuf *err)
{
	int old_left = 0, new_left = 0, old_no = 0, new_no = 0, lineno = 0;
	size_t nfiles = 0, afiles = 0, ahdr = 0, alines = 0, ahunks = 0;
	struct fdiff *files = NULL, *fd = NULL;
	const char *pos, *end, *l;
	bool in_binary = false;
	struct hunk *h = NULL;
	const char *msg;
	char *raw;
	size_t ll;

	/*
	 * Every file shares one copy of the input. Only the first file
	 * owns it (fdiff.raw), and later ones point into it through
	 * dline.text.
	 */
	raw = xmalloc(len + 1);
	if (len)
		memcpy(raw, buf, len);
	raw[len] = '\0';
	pos = raw;
	end = raw + len;

	while ((l = next_line(&pos, end, &ll))) {
		lineno++;
		if (mem_starts_with(l, ll, "diff --git ") ||
		    mem_starts_with(l, ll, "diff --cc ") ||
		    mem_starts_with(l, ll, "diff --combined ")) {
			if (h && (old_left || new_left))
				goto truncated;
			ALLOC_GROW(files, nfiles + 1, afiles);
			fd = &files[nfiles++];
			memset(fd, 0, sizeof(*fd));
			ahdr = ahunks = 0;
			h = NULL;
			in_binary = false;
			hdr_push(fd, &ahdr, l, ll);
			if (!mem_starts_with(l, ll, "diff --git "))
				fd->combined = true;
			else
				parse_git_line(fd, l + 11, ll - 11);
			continue;
		}
		if (!fd)
			continue; /* junk before the first diff */
		if (in_binary)
			continue;
		if (h && (old_left || new_left)) {
			char op = ll ? l[0] : ' ';
			struct dline *d;

			if (ll && l[0] == '\\') {
				if (!h->nlines) {
					msg = "stray '\\'";
					goto fail;
				}
				h->lines[h->nlines - 1].no_eol = true;
				continue;
			}
			if (op != ' ' && op != '+' && op != '-') {
				msg = "bad hunk line";
				goto fail;
			}
			ALLOC_GROW(h->lines, h->nlines + 1, alines);
			d = &h->lines[h->nlines++];
			memset(d, 0, sizeof(*d));
			d->op = op;
			d->text = ll ? l + 1 : l;
			d->len = ll ? ll - 1 : 0;
			if (op != '+') {
				if (!old_left)
					goto overflow;
				old_left--;
				d->old_no = old_no++;
			}
			if (op != '-') {
				if (!new_left)
					goto overflow;
				new_left--;
				d->new_no = new_no++;
			}
			continue;
		}
		if (h && ll && l[0] == '\\') {
			/* "\ No newline" after the last line of a hunk */
			if (!h->nlines) {
				msg = "stray '\\'";
				goto fail;
			}
			h->lines[h->nlines - 1].no_eol = true;
			continue;
		}
		if (fd->combined && mem_starts_with(l, ll, "@@@")) {
			/*
			 * skip to the next file, this one cannot be edited
			 * by line
			 */
			in_binary = true;
			continue;
		}
		if (mem_starts_with(l, ll, "@@ ")) {
			if (fd->combined)
				continue;
			ALLOC_GROW(fd->hunks, fd->nhunks + 1, ahunks);
			h = &fd->hunks[fd->nhunks++];
			memset(h, 0, sizeof(*h));
			alines = 0;
			if (parse_hunk_header(l, ll, h)) {
				msg = "bad hunk header";
				goto fail;
			}
			old_left = h->old_count;
			new_left = h->new_count;
			old_no = h->old_count ? h->old_start : h->old_start + 1;
			new_no = h->new_count ? h->new_start : h->new_start + 1;
			continue;
		}
		if (h) {
			if (fd->combined)
				continue;
			msg = "unexpected line after hunk";
			goto fail;
		}
		/* extended header */
		hdr_push(fd, &ahdr, l, ll);
		if (mem_starts_with(l, ll, "new file mode ")) {
			fd->is_new = true;
		} else if (mem_starts_with(l, ll, "deleted file mode ")) {
			fd->is_deleted = true;
		} else if (mem_starts_with(l, ll, "old mode ") ||
			   mem_starts_with(l, ll, "new mode ")) {
			fd->mode_change = true;
		} else if (mem_starts_with(l, ll, "rename from ")) {
			fd->is_rename = true;
			free(fd->old_path);
			fd->old_path = unquote_path(l + 12, ll - 12);
		} else if (mem_starts_with(l, ll, "rename to ")) {
			fd->is_rename = true;
			free(fd->new_path);
			fd->new_path = unquote_path(l + 10, ll - 10);
		} else if (mem_starts_with(l, ll, "--- ")) {
			free(fd->old_path);
			fd->old_path = parse_name(l + 4, ll - 4);
		} else if (mem_starts_with(l, ll, "+++ ")) {
			free(fd->new_path);
			fd->new_path = parse_name(l + 4, ll - 4);
		} else if (mem_starts_with(l, ll, "Binary files ")) {
			fd->binary = true;
		} else if (mem_starts_with(l, ll, "GIT binary patch")) {
			fd->binary = true;
			in_binary = true;
		}
	}
	if (h && (old_left || new_left))
		goto truncated;

	for (size_t i = 0; i < nfiles; i++) {
		fd = &files[i];
		if (fd->is_new && fd->old_path && !fd->is_rename) {
			free(fd->old_path);
			fd->old_path = NULL;
		}
		if (fd->is_deleted && fd->new_path) {
			free(fd->new_path);
			fd->new_path = NULL;
		}
		fd->mode_only = fd->mode_change && !fd->nhunks && !fd->binary &&
				!fd->is_rename;
	}
	if (nfiles)
		files[0].raw = raw;
	else
		free(raw);
	*out = files;
	*nout = nfiles;
	return 0;

overflow:
	msg = "hunk longer than its header says";
	goto fail;
truncated:
	msg = "hunk shorter than its header says";
fail:
	fdiff_free_array(files, nfiles);
	free(raw);
	return perr(err, lineno, msg);
}

bool fdiff_line_ops(const struct fdiff *fd, const char **why)
{
	const char *w = NULL;

	if (fd->combined)
		w = "unmerged path; resolve the conflict first";
	else if (fd->view_only)
		w = "whitespace is ignored in this view: line operations are "
		    "off";
	else if (fd->binary)
		w = "binary file: whole-file operations only";
	else if (fd->is_rename)
		w = "renamed file: whole-file operations only";
	else if (fd->is_deleted)
		w = "deleted file: whole-file operations only";
	else if (fd->mode_only)
		w = "mode change only: whole-file operations only";
	else if (!fd->nhunks)
		w = "no content changes: whole-file operations only";
	if (why)
		*why = w;
	return !w;
}

int path_in_index(struct repo *r, const char *path)
{
	return index_entry_of(r, path, NULL);
}

bool path_in_worktree(const char *path)
{
	struct stat st;

	return lstat(path, &st) == 0;
}

long worktree_size(const char *path)
{
	struct stat st;

	return lstat(path, &st) == 0 ? (long)st.st_size : -1;
}

bool path_matches(const char *path, char *const *pats, size_t n)
{
	const char *base = strrchr(path, '/');
	size_t i;

	base = base ? base + 1 : path;
	for (i = 0; i < n; i++) {
		const char *p = pats[i];

		if (!*p)
			continue;
		/*
		 * with a slash the pattern is the whole path from the top,
		 * else the name
		 */
		if (strchr(p, '/')) {
			if (*p == '/')
				p++;
			if (!fnmatch(p, path, FNM_PATHNAME))
				return true;
		} else if (!fnmatch(p, base, 0)) {
			return true;
		}
	}
	return false;
}

int load_diff(struct repo *r, const char *path, bool cached, struct fdiff **out)
{
	return load_diff_opts(r, path, cached, NULL, out);
}

void diff_run_opts(const struct diff_opts *opts, struct run_opts *ro)
{
	if (!opts)
		return;
	ro->lim = opts->lim;
}

void push_diff_excludes(struct argv *a, const struct diff_opts *opts)
{
	struct strbuf sb = STRBUF_INIT;
	size_t i;

	if (!opts || (!opts->nexclude && !opts->nonly))
		return;
	argv_push(a, "--");
	for (i = 0; i < opts->nonly; i++) {
		sb_reset(&sb);
		sb_addf(&sb, ":(literal)%s", opts->only[i]);
		argv_push(a, sb.buf);
	}
	for (i = 0; i < opts->nexclude; i++) {
		const char *p = opts->exclude[i];

		if (!*p)
			continue;
		sb_reset(&sb);
		/* a name anywhere, or a path from the top */
		if (strchr(p, '/'))
			sb_addf(&sb, ":(exclude,glob)%s",
				*p == '/' ? p + 1 : p);
		else
			sb_addf(&sb, ":(exclude,glob)**/%s", p);
		argv_push(a, sb.buf);
	}
	sb_release(&sb);
}

bool diff_stopped(const struct diff_opts *opts)
{
	return opts && opts->lim.stopped && *opts->lim.stopped != RUN_DONE;
}

void push_diff_opts(struct argv *a, const struct diff_opts *opts)
{
	char u[24];

	if (!opts)
		return;
	/* after the -U3 of push_diff_args, where the last one wins */
	if (opts->context != 3) {
		snprintf(u, sizeof(u), "-U%d", opts->context);
		argv_push(a, u);
	}
	if (opts->function_context)
		argv_push(a, "-W");
	if (opts->ignore_ws)
		argv_push(a, "-w");
}

int load_diff_opts(struct repo *r, const char *path, bool cached,
		   const struct diff_opts *opts, struct fdiff **out)
{
	return load_diff_index(r, NULL, path, cached, opts, out);
}

/* One git diff of path into *out, NULL for no changes, as load_diff_index. */
static int run_diff(struct repo *r, const char *index_file, const char *path,
		    bool cached, bool no_index, const struct diff_opts *opts,
		    struct fdiff **out)
{
	struct strbuf o = STRBUF_INIT, e = STRBUF_INIT, perrb = STRBUF_INIT;
	const char *env[2] = { NULL, NULL };
	struct run_opts ro = { .env = env };
	struct strbuf env0 = STRBUF_INIT;
	struct fdiff *files = NULL;
	struct argv a = ARGV_INIT;
	size_t n = 0, i;
	int rc;

	/*
	 * No optional lock. A diff shown must not rewrite the index with
	 * the stat data git refreshed, or everything keyed by the index
	 * file (the diff cache, undo's states) would look changed.
	 */
	argv_pushl(&a, "--no-optional-locks", "--literal-pathspecs", NULL);
	push_diff_args(&a);
	push_diff_opts(&a, opts);
	if (cached)
		argv_push(&a, "--cached");
	if (no_index)
		argv_pushl(&a, "--no-index", "--", "/dev/null", path, NULL);
	else
		argv_pushl(&a, "--", path, NULL);
	diff_run_opts(opts, &ro);
	if (index_file) {
		sb_addf(&env0, "GIT_INDEX_FILE=%s", index_file);
		env[0] = env0.buf;
	}
	rc = run_git(&a, &ro, &o, &e);
	argv_clear(&a);
	if (diff_stopped(opts)) {
		rc = 0;
		goto out;
	}
	if (rc != 0 && !(no_index && rc == 1)) {
		git_fail(r, "git diff", &e);
		goto out;
	}
	rc = diff_parse(o.buf, o.len, &files, &n, &perrb);
	if (rc) {
		repo_err(r, "%s", perrb.buf);
		goto out;
	}
	if (n > 1) {
		rc = repo_err(r, "%s: unexpected multi-file diff", path);
		goto out;
	}
	if (n == 1) {
		*out = xmalloc(sizeof(**out));
		**out = files[0];
		/* -W can leave hunks without context too */
		(*out)->zero_context = opts && !opts->context;
		(*out)->view_only = opts && opts->ignore_ws;
		free(files);
		files = NULL;
		n = 0;
	}
	rc = 0;
out:
	for (i = 0; i < n; i++)
		fdiff_free(&files[i]);
	free(files);
	sb_release(&o);
	sb_release(&e);
	sb_release(&perrb);
	sb_release(&env0);
	return rc ? -1 : 0;
}

int load_diff_index(struct repo *r, const char *index_file, const char *path,
		    bool cached, const struct diff_opts *opts,
		    struct fdiff **out)
{
	int in;

	*out = NULL;
	if (opts && opts->untracked && !cached && !index_file &&
	    path_in_worktree(path))
		return run_diff(r, index_file, path, false, true, opts, out);
	/*
	 * The diff first, since a changed tracked file, the common case,
	 * needs no more. No diff means an untracked file (diffed with
	 * --no-index), a staged deletion, or no changes, and the index
	 * tells them apart.
	 */
	if (run_diff(r, index_file, path, cached, false, opts, out))
		return -1;
	if (*out || cached || index_file || (opts && opts->in_index) ||
	    diff_stopped(opts))
		return 0;
	in = path_in_index(r, path);
	if (in < 0)
		return -1;
	if (in)
		return 0;
	if (!path_in_worktree(path)) {
		struct strbuf spec = STRBUF_INIT;
		bool staged_rm;

		/*
		 * after git rm the deletion is staged and nothing is
		 * unstaged
		 */
		sb_addf(&spec, "HEAD:%s", path);
		staged_rm = !repo_rev_status(r, spec.buf);
		sb_release(&spec);
		return staged_rm ? 0 : repo_err(r, "%s: no such path", path);
	}
	return run_diff(r, index_file, path, false, true, opts, out);
}

static void put_no(struct strbuf *sb, int no)
{
	if (no)
		sb_addf(sb, "%5d ", no);
	else
		sb_adds(sb, "      ");
}

void diff_annotate(const struct fdiff *fd, struct strbuf *out)
{
	const char *why;
	size_t i, j;

	for (i = 0; i < fd->nhdr; i++) {
		sb_adds(out, fd->hdr[i]);
		sb_addch(out, '\n');
	}
	if (!fdiff_line_ops(fd, &why))
		sb_addf(out, "# %s\n", why);
	if (fd->nhunks)
		sb_adds(out, "  old   new\n");
	for (i = 0; i < fd->nhunks; i++) {
		const struct hunk *h = &fd->hunks[i];

		sb_addf(out, "@@ -%d,%d +%d,%d @@%s%s\n", h->old_start,
			h->old_count, h->new_start, h->new_count,
			*h->section ? " " : "", h->section);
		for (j = 0; j < h->nlines; j++) {
			const struct dline *d = &h->lines[j];

			put_no(out, d->old_no);
			put_no(out, d->new_no);
			sb_addch(out, d->op);
			sb_add(out, d->text, d->len);
			sb_addch(out, '\n');
			if (d->no_eol)
				sb_adds(out, "            " NO_EOL_MARKER);
		}
	}
}

void sel_clear(struct fdiff *fd)
{
	size_t i, j;

	for (i = 0; i < fd->nhunks; i++)
		for (j = 0; j < fd->hunks[i].nlines; j++)
			fd->hunks[i].lines[j].sel = false;
}

size_t sel_count(const struct fdiff *fd)
{
	size_t i, j, n = 0;

	for (i = 0; i < fd->nhunks; i++)
		for (j = 0; j < fd->hunks[i].nlines; j++)
			if (fd->hunks[i].lines[j].sel &&
			    fd->hunks[i].lines[j].op != ' ')
				n++;
	return n;
}

/*
 * Position of a row on one side. Rows present on that side use their line
 * number. For ranges, a removed row sits at the new-side number of the
 * line that follows it, and an added row at the old-side number of the
 * line before it. Git lists a change as "-old... +new...", and this keeps
 * both halves of a change inside a range naming either half.
 */
static int sel_item(struct fdiff *fd, bool old, int a, int b, bool range,
		    bool ctx, struct strbuf *err)
{
	struct dline *exact = NULL;
	bool found = false;
	size_t i, j;
	int n = 0;

	for (i = 0; i < fd->nhunks; i++) {
		struct hunk *h = &fd->hunks[i];
		int next_new;
		int prev_old;

		next_new = h->new_count ? h->new_start : h->new_start + 1;
		prev_old = h->old_count ? h->old_start - 1 : h->old_start;

		for (j = 0; j < h->nlines; j++) {
			struct dline *d = &h->lines[j];
			int no = old ? d->old_no : d->new_no, pos = no;

			if (!no)
				pos = old ? prev_old : next_new;
			if (d->op != '-')
				next_new = d->new_no + 1;
			if (d->op != '+')
				prev_old = d->old_no;

			if (!range) {
				/* a single number names exactly one line */
				if (no == a)
					exact = d;
				continue;
			}
			if (pos < a || pos > b)
				continue;
			if (no)
				found = true;
			if (d->op == ' ' && !ctx)
				continue;
			if (!d->sel && d->op != ' ')
				n++;
			d->sel = true;
		}
	}
	if (!range) {
		if (!exact) {
			sb_addf(err, "%c%d is not in the diff", old ? 'o' : 'n',
				a);
			return -1;
		}
		if (exact->op == ' ' && !ctx) {
			sb_addf(err, "%c%d is an unchanged context line",
				old ? 'o' : 'n', a);
			return -1;
		}
		if (!exact->sel && exact->op != ' ')
			n++;
		exact->sel = true;
		return n;
	}
	if (!found) {
		sb_addf(err, "%c%d-%d is not in the diff", old ? 'o' : 'n', a,
			b);
		return -1;
	}
	return n;
}

int sel_apply(struct fdiff *fd, const char *spec, struct strbuf *err)
{
	return sel_apply_ex(fd, spec, false, err);
}

bool sel_side(const char **p, bool *old)
{
	static const struct {
		const char *s;
		bool old;
	} pfx[] = { { "new:", false },
		    { "old:", true },
		    { "n", false },
		    { "o", true } };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(pfx); i++) {
		if (starts_with(*p, pfx[i].s)) {
			*p += strlen(pfx[i].s);
			*old = pfx[i].old;
			return true;
		}
	}
	return false;
}

int sel_apply_ex(struct fdiff *fd, const char *spec, bool ctx,
		 struct strbuf *err)
{
	const char *p = spec;
	int total = 0;

	if (!*p) {
		sb_adds(err, "empty selection");
		return -1;
	}
	while (*p) {
		bool old = false, range = false;
		int a, b, n;

		if (*p == 'L' || (p[0] == '-' && p[1] == 'L')) {
			sb_addf(err,
				"'%s': selections are now n<N> (new side) "
				"and o<N> (old side), e.g. o12,n12",
				spec);
			return -1;
		}
		if (!sel_side(&p, &old)) {
			sb_addf(err,
				"bad selection '%s' (expected n<N>, o<N>, "
				"ranges like n<A>-<B>, comma separated)",
				spec);
			return -1;
		}
		if (parse_int(p, &p, &a) || a < 1) {
			sb_addf(err, "bad line number in '%s'", spec);
			return -1;
		}
		b = a;
		if (*p == '-') {
			range = true;
			p++;
			if (parse_int(p, &p, &b) || b < a) {
				sb_addf(err, "bad range in '%s'", spec);
				return -1;
			}
		}
		if (*p == ',') {
			p++;
		} else if (*p) {
			sb_addf(err, "bad selection '%s'", spec);
			return -1;
		}
		n = sel_item(fd, old, a, b, range, ctx, err);
		if (n < 0)
			return -1;
		total += n;
	}
	return total;
}

size_t hunk_order(const struct hunk *h, size_t *order)
{
	size_t i = 0, n = 0, j, k, m, t;

	while (i < h->nlines) {
		if (h->lines[i].op != '-') {
			order[n++] = i++;
			continue;
		}
		/* a change block, removals i..j-1, then additions j..k-1 */
		for (j = i; j < h->nlines && h->lines[j].op == '-'; j++)
			;
		for (k = j; k < h->nlines && h->lines[k].op == '+'; k++)
			;
		m = j - i < k - j ? j - i : k - j;
		for (t = 0; t < m; t++) {
			order[n++] = i + t;
			order[n++] = j + t;
		}
		for (t = i + m; t < j; t++)
			order[n++] = t;
		for (t = j + m; t < k; t++)
			order[n++] = t;
		i = k;
	}
	return n;
}

/* Seconds since an arbitrary start. */
static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/*
 * Whether a key waits on the terminal. Keys typed in the first 300 ms are
 * typed ahead and do not count, as for a git that run_git stops.
 */
static bool key_waiting(double since)
{
	struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };

	return now_s() - since > 0.3 && poll(&p, 1, 0) > 0 &&
	       (p.revents & POLLIN);
}

/* A changed file of diff_paths_matching, and whether pat is in it. */
struct scan_file {
	const char *path, *path2; /* path2: the new path of a rename */
	char ooid[65], noid[65];
	bool hit;
};

/* What the cat-file answers of diff_paths_matching go through. */
struct scan {
	struct scan_file *f;
	size_t *of; /* the file of each object asked for */
	const char *pat;
	const struct diff_opts *opts;
	double start;
	bool cancelled;
};

static bool is_null_oid(const char *oid)
{
	return strspn(oid, "0") == strlen(oid);
}

/* Whether the scan should stop for a key, and if so it says why. */
static bool scan_cancel(struct scan *sc, size_t i)
{
	if (!sc->opts || !sc->opts->lim.cancel_on_tty || i % 64 ||
	    !key_waiting(sc->start))
		return false;
	if (sc->opts->lim.stopped)
		*sc->opts->lim.stopped = RUN_CANCELLED;
	sc->cancelled = true;
	return true;
}

/* One blob from cat-file. A blob that cannot be read counts as a hit. */
static bool scan_blob(void *ctx, size_t i, const char *data, size_t len)
{
	struct scan *sc = ctx;
	struct scan_file *f = &sc->f[sc->of[i]];

	if (scan_cancel(sc, i))
		return false;
	if (!f->hit)
		f->hit = !data || smart_match(data, len, sc->pat);
	return true;
}

/*
 * Whether the work-tree file holds pat. A missing file is an empty side,
 * and one that cannot be read counts as a hit.
 */
static bool worktree_has(struct repo *r, const char *path, const char *pat,
			 struct strbuf *buf)
{
	struct strbuf full = STRBUF_INIT;
	bool yes = true;
	struct stat st;

	sb_reset(buf);
	sb_addf(&full, "%s/%s", r->top, path);
	if (lstat(full.buf, &st))
		yes = false;
	else if (S_ISREG(st.st_mode) && !read_file(full.buf, buf))
		yes = smart_match(sb_str(buf), buf->len, pat);
	sb_release(&full);
	return yes;
}

/* The files of git diff --raw -z output, which they point into. */
static size_t parse_raw(const struct strbuf *o, struct scan_file **out)
{
	const char *p = sb_str(o), *end = p + o->len;
	size_t n = 0, alloc = 0;

	*out = NULL;
	/* ":<mode> <mode> <oid> <oid> <status>\0<path>\0[<path>\0]" */
	while (p < end && *p == ':') {
		const char *st = p, *status;
		char omode[8], nmode[8];
		struct scan_file *f;

		ALLOC_GROW(*out, n + 1, alloc);
		f = &(*out)[n];
		memset(f, 0, sizeof(*f));
		p += strlen(p) + 1;
		if (p >= end || sscanf(st, ":%7s %7s %64s %64s", omode, nmode,
				       f->ooid, f->noid) != 4)
			break;
		status = strrchr(st, ' ');
		status = status ? status + 1 : "";
		f->path = p;
		p += strlen(p) + 1;
		if ((*status == 'R' || *status == 'C') && p < end) {
			f->path2 = p;
			p += strlen(p) + 1;
		}
		/* a submodule or a conflict, the diff decides */
		f->hit = !strcmp(omode, "160000") || !strcmp(nmode, "160000") ||
			 *status == 'U';
		n++;
	}
	return n;
}

int diff_paths_matching(struct repo *r, bool cached, const char *pat,
			const struct diff_opts *opts, struct argv *paths)
{
	struct strbuf o = STRBUF_INIT, e = STRBUF_INIT, buf = STRBUF_INIT;
	struct scan sc = { .pat = pat, .opts = opts, .start = now_s() };
	struct run_opts ro = { 0 };
	struct argv a = ARGV_INIT;
	size_t n = 0, nobj = 0, i;
	char **oids = NULL;
	int rc;

	argv_pushl(&a, "--no-optional-locks", GIT_PLAIN, "diff", "--raw", "-z",
		   "--no-abbrev", "--no-ext-diff", NULL);
	if (cached)
		argv_push(&a, "--cached");
	push_diff_excludes(&a, opts);
	diff_run_opts(opts, &ro);
	rc = run_git(&a, &ro, &o, &e);
	argv_clear(&a);
	if (diff_stopped(opts)) {
		rc = 0;
		goto out;
	}
	if (rc) {
		git_fail(r, "git diff --raw", &e);
		goto out;
	}
	n = parse_raw(&o, &sc.f);
	oids = xcalloc(2 * n + 1, sizeof(*oids));
	sc.of = xcalloc(2 * n + 1, sizeof(*sc.of));
	/* the work tree first, it needs no git */
	for (i = 0; i < n && !scan_cancel(&sc, i + 1); i++) {
		struct scan_file *f = &sc.f[i];

		if (!f->hit && is_null_oid(f->noid))
			f->hit = worktree_has(r, f->path2 ? f->path2 : f->path,
					      pat, &buf);
	}
	/* then the blobs, the new sides first */
	for (i = 0; i < n; i++)
		if (!sc.f[i].hit && !is_null_oid(sc.f[i].noid)) {
			sc.of[nobj] = i;
			oids[nobj++] = sc.f[i].noid;
		}
	for (i = 0; i < n; i++)
		if (!sc.f[i].hit && !is_null_oid(sc.f[i].ooid)) {
			sc.of[nobj] = i;
			oids[nobj++] = sc.f[i].ooid;
		}
	if (!sc.cancelled && repo_cat_each(r, oids, nobj, scan_blob, &sc))
		rc = repo_err(r, "git cat-file went away while searching");
	for (i = 0; !sc.cancelled && i < n; i++) {
		if (!sc.f[i].hit)
			continue;
		argv_push(paths, sc.f[i].path);
		if (sc.f[i].path2)
			argv_push(paths, sc.f[i].path2);
	}
out:
	free(sc.f);
	free(sc.of);
	free(oids);
	sb_release(&o);
	sb_release(&e);
	sb_release(&buf);
	return rc ? -1 : 0;
}

int load_diff_all(struct repo *r, bool cached, const struct diff_opts *opts,
		  struct fdiff **out, size_t *n)
{
	struct strbuf o = STRBUF_INIT, e = STRBUF_INIT, perrb = STRBUF_INIT;
	struct run_opts ro = { 0 };
	struct argv a = ARGV_INIT;
	int rc;

	*out = NULL;
	*n = 0;
	/*
	 * No index.lock, since a diff of every file may be stopped at any
	 * moment by a key or too many lines, even while it writes the
	 * index.
	 */
	argv_push(&a, "--no-optional-locks");
	push_diff_args(&a);
	push_diff_opts(&a, opts);
	if (cached)
		argv_push(&a, "--cached");
	push_diff_excludes(&a, opts);
	diff_run_opts(opts, &ro);
	rc = run_git(&a, &ro, &o, &e);
	argv_clear(&a);
	if (diff_stopped(opts))
		rc = 0;
	else if (rc)
		git_fail(r, "git diff", &e);
	else if (diff_parse(o.buf, o.len, out, n, &perrb))
		rc = repo_err(r, "%s", perrb.buf);
	sb_release(&o);
	sb_release(&e);
	sb_release(&perrb);
	return rc ? -1 : 0;
}

int fdiff_map_line(const struct fdiff *m, int x)
{
	int delta = 0;
	size_t i;

	for (i = 0; m && i < m->nhunks; i++) {
		const struct hunk *h = &m->hunks[i];

		if (h->new_count && x >= h->new_start &&
		    x < h->new_start + h->new_count)
			return -1;
		if ((h->new_count && h->new_start + h->new_count <= x) ||
		    (!h->new_count && h->new_start < x))
			delta += h->old_count - h->new_count;
	}
	return x + delta;
}

int fdiff_follow_line(const struct fdiff *m, int x)
{
	int delta = 0;
	size_t i;

	for (i = 0; m && i < m->nhunks; i++) {
		const struct hunk *h = &m->hunks[i];

		if (h->old_count && x >= h->old_start &&
		    x < h->old_start + h->old_count)
			return h->new_start > 0 ? h->new_start : 1;
		if ((h->old_count && h->old_start + h->old_count <= x) ||
		    (!h->old_count && h->old_start < x))
			delta += h->new_count - h->old_count;
	}
	return x + delta;
}

int worktree_line(struct repo *r, const char *rev, const char *path, int x)
{
	struct strbuf o = STRBUF_INIT, e = STRBUF_INIT, perr = STRBUF_INIT;
	struct fdiff *files = NULL;
	struct argv a = ARGV_INIT;
	size_t n = 0;
	int res = x;

	argv_push(&a, "--literal-pathspecs");
	push_diff_args(&a);
	/* after the -U3 of push_diff_args, where the last one wins */
	argv_pushl(&a, "-U0", rev, "--", path, NULL);
	if (!run_git(&a, NULL, &o, &e) &&
	    !diff_parse(o.buf, o.len, &files, &n, &perr) && n == 1)
		res = fdiff_follow_line(&files[0], x);
	fdiff_free_array(files, n);
	argv_clear(&a);
	sb_release(&o);
	sb_release(&e);
	sb_release(&perr);
	return res;
}

long dline_partner(const struct hunk *h, size_t x)
{
	size_t i, j, k, m;

	if (x >= h->nlines || h->lines[x].op == ' ')
		return -1;
	/* the change block around x, removals i..j-1, additions j..k-1 */
	for (i = x; i > 0 && h->lines[i - 1].op != ' ' &&
		    !(h->lines[i].op == '-' && h->lines[i - 1].op == '+');
	     i--)
		;
	for (j = i; j < h->nlines && h->lines[j].op == '-'; j++)
		;
	for (k = j; k < h->nlines && h->lines[k].op == '+'; k++)
		;
	m = j - i < k - j ? j - i : k - j;
	if (x < j)
		return x - i < m ? (long)(j + (x - i)) : -1;
	return x - j < m ? (long)(i + (x - j)) : -1;
}

static bool word_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80;
}

bool change_span(const char *a, size_t alen, const char *b, size_t blen,
		 size_t *start, size_t *aend, size_t *bend)
{
	size_t p = 0, s = 0, shorter = alen < blen ? alen : blen;

	while (p < shorter && a[p] == b[p])
		p++;
	while (s < shorter - p && a[alen - 1 - s] == b[blen - 1 - s])
		s++;
	/* for whole words, back to the start of the word the change is in */
	while (p > 0 && word_char(a[p - 1]) &&
	       ((p < alen && word_char(a[p])) || (p < blen && word_char(b[p]))))
		p--;
	while (s > 0 && word_char(a[alen - s]) &&
	       ((alen - s > p && word_char(a[alen - s - 1])) ||
		(blen - s > p && word_char(b[blen - s - 1]))))
		s--;
	/* too little in common to be worth showing */
	if ((p + s) * 4 < shorter || (p + s == alen && p + s == blen))
		return false;
	*start = p;
	*aend = alen - s;
	*bend = blen - s;
	return true;
}
