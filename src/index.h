/* SPDX-License-Identifier: MIT */
#ifndef CARVE_INDEX_H
#define CARVE_INDEX_H

#include "diff.h"

/*
 * A run of index lines and the text that is to replace it. Lines count
 * from 1, and count may be 0 for an insertion before line start.
 */
struct region {
	int start;
	int count;
	struct strbuf text;
};

/*
 * The region covered by the selected rows of fd (dline.sel, context rows
 * included), given the current index blob. For an unstaged diff the text
 * is what the region will look like once the selection is staged. For a
 * staged diff it is the region as staged now. Returns 0, or -1 with a
 * message in *err when nothing is selected.
 */
int staged_region(const struct fdiff *fd, bool cached, const char *blob,
		  size_t bloblen, struct region *rg, struct strbuf *err);
void region_release(struct region *rg);

/* Replace lines [start, start + count) of blob with text. */
void splice_lines(const char *blob, size_t bloblen, int start, int count,
		  const char *text, size_t len, struct strbuf *out);

/*
 * Select spec in the unstaged or staged diff of path and replace that
 * region of the index blob with content (set-staged). With content NULL,
 * only compute the region's text into *show.
 */
int set_staged(struct repo *r, const char *path, const char *spec, bool cached,
	       const char *content, size_t len, struct strbuf *show);

/*
 * Which of the paths are in the index, from one git ls-files. It names the
 * paths when they are few and lists the whole index otherwise, which takes
 * 20 ms on a kernel. Paths are looked up by binary search, since ls-files
 * lists in index order, which is strcmp order. in[i] is set for paths[i].
 * Returns -1 when git failed, with in[] untouched.
 */
int paths_in_index(struct repo *r, const char *const *paths, size_t n,
		   bool *in);

#endif
