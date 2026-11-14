/* SPDX-License-Identifier: MIT */
/*
 * Conflicts left in a work-tree file by a merge, rebase or cherry-pick.
 * Find the marked regions, and resolve one or all of them by taking our or
 * their side.
 */
#ifndef CARVE_CONFLICT_H
#define CARVE_CONFLICT_H

#include <stdbool.h>

#include "git.h"

/*
 * Byte ranges in the file. base is empty unless the style is diff3 or
 * zdiff3.
 */
struct conflict {
	size_t start, end; /* "<<<<<<<" line .. after ">>>>>>>" */
	size_t ours, ours_end;
	size_t base, base_end;
	size_t theirs, theirs_end;
	int line; /* 1-based line of "<<<<<<<" */
	char *ours_label, *theirs_label;
};

/* The conflicts in buf, in order. Unbalanced markers are ignored. */
size_t conflicts_parse(const char *buf, size_t len, struct conflict **out);
void conflicts_free(struct conflict *c, size_t n);

/*
 * Resolve conflict n of path, counting from 1, by taking one side. n == 0
 * takes that side of them all with git checkout --ours or --theirs. When
 * no conflict is left the file is staged, which ends its unmerged state.
 */
int resolve_conflict(struct repo *r, const char *path, int n, bool theirs);

#endif
