// SPDX-License-Identifier: MIT
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void die(const char *fmt, ...)
{
	va_list ap;

	fputs("git-carve: fatal: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(128);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p)
		die("out of memory");
	return p;
}

void *xcalloc(size_t nmemb, size_t size)
{
	void *p = calloc(nmemb ? nmemb : 1, size ? size : 1);

	if (!p)
		die("out of memory");
	return p;
}

void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);

	if (!q)
		die("out of memory");
	return q;
}

char *xstrdup(const char *s)
{
	return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);

	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

static void sb_grow(struct strbuf *sb, size_t extra)
{
	size_t need = sb->len + extra + 1;

	if (need <= sb->alloc && sb->buf)
		return;
	if (sb->alloc * 2 > need)
		need = sb->alloc * 2;
	if (need < 64)
		need = 64;
	sb->buf = xrealloc(sb->buf, need);
	sb->alloc = need;
}

void sb_add(struct strbuf *sb, const void *data, size_t len)
{
	sb_grow(sb, len);
	if (len)
		memcpy(sb->buf + sb->len, data, len);
	sb->len += len;
	sb->buf[sb->len] = '\0';
}

void sb_adds(struct strbuf *sb, const char *s)
{
	sb_add(sb, s, strlen(s));
}

void sb_addch(struct strbuf *sb, char c)
{
	sb_add(sb, &c, 1);
}

void sb_vaddf(struct strbuf *sb, const char *fmt, va_list ap)
{
	va_list cp;
	int n;

	va_copy(cp, ap);
	n = vsnprintf(NULL, 0, fmt, cp);
	/* clang-tidy 22 misses the va_copy above on glibc */
	va_end(cp); /* NOLINT(clang-analyzer-security.VAList) */
	if (n < 0)
		die("vsnprintf failed");
	sb_grow(sb, (size_t)n);
	vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap);
	sb->len += (size_t)n;
}

void sb_addf(struct strbuf *sb, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	sb_vaddf(sb, fmt, ap);
	va_end(ap);
}

void sb_reset(struct strbuf *sb)
{
	sb->len = 0;
	if (sb->buf)
		sb->buf[0] = '\0';
}

void sb_release(struct strbuf *sb)
{
	free(sb->buf);
	sb->buf = NULL;
	sb->len = sb->alloc = 0;
}

char *sb_detach(struct strbuf *sb)
{
	char *p;

	if (!sb->buf)
		return xstrdup("");
	p = sb->buf;
	sb->buf = NULL;
	sb->len = sb->alloc = 0;
	return p;
}

const char *sb_str(const struct strbuf *sb)
{
	return sb->buf ? sb->buf : "";
}

void sb_rtrim(struct strbuf *sb)
{
	while (sb->len &&
	       (sb->buf[sb->len - 1] == '\n' || sb->buf[sb->len - 1] == '\r' ||
		sb->buf[sb->len - 1] == ' ' || sb->buf[sb->len - 1] == '\t'))
		sb->buf[--sb->len] = '\0';
}

void argv_push(struct argv *a, const char *s)
{
	ALLOC_GROW(a->v, a->n + 2, a->alloc);
	a->v[a->n++] = xstrdup(s);
	a->v[a->n] = NULL;
}

void argv_pushl(struct argv *a, ...)
{
	const char *s;
	va_list ap;

	va_start(ap, a);
	while ((s = va_arg(ap, const char *)))
		argv_push(a, s);
	va_end(ap);
}

void argv_clear(struct argv *a)
{
	size_t i;

	for (i = 0; i < a->n; i++)
		free(a->v[i]);
	free(a->v);
	a->v = NULL;
	a->n = a->alloc = 0;
}

const char *next_line(const char **pos, const char *end, size_t *len)
{
	const char *start = *pos, *nl;

	if (!start || start >= end)
		return NULL;
	nl = memchr(start, '\n', (size_t)(end - start));
	if (nl) {
		*len = (size_t)(nl - start);
		*pos = nl + 1;
	} else {
		*len = (size_t)(end - start);
		*pos = end;
	}
	return start;
}

int read_file(const char *path, struct strbuf *sb)
{
	int fd = open(path, O_RDONLY);
	char buf[8192];
	ssize_t n;

	if (fd < 0)
		return -1;
	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		sb_add(sb, buf, (size_t)n);
	}
	close(fd);
	if (!sb->buf)
		sb_add(sb, "", 0);
	return n < 0 ? -1 : 0;
}

int write_file(const char *path, const void *data, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	const char *p = data;
	ssize_t n;

	if (fd < 0)
		return -1;
	while (len) {
		n = write(fd, p, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			close(fd);
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return close(fd);
}

bool starts_with(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool mem_starts_with(const char *s, size_t len, const char *prefix)
{
	size_t pl = strlen(prefix);

	return len >= pl && memcmp(s, prefix, pl) == 0;
}

int parse_long(const char *s, const char **end, long *out)
{
	char *e;
	long v;

	/* strtol would take leading spaces and a sign too */
	if (!s || *s < '0' || *s > '9')
		return -1;
	errno = 0;
	v = strtol(s, &e, 10);
	if (errno)
		return -1;
	*out = v;
	if (end)
		*end = e;
	return 0;
}

int parse_int(const char *s, const char **end, int *out)
{
	long v;

	if (parse_long(s, end, &v) || v > INT_MAX)
		return -1;
	*out = (int)v;
	return 0;
}

long number_or(const char *s, long fallback)
{
	long v;

	return parse_long(s, NULL, &v) ? fallback : v;
}

/* Whether s starts with the n bytes of pat, ignoring case with icase. */
static bool starts_like(const char *s, const char *pat, size_t n, bool icase)
{
	size_t k;

	for (k = 0; k < n; k++) {
		char a = s[k];

		if (icase && a >= 'A' && a <= 'Z')
			a += 'a' - 'A';
		if (a != pat[k])
			return false;
	}
	return true;
}

/*
 * Candidates are found with memchr for the first character of pat, in
 * both cases when case is ignored. Each kind of candidate is looked for
 * again only once the scan has passed the last one found, and every byte
 * is read at most twice even in a file of a gigabyte.
 */
bool smart_match(const char *s, size_t len, const char *pat)
{
	const char *p, *end, *lo = NULL, *up = NULL;
	size_t pl = strlen(pat), k;
	bool icase = true;
	char first, other;

	if (!pl)
		return true;
	/* an empty strbuf has no buffer */
	if (!s || len < pl)
		return false;
	p = s;
	end = s + len;
	for (k = 0; k < pl; k++)
		if (pat[k] >= 'A' && pat[k] <= 'Z')
			icase = false;
	first = pat[0];
	other = icase && first >= 'a' && first <= 'z' ? first - 'a' + 'A' :
							first;
	while ((size_t)(end - p) >= pl) {
		const char *c;

		if (!lo || lo < p) {
			lo = memchr(p, first, (size_t)(end - p));
			if (!lo)
				lo = end;
		}
		if (other != first && (!up || up < p)) {
			up = memchr(p, other, (size_t)(end - p));
			if (!up)
				up = end;
		}
		c = other != first && up < lo ? up : lo;
		if ((size_t)(end - c) < pl)
			return false;
		if (starts_like(c + 1, pat + 1, pl - 1, icase))
			return true;
		p = c + 1;
	}
	return false;
}

size_t count_lines(const char *s, size_t len)
{
	const char *p = s, *end;
	size_t n = 0;

	if (!s)
		return 0; /* an empty strbuf has no buffer */
	end = s + len;
	while ((p = memchr(p, '\n', (size_t)(end - p)))) {
		n++;
		p++;
	}
	return n;
}
