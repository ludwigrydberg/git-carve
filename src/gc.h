/* SPDX-License-Identifier: MIT */
/*
 * git carve gc removes only what is provably dead. That is state files
 * without an operation, temporary indexes and check worktrees whose
 * process is gone, undo entries beyond carve.undoLimit, and the steps of
 * an operation that ended without git carve noticing, which are folded. A
 * work-tree snapshot or parked changes left behind may be the only copy of
 * someone's work. They are listed with how to get them back and never
 * removed. Nothing changes unless act is set.
 */
#ifndef CARVE_GC_H
#define CARVE_GC_H

#include <stdbool.h>

#include "git.h"

int carve_gc(struct repo *r, bool act, struct strbuf *out);
/*
 * Something an interrupted operation left behind that carve_gc removes,
 * which the TUI offers to clean up at startup. Only files are looked at,
 * and the split's ref when there are state files.
 */
bool gc_leftovers(struct repo *r);
/*
 * A snapshot of the work tree an interrupted move saved. It is never
 * removed, since it may be the only copy of someone's work.
 */
bool gc_snapshot_kept(struct repo *r);

#endif
