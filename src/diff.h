/* SPDX-License-Identifier: MIT */
#ifndef CARVE_DIFF_H
#define CARVE_DIFF_H

#include "git.h"

struct dline {
	char op;	  /* ' ', '+', '-' */
	int old_no;	  /* 0 if absent on old side */
	int new_no;	  /* 0 if absent on new side */
	const char *text; /* points into fdiff.raw, not NUL terminated */
	size_t len;
	bool no_eol; /* followed by "\ No newline at end of file" */
	bool sel;    /* selected for a partial operation */
};

struct hunk {
	int old_start, old_count, new_start, new_count;
	char *section; /* text after the second @@, may be "" */
	struct dline *lines;
	size_t nlines;
};

struct fdiff {
	char **hdr; /* diff --git, index, mode, rename, ---/+++ */
	size_t nhdr;
	char *old_path; /* unquoted, without prefix, NULL for /dev/null */
	char *new_path;
	struct hunk *hunks;
	size_t nhunks;
	bool binary, is_new, is_deleted, is_rename, mode_only;
	bool mode_change; /* old mode/new mode present */
	bool combined;	  /* diff --cc (unmerged), not line-editable */
	bool zero_context; /* loaded with -U0, which git apply needs --unidiff-zero for */
	bool view_only; /* loaded with -w, which makes it read only */
	char *raw;	/* owns all dline.text */
};

/*
 * Parse unified diff output (possibly several files). On success returns
 * 0 and a newly allocated array in *out. On malformed input returns -1 and
 * describes the problem in *err.
 */
int diff_parse(const char *buf, size_t len, struct fdiff **out, size_t *nout,
	       struct strbuf *err);
void fdiff_free(struct fdiff *fd);
/* A copy of src that owns everything it points to. */
void fdiff_dup(const struct fdiff *src, struct fdiff *dst);
void fdiff_free_array(struct fdiff *fd, size_t n);

/*
 * Whether line-level operations make sense for this diff. If not, *why
 * gets a short human readable reason.
 */
bool fdiff_line_ops(const struct fdiff *fd, const char **why);

/* Undo C-style quoting of a git path (with or without quotes). */
char *unquote_path(const char *s, size_t len);
/* Append path, C-quoted if git would need quoting to read it back. */
void quote_path(struct strbuf *sb, const char *path);

/* How a diff is shown. Line operations work with any context. */
struct diff_opts {
	int context;	       /* -U<n> */
	bool function_context; /* -W */
	bool ignore_ws;	       /* -w, which makes the view read only */
	/*
	 * Loading (the TUI): git may be stopped early (struct run_limits).
	 * A load that was stopped gives no diff (*out NULL), and *stopped
	 * says why.
	 */
	struct run_limits lim;
	/*
	 * For the unstaged diff the caller knows path is in the index, or
	 * that it is untracked and is diffed with --no-index at once.
	 */
	bool in_index, untracked;
	/*
	 * load_diff_all and load_commit_diff_all leave out the files
	 * matching these patterns, as path_matches() reads them.
	 */
	char *const *exclude;
	size_t nexclude;
	/* load_diff_all diffs only these paths, when there are any. */
	char *const *only;
	size_t nonly;
};
/*
 * Pathspecs leaving out opts' exclude patterns and keeping opts' only
 * paths, with "--" first if there are any.
 */
void push_diff_excludes(struct argv *a, const struct diff_opts *opts);
#define DIFF_OPTS_INIT { .context = 3 }

/* The loading limits of opts, if not NULL, into ro. */
void diff_run_opts(const struct diff_opts *opts, struct run_opts *ro);
/* Whether the last load with opts was stopped early. */
bool diff_stopped(const struct diff_opts *opts);

/* Push -U<n>, -W and -w from opts, if not NULL, after push_diff_args(). */
void push_diff_opts(struct argv *a, const struct diff_opts *opts);
/* load_diff with display options, or the defaults for NULL. */
int load_diff_opts(struct repo *r, const char *path, bool cached,
		   const struct diff_opts *opts, struct fdiff **out);
/* The same against another index file (GIT_INDEX_FILE), path in it. */
int load_diff_index(struct repo *r, const char *index_file, const char *path,
		    bool cached, const struct diff_opts *opts,
		    struct fdiff **out);
/*
 * Load the unstaged (index -> worktree) or staged (HEAD -> index) diff of
 * one path. Untracked files get a /dev/null -> file diff. *out is NULL when
 * there are no changes.
 */
