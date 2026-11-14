// SPDX-License-Identifier: MIT
/*
 * Commit messages. Change-Id, review tags and Signed-off-by lines, the
 * editor templates of a split, and the subsystem prefix the history of the
 * staged files suggests.
 */
#include "message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * At most this many staged files vote, with this many commits each, from
 * this far back. A rarely changed file would otherwise walk all of a
 * kernel's history, which takes seconds. Above PREFIX_MAX staged files one
 * prefix means nothing and none is suggested.
 */
#define PREFIX_FILES 50
#define PREFIX_COMMITS 20
#define PREFIX_YEARS 2
#define PREFIX_MAX 500

char *change_id_of(const char *msg)
{
	const char *p = msg, *found = NULL;

	while (p && *p) {
		if (starts_with(p, "Change-Id:"))
			found = p;
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	if (!found)
		return NULL;
	found += strlen("Change-Id:");
	while (*found == ' ' || *found == '\t')
		found++;
	return xstrndup(found, strcspn(found, "\r\n"));
}

void strip_change_id(const char *msg, struct strbuf *out)
{
	const char *p = msg;

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len;

		len = nl ? (size_t)(nl - p) + 1 : strlen(p);

		if (!starts_with(p, "Change-Id:"))
			sb_add(out, p, len);
		p += len;
	}
	sb_rtrim(out);
	sb_addch(out, '\n');
}

/* A review tag, given for the patch as it was and not for a part of it. */
static const char *review_tag(const char *line)
{
	static const char *const tags[] = { "Reviewed-by:", "Acked-by:",
					    "Tested-by:" };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(tags); i++)
		if (!strncasecmp(line, tags[i], strlen(tags[i])))
			return tags[i];
	return NULL;
}

void strip_review_tags(const char *msg, struct strbuf *out,
		       struct strbuf *dropped)
{
	const char *p = msg;

	while (*p) {
		const char *nl = strchr(p, '\n');
		const char *tag = review_tag(p);
		size_t len;

		len = nl ? (size_t)(nl - p) + 1 : strlen(p);

		if (!tag) {
			sb_add(out, p, len);
		} else if (dropped) {
			/* each name once, as "Reviewed-by, Acked-by" */
			char *name = xstrndup(tag, strlen(tag) - 1);

			if (!strstr(sb_str(dropped), name)) {
				if (dropped->len)
					sb_adds(dropped, ", ");
				sb_adds(dropped, name);
			}
			free(name);
		}
		p += len;
	}
}

void signoffs_of(const char *msg, struct strbuf *out)
{
	const char *p = msg;

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len;

		len = nl ? (size_t)(nl - p) : strlen(p);

		if (!strncasecmp(p, "Signed-off-by:", 14)) {
			sb_add(out, p, len);
			sb_addch(out, '\n');
		}
		p += len + (nl ? 1 : 0);
	}
}

/* Whether s (lines) has a line that is exactly line. */
static bool has_line(const char *s, const char *line)
{
	size_t n = strlen(line);
	const char *p = s;

	while ((p = strstr(p, line))) {
		if ((p == s || p[-1] == '\n') && (p[n] == '\n' || !p[n]))
			return true;
		p++;
	}
	return false;
}

void split_template(const char *msg, bool first, bool keep_change_id,
		    char comment, const char *signoff, struct strbuf *out)
{
	struct strbuf base = STRBUF_INIT, stripped = STRBUF_INIT,
		      sobs = STRBUF_INIT;
	char *cid = change_id_of(msg);
	const char *p;

	/* review tags were given for the whole patch and no part keeps them */
	strip_review_tags(msg, &base, NULL);
	strip_change_id(sb_str(&base), &stripped);
	if (first) {
		if (keep_change_id) {
			sb_adds(out, sb_str(&base));
			if (!out->len || out->buf[out->len - 1] != '\n')
				sb_addch(out, '\n');
		} else {
			sb_adds(out, stripped.buf);
		}
		goto out;
	}
	/* every part is signed off as the original was */
	signoffs_of(stripped.buf, &sobs);
	if (signoff && !has_line(sb_str(&sobs), signoff))
		sb_addf(&sobs, "%s\n", signoff);
	sb_addch(out, '\n');
	if ((keep_change_id && cid) || sobs.len)
		sb_addch(out, '\n');
	if (keep_change_id && cid)
		sb_addf(out, "Change-Id: %s\n", cid);
	sb_add(out, sb_str(&sobs), sobs.len);
	sb_addch(out, '\n');
	add_scissors(out, comment);
	sb_addf(out, "%c Original message of the commit being split:\n%c\n",
		comment, comment);
	for (p = stripped.buf; *p;) {
		const char *nl = strchr(p, '\n');
		size_t len;

		len = nl ? (size_t)(nl - p) : strlen(p);

		if (len)
			sb_addf(out, "%c %.*s\n", comment, (int)len, p);
		else
			sb_addf(out, "%c\n", comment);
		p += len + (nl ? 1 : 0);
	}
out:
	free(cid);
	sb_release(&base);
	sb_release(&stripped);
	sb_release(&sobs);
}

