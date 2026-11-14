/* SPDX-License-Identifier: MIT */
#ifndef CARVE_STATUS_H
#define CARVE_STATUS_H

#include "git.h"

struct file_status {
	char *path;
	char x; /* index status (porcelain v1 X), ' ' if none */
	char y; /* work tree status (Y), '?' for untracked */
	bool unmerged;
	char *from_x; /* x == 'R': renamed from (HEAD -> index) */
	char *from_y; /* y == 'R': renamed from (index -> work tree) */
	/*
	 * In the TUI, an untracked entry of a scan whose path got tracked
	 * changes since, which were staged. It is still untracked only if
	 * it left the index.
	 */
	bool seen_tracked;
	/* In the TUI, whether such an entry is in the index, once asked. */
	bool ix_known, in_index;
};

/*
 * List changed paths (git status --porcelain=v1 -z --no-renames
 * --untracked-files=all), sorted by path.
 */
int load_status(struct repo *r, struct file_status **out, size_t *n);
/*
 * git status arguments, with --untracked-files=all when untracked is set
 * and =no otherwise.
 */
void status_args(struct argv *a, bool untracked);
/* An untracked entry (y '?') of path, which it takes over. */
struct file_status file_status_untracked(char *path);
/*
 * git ls-files -z -o output as untracked entries (y '?') sorted by path.
 * It is the background scan where git has no untracked cache.
 */
size_t parse_untracked(const char *buf, size_t len, struct file_status **out);
int load_status_mode(struct repo *r, bool untracked, struct file_status **out,
		     size_t *n);
/*
 * The index and the tracked files are as in HEAD (untracked files do not
 * count). what, if given, gets the first changed paths ("a.c, b.c and 2
 * more").
 */
bool tree_is_clean(struct repo *r, struct strbuf *what);
/* The one refusal for a tree that is not clean (repo_err, -1). */
#define DIRTY_TREE_MSG \
	"the index or work tree has changes; commit or stash them first"
int refuse_dirty(struct repo *r);
/* Tracked status (--untracked-files=no) of only these paths, sorted. */
int load_status_paths(struct repo *r, const char *const *paths, size_t np,
		      struct file_status **out, size_t *n);
/*
 * Replace the entries of paths in the sorted list *v of n by the sorted
 * entries add, which *v takes over. The replaced ones go to *old for the
 * caller to free. Returns the new length.
 */
size_t status_splice(struct file_status **v, size_t n, const char *const *paths,
		     size_t np, struct file_status *add, size_t nadd,
		     struct file_status **old, size_t *nold);
/*
 * The tracked entries and those untracked entries of a separate scan that
 * git does not track by now, sorted by path. Everything is copied.
 */
size_t merge_untracked(const struct file_status *tracked, size_t nt,
		       const struct file_status *scan, size_t ns,
		       struct file_status **out);
void free_status(struct file_status *fs, size_t n);
/* The .gitignore line matching exactly path (from the top). */
void exclude_pattern(const char *path, struct strbuf *out);
/*
 * Add path to $GIT_DIR/info/exclude, which keeps git from listing it as
 * untracked.
 */
int exclude_path(struct repo *r, const char *path);
/* The qsort order of file_status entries, by path. */
int status_cmp(const void *a, const void *b);

/* Parse porcelain v1 -z output (exposed for unit tests). */
size_t parse_status(const char *buf, size_t len, struct file_status **out);

static inline bool fs_unstaged(const struct file_status *f)
{
	return f->unmerged || f->y != ' ';
}

static inline bool fs_staged(const struct file_status *f)
{
	return !f->unmerged && f->x != ' ' && f->x != '?';
}

/* Whether anything is staged (index differs from HEAD). */
bool have_staged(struct repo *r);

#endif
