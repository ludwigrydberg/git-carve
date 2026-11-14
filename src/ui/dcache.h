/* SPDX-License-Identifier: MIT */
#ifndef CARVE_UI_DCACHE_H
#define CARVE_UI_DCACHE_H

#include "ui_int.h"

/* The diff cache (dcache.c): a file's diffs without git, while valid. */
bool dcache_get(const char *path, bool cached, struct fdiff **out);
/* A command of ours changed path. The rest of the cache still holds. */
void dcache_forget(const char *path);
/* A diff of path loaded with git, for the next time while it holds. */
void dcache_put(const char *path, bool cached, const struct fdiff *fd,
		time_t since);
/* The cache holds for the index file as it is now. */
bool dcache_holds(void);
bool dcache_each(bool cached, void (*fn)(const struct fdiff *, void *),
		 void *ctx);
/* After undo's write-tree for a command dcache_forget vouched for. */
void dcache_restamp(void);
void dcache_drop(void);
/*
 * The cache fills in the background, one git diff a side. While the UI
 * is idle and it is wanted, dcache_idle starts or pumps that git.
 */
bool dcache_busy(void);
bool dcache_wanted(void);
void dcache_idle(void);

#endif