char commit_comment_char(struct repo *r)
{
	char *c = repo_config_dup(r, "core.commentChar");
	char res = '#';

	if (c && *c && strcmp(c, "auto"))
		res = c[0];
	free(c);
	return res;
}

void add_scissors(struct strbuf *out, char c)
{
	sb_addf(out,
		"%c " SCISSORS_LINE
		"\n%c Do not modify or remove the line above.\n"
		"%c Everything below it is left out.\n",
		c, c, c);
}

char *signoff_line(struct repo *r)
{
	char *ident = repo_committer(r);
	struct strbuf sb = STRBUF_INIT;

	if (!ident) {
		repo_err(r, "cannot tell who signs off: set user.name and "
			    "user.email");
		return NULL;
	}
	sb_addf(&sb, "Signed-off-by: %s", ident);
	free(ident);
	return sb_detach(&sb);
}

void ended_line(struct strbuf *sb, const char *msg, size_t len)
{
	sb_add(sb, msg, len);
	if (len && msg[len - 1] != '\n')
		sb_addch(sb, '\n');
}

int add_signoff(struct repo *r, const char *msg, struct strbuf *out)
{
	struct strbuf in = STRBUF_INIT;
	char *line = signoff_line(r);
	int rc;

	if (!line)
		return -1;
	ended_line(&in, msg, strlen(msg));
	rc = git_in(r, in.buf, in.len, out, "interpret-trailers", "--if-exists",
		    "addIfDifferent", "--trailer", line, NULL);
	sb_release(&in);
	free(line);
	return rc ? -1 : 0;
}

int add_signoffs(struct repo *r, const char *orig, struct strbuf *text)
{
	struct strbuf sobs = STRBUF_INIT, out = STRBUF_INIT, in = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	const char *pos, *end, *l;
	size_t len;
	int rc = 0;

	signoffs_of(orig, &sobs);
	if (!sobs.len)
		goto done;
	argv_pushl(&a, "interpret-trailers", "--if-exists", "addIfDifferent",
		   NULL);
	pos = sobs.buf;
	end = pos + sobs.len;
	while ((l = next_line(&pos, end, &len))) {
		char *t = xstrndup(l, len);

		argv_pushl(&a, "--trailer", t, NULL);
		free(t);
	}
	ended_line(&in, sb_str(text), text->len);
	{
		struct run_opts o = { .in = sb_str(&in), .inlen = in.len };

		rc = run_git(&a, &o, &out, NULL) ?
			     repo_err(r, "git "
					 "interpret-trailers "
					 "failed") :
			     0;
	}
	if (!rc) {
		sb_reset(text);
		sb_add(text, sb_str(&out), out.len);
	}
done:
	argv_clear(&a);
	sb_release(&sobs);
	sb_release(&out);
	sb_release(&in);
	return rc;
}

void commit_note_patch(struct strbuf *note, const char *patch)
{
	const char *p = patch, *path = NULL;
	size_t n, shown = 0, more = 0;

	while (*p) {
		n = strcspn(p, "\n");
		if (!strncmp(p, "+++ b/", 6)) {
			path = p + 6;
			sb_addf(note, "  %.*s:\n", (int)(n - 6), path);
		} else if ((*p == '+' || *p == '-') && strncmp(p, "--- ", 4) &&
			   strncmp(p, "+++ ", 4)) {
			if (shown < 20)
				sb_addf(note, "    %.*s\n", (int)n, p);
			else
				more++;
			shown++;
		}
		p += n + (p[n] ? 1 : 0);
	}
	if (more)
		sb_addf(note, "    ... and %zu more\n", more);
}

bool config_signoff(struct repo *r)
{
	return repo_config_bool(r, "carve.signoff", false);
}

/* "net: phy: fix x" becomes "net: phy", everything before the last ": ". */
static char *prefix_of(const char *subject)
{
	const char *last = NULL, *p;

	for (p = subject; (p = strstr(p, ": ")); p += 2)
		last = p;
	if (!last || last == subject)
		return NULL;
	/* "Revert "...", "Merge ..." and prose are not prefixes */
	for (p = subject; p < last; p++)
		if (*p == '"' || (*p == ' ' && (p == subject || p[-1] != ':')))
			return NULL;
	return xstrndup(subject, (size_t)(last - subject));
}

/* Count one vote for the prefix p (taken over) in pre / count. */
static void vote(char ***pre, int **count, size_t *n, size_t *alloc,
		 size_t *alloc2, char *p, int *bestn, char **best)
{
	size_t i;

	for (i = 0; i < *n && strcmp((*pre)[i], p); i++)
		;
	if (i == *n) {
		ALLOC_GROW(*pre, *n + 1, *alloc);
		ALLOC_GROW(*count, *n + 1, *alloc2);
		(*pre)[*n] = p;
		(*count)[(*n)++] = 0;
	} else {
		free(p);
	}
	if (++(*count)[i] > *bestn) {
		*bestn = (*count)[i];
		*best = (*pre)[i];
	}
}

