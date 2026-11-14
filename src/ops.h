/* SPDX-License-Identifier: MIT */
#ifndef CARVE_OPS_H
#define CARVE_OPS_H

#include "git.h"

/*
 * Continue or abort the operation in progress that a rebase drives, a
 * move, fixup-to, defer or squash.
 */
int op_continue(struct repo *r);
int op_abort(struct repo *r);

#endif