int load_diff(struct repo *r, const char *path, bool cached,
	      struct fdiff **out);

/*
 * The changed paths whose unstaged (or staged) diff may have a line that
 * smart_match() finds pat in, both paths of a rename. It reads the old
 * and the new content of every changed file, from the work tree and
 * through git cat-file, and makes no diff. That is many times faster than
 * the diffs of a huge change set. A key stops it as opts' limits say, and
 * then nothing is found.
 */
int diff_paths_matching(struct repo *r, bool cached, const char *pat,
			const struct diff_opts *opts, struct argv *paths);

/* Every tracked file's unstaged (or staged) diff, from one git diff. */
int load_diff_all(struct repo *r, bool cached, const struct diff_opts *opts,
		  struct fdiff **out, size_t *n);

/* Whether path has an entry in the index. -1 on error. */
int path_in_index(struct repo *r, const char *path);
/* Whether path exists in the work tree (lstat). */
bool path_in_worktree(const char *path);
/* Size of the work-tree file (not followed if a link), -1 if absent. */
long worktree_size(const char *path);
/*
 * Whether path matches one of the patterns of carve.skipDiff, read as
 * .gitignore reads them. A pattern without a slash matches the file name
 * in any directory, and one with a slash matches the path from the top, a
 * leading slash dropped. Globs are those of fnmatch(3), *, ? and [...],
 * without **.
 */
bool path_matches(const char *path, char *const *pats, size_t n);

/*
 * The old-side line of new-side line x in the -U0 diff m (NULL when
 * nothing changed), or -1 if x itself was changed.
 */
int fdiff_map_line(const struct fdiff *m, int x);
/*
 * The new-side line of old-side line x in the -U0 diff m (NULL when
 * nothing changed). For a changed line it is the first line of what
 * replaced it, or the line before when it was only removed.
 */
int fdiff_follow_line(const struct fdiff *m, int x);
/*
 * The work-tree line of line x of path in rev, through fdiff_follow_line,
 * or x itself when that cannot be worked out.
 */
int worktree_line(struct repo *r, const char *rev, const char *path, int x);

/* Annotated diff with old/new line number columns (git carve diff). */
void diff_annotate(const struct fdiff *fd, struct strbuf *out);

/*
 * The order in which partial operations walk a hunk's lines, as h->nlines
 * indices into h->lines. Git prints a change block as all its removals
 * followed by all its additions. They are paired up in order (-a +A -b
 * +B), and taking one pair keeps its place. Staging a->A out of "-a -b +A
 * +B" must give "A b", not "b A". Leftovers of the longer side follow.
 */
size_t hunk_order(const struct hunk *h, size_t *order);
/* The line x of h is paired with (as hunk_order pairs them), or -1. */
long dline_partner(const struct hunk *h, size_t x);
/*
 * The changed middle of two paired lines, extended to whole words, as
 * [start, aend) of a and [start, bend) of b. False when they have too
 * little in common for it to help, or nothing differs.
 */
bool change_span(const char *a, size_t alen, const char *b, size_t blen,
		 size_t *start, size_t *aend, size_t *bend);

/*
 * Consume a side prefix (n, o, new: or old:) at *p and set *old to the
 * side. False if there is none. The TUI's ":" jump uses it too.
 */
bool sel_side(const char **p, bool *old);
/*
 * Selections. A spec is a comma separated list of items "n<N>" or
 * "n<A>-<B>" for new-side line numbers and "o<N>" or "o<A>-<B>" for the
 * old side. "new:" and "old:" are long forms of the prefixes. A single
 * number selects exactly that line. A range selects every change line
 * whose number lies in it. A removed line counts as sitting at the
 * new-side number of the line after it, and an added line at the old-side
 * number of the line before it, which takes both halves of a change.
 * Returns the number of selected change lines, or -1 with a message in
 * *err.
 */
int sel_apply(struct fdiff *fd, const char *spec, struct strbuf *err);
/* sel_apply, also marking the context rows in the ranges when ctx is set. */
int sel_apply_ex(struct fdiff *fd, const char *spec, bool ctx,
		 struct strbuf *err);
void sel_clear(struct fdiff *fd);
size_t sel_count(const struct fdiff *fd);

/* The line git writes after a line without a newline. */
#define NO_EOL_MARKER "\\ No newline at end of file\n"

#endif