/* HEAD's committer time (through the cat-file worker), 0 if unknown. */
static long long head_time(struct repo *r)
{
	struct strbuf c = STRBUF_INIT;
	const char *l, *gt;
	long long t = 0;

	if (!repo_cat(r, "HEAD", &c, NULL) &&
	    (l = strstr(sb_str(&c), "\ncommitter ")) && (gt = strchr(l, '>')))
		t = strtoll(gt + 1, NULL, 10);
	sb_release(&c);
	return t;
}

char *suggest_prefix(struct repo *r)
{
	size_t n = 0, alloc = 0, alloc2 = 0, len, i, np = 0, palloc = 0;
	char **pre = NULL, **paths = NULL, *best = NULL, *subj = NULL;
	bool off = !repo_config_bool(r, "carve.suggestPrefix", true);
	struct strbuf files = STRBUF_INIT, log = STRBUF_INIT;
	int *count = NULL, *seen = NULL, bestn = 0;
	char window[32], since[48];
	struct argv a = ARGV_INIT;
	const char *pos, *end, *l;
	long long t;

	/* new files have no history to vote with */
	if (off ||
	    git_out(r, &files, GIT_PLAIN, "diff", "--cached", "--name-only",
		    "--no-renames", "--diff-filter=a", NULL))
		goto out;
	if (count_lines(files.buf, files.len) > PREFIX_MAX)
		goto out;
	pos = sb_str(&files);
	end = sb_str(&files) + files.len;
	while (np < PREFIX_FILES && (l = next_line(&pos, end, &len))) {
		ALLOC_GROW(paths, np + 1, palloc);
		paths[np++] = xstrndup(l, len);
	}
	if (!np)
		goto out;
	/*
	 * The most frequent prefix in the recent history of these files.
	 * One git log covers them all, and each file votes with the
	 * subjects of its last PREFIX_COMMITS commits.
	 */
	seen = xcalloc(np, sizeof(*seen));
	snprintf(window, sizeof(window), "%zu", np * PREFIX_COMMITS);
	argv_pushl(&a, GIT_PLAIN, "log", "-n", window, "--no-merges",
		   "--no-renames", "--format=%x01%s", NULL);
	/*
	 * the years before HEAD, not before now, since a repository may be
	 * old
	 */
	t = head_time(r);
	if (t > PREFIX_YEARS * 366LL * 86400) {
		snprintf(since, sizeof(since), "--since=%lld",
			 t - PREFIX_YEARS * 366LL * 86400);
		argv_push(&a, since);
	}
	argv_pushl(&a, "--name-only", "--", NULL);
	for (i = 0; i < np; i++)
		argv_push(&a, paths[i]);
	if (run_git(&a, NULL, &log, NULL))
		goto out;
	pos = sb_str(&log);
	end = pos + log.len;
	while ((l = next_line(&pos, end, &len))) {
		char *path, *p;

		if (*l == '\1') {
			free(subj);
			subj = xstrndup(l + 1, len - 1);
			continue;
		}
		if (!len || !subj)
			continue;
		path = xstrndup(l, len);
		for (i = 0; i < np && strcmp(paths[i], path); i++)
			;
		free(path);
		if (i == np || seen[i]++ >= PREFIX_COMMITS ||
		    !(p = prefix_of(subj)))
			continue;
		vote(&pre, &count, &n, &alloc, &alloc2, p, &bestn, &best);
	}
	best = best ? xstrdup(best) : NULL;
out:
	sb_reset(&r->err);
	for (i = 0; i < n; i++)
		free(pre[i]);
	for (i = 0; i < np; i++)
		free(paths[i]);
	free(pre);
	free(paths);
	free(count);
	free(seen);
	free(subj);
	argv_clear(&a);
	sb_release(&files);
	sb_release(&log);
	return best;
}

int draft_read(struct repo *r, struct strbuf *out)
{
	const char *path = repo_git_path(r, DRAFT_FILE);
	struct stat st;

	if (!path)
		return -1;
	if (stat(path, &st))
		return 0;
	return read_file(path, out);
}

int draft_write(struct repo *r, const char *text, size_t len)
{
	const char *path = repo_git_path(r, DRAFT_FILE);

	if (!path || write_file(path, text, len))
		return repo_err(r, "cannot write the commit message (%s)",
				path ? path : DRAFT_FILE);
	return 0;
}

void draft_remove(struct repo *r)
{
	const char *path = repo_git_path(r, DRAFT_FILE);

	if (path)
		unlink(path);
}

void message_strip(const char *msg, char c, struct strbuf *out)
{
	const char *p = msg;

	while (*p) {
		size_t n = strcspn(p, "\n");

		/* "# ---- >8 ----" and all below it */
		if (n > 2 && p[0] == c && p[1] == ' ' &&
		    !strncmp(p + 2, SCISSORS_LINE, strlen(SCISSORS_LINE)))
			break;
		if (!(n && p[0] == c)) {
			sb_add(out, p, n);
			sb_addch(out, '\n');
		}
		p += n + (p[n] ? 1 : 0);
	}
	sb_rtrim(out);
	/* leading blank lines go too */
	while (out->len && out->buf[0] == '\n') {
		memmove(out->buf, out->buf + 1, out->len);
		out->len--;
	}
	if (out->len)
		sb_addch(out, '\n');
}
