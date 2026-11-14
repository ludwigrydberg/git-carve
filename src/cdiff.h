/* SPDX-License-Identifier: MIT */
#ifndef CARVE_CDIFF_H
#define CARVE_CDIFF_H

#include "diff.h"
#include "patch.h"

/*
 * The diff of path in one commit, against its parent or the empty tree for
 * a root, with display options or the defaults for NULL. *out is NULL when
 * the commit does not change path. Used by "git carve show" and the commit
 * view.
 */
int load_commit_diff(struct repo *r, const char *commit, const char *path,
		     const struct diff_opts *opts, struct fdiff **out);

/*
 * load_commit_diff for a rename from from to path. A NULL from is
 * load_commit_diff.
 */
int load_commit_diff_from(struct repo *r, const char *commit, const char *path,
			  const char *from, const struct diff_opts *opts,
			  struct fdiff **out);

/* The same for every file the commit changes, from one git diff. */
int load_commit_diff_all(struct repo *r, const char *commit,
			 const struct diff_opts *opts, struct fdiff **out,
			 size_t *n);

/*
 * The patch of the lines sel selects in commit's diff of path.
 * PATCH_UNSTAGE builds it by the unstage rule, and applied in reverse to
 * commit it takes out just those lines. PATCH_STAGE builds it by the stage
 * rule, and applied to the parent it adds just those. Without a selection
 * it is all of it, binary files and modes too. name is what messages call
 * the commit, and *nlines, if not NULL, gets the changed lines selected.
 */
int commit_patch(struct repo *r, const char *commit, const char *name,
		 const char *path, const char *sel, enum patch_dir dir,
		 struct strbuf *patch, int *nlines);

/* Both rules (unstage into rem, stage into add) from one diff. */
int commit_patches(struct repo *r, const char *commit, const char *name,
		   const char *path, const char *sel, struct strbuf *rem,
		   struct strbuf *add);

/* A file a commit changes, its status letter (A, M, D, T) and path. */
struct cfile {
	char status;
	char *path;
	char *from; /* a rename (R): the old path, else NULL */
};

/* The files commit changes. A rename is one entry, R with from and path. */
int commit_files(struct repo *r, const char *commit, struct cfile **out,
		 size_t *n);

void cfiles_free(struct cfile *f, size_t n);

/* The whole commit as text, with id, author, message, stat and diff. */
int commit_show(struct repo *r, const char *commit, struct strbuf *out);

#endif
