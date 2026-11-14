/* SPDX-License-Identifier: MIT */
/*
 * Search and replace on lines of a work-tree file, as vim's
 * :s/pattern/replacement/flags does, for a small edit without an editor.
 * Patterns are POSIX extended regular expressions, as for grep -E. In the
 * replacement & is the match, \1 to \9 its groups, and \& and \\ stand for
 * themselves. The flags are g, every match on a line instead of the first,
 * and i, ignoring case.
 */
#ifndef CARVE_SUBST_H
#define CARVE_SUBST_H

#include <regex.h>
#include <stdbool.h>

#include "git.h"

struct subst {
	regex_t re;
	char *repl;
	bool global;
	bool compiled;
};

/* A range of work-tree lines, 1-based and inclusive. */
struct line_range {
	int from, to;
};

/*
 * Parse "s/pattern/replacement/flags". Any character after the s is the
 * delimiter, a backslash escapes it, and the last one may be left out.
 * Returns 0, or -1 with the reason in err.
 */
int subst_parse(const char *expr, struct subst *s, struct strbuf *err);
void subst_free(struct subst *s);
/*
 * s on one line without its line end, the result in out. Returns the
 * number of substitutions made.
 */
int subst_line(const struct subst *s, const char *line, size_t len,
	       struct strbuf *out);
/*
 * Work-tree lines from a selection of new-side numbers such as "n12",
 * "n3-9" or "12", separated by commas. The new side of an unstaged diff is
 * the work tree. Old-side numbers are refused. Returns 0, or -1 with the
 * reason in err.
 */
int subst_ranges(const char *spec, struct line_range **out, size_t *n,
		 struct strbuf *err);
/*
 * s on the lines of the work-tree file path (relative to the top) in the
 * ranges, or on every line when n == 0. Line ends are kept, and the file
 * is written only if a line changed. Returns the number of lines changed,
 * or -1 on errors.
 */
int subst_file(struct repo *r, const char *path, const struct line_range *rg,
	       size_t n, const struct subst *s);

#endif
