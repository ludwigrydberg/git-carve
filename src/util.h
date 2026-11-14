/* SPDX-License-Identifier: MIT */
#ifndef CARVE_UTIL_H
#define CARVE_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#define NORETURN __attribute__((noreturn))
/* An argument list that ends in NULL, checked by the compiler. */
#define SENTINEL __attribute__((sentinel))
#else
#define PRINTF_LIKE(f, a)
#define NORETURN
#define SENTINEL
#endif

/*
 * Print "fatal: ..." and exit, for what cannot go on, such as running out
 * of memory.
 */
NORETURN void die(const char *fmt, ...) PRINTF_LIKE(1, 2);

/* malloc and friends that die instead of returning NULL */
void *xmalloc(size_t n);
void *xcalloc(size_t nmemb, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Grow *p (holding *alloc elements of size sz) to hold at least n. */
#define ALLOC_GROW(p, n, alloc)                                      \
	do {                                                         \
		if ((n) > (alloc)) {                                 \
			(alloc) = (alloc) ? (alloc) * 2 : 8;         \
			if ((alloc) < (n))                           \
				(alloc) = (n);                       \
			(p) = xrealloc((p), (alloc) * sizeof(*(p))); \
		}                                                    \
	} while (0)

/*
 * A string that grows as it is added to. sb_detach hands its buffer to the
 * caller, who frees it, and leaves the strbuf empty.
 */
struct strbuf {
	char *buf; /* always NUL terminated once allocated */
	size_t len;
	size_t alloc;
};

#define STRBUF_INIT { NULL, 0, 0 }

void sb_add(struct strbuf *sb, const void *data, size_t len);
void sb_adds(struct strbuf *sb, const char *s);
void sb_addch(struct strbuf *sb, char c);
void sb_addf(struct strbuf *sb, const char *fmt, ...) PRINTF_LIKE(2, 3);
void sb_vaddf(struct strbuf *sb, const char *fmt, va_list ap);
/* Empty it and keep the memory for reuse. */
void sb_reset(struct strbuf *sb);
/* Free the memory and start over. */
void sb_release(struct strbuf *sb);
char *sb_detach(struct strbuf *sb);
/* Returns sb->buf, or "" when nothing was ever added. */
const char *sb_str(const struct strbuf *sb);
/* Drop trailing whitespace, newlines included. */
void sb_rtrim(struct strbuf *sb);
/* The newlines in s[0, len): its lines, a last one without a newline not counted. */
size_t count_lines(const char *s, size_t len);

/* Dynamic NULL-terminated argument vector. */
struct argv {
	char **v;
	size_t n;
	size_t alloc;
};

#define ARGV_INIT { NULL, 0, 0 }

/* Each pushed string is copied. */
void argv_push(struct argv *a, const char *s);
void argv_pushl(struct argv *a, ...) SENTINEL;
void argv_clear(struct argv *a);

/*
 * Iterate over lines of [*pos, end). Returns the start of the next line and
 * stores its length (without '\n') in *len, or NULL at the end.
 */
const char *next_line(const char **pos, const char *end, size_t *len);

/* Read a whole file into sb (added to it), 0 or -1 with errno. */
int read_file(const char *path, struct strbuf *sb);
/* Write a whole file, replacing it, 0 or -1 with errno. */
int write_file(const char *path, const void *data, size_t len);

bool starts_with(const char *s, const char *prefix);
/*
 * Whether pat occurs in s[0, len). Case is ignored (ASCII only) unless pat
 * has an uppercase letter, the "smart case" of vim and less.
 */
bool smart_match(const char *s, size_t len, const char *pat);
bool mem_starts_with(const char *s, size_t len, const char *prefix);
/*
 * The decimal number s starts with, without sign or spaces. Returns 0 and
 * its end in *end if end is not NULL, or -1 when there is none or it does
 * not fit.
 */
int parse_long(const char *s, const char **end, long *out);
int parse_int(const char *s, const char **end, int *out);
/* The number s starts with, or fallback (s NULL or not a number). */
long number_or(const char *s, long fallback);

/* The nanoseconds of a struct stat's times (macOS names them apart). */
#ifdef __APPLE__
#define ST_MTIME_NSEC(st) ((st).st_mtimespec.tv_nsec)
#define ST_CTIME_NSEC(st) ((st).st_ctimespec.tv_nsec)
#else
#define ST_MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#define ST_CTIME_NSEC(st) ((st).st_ctim.tv_nsec)
#endif

#endif
