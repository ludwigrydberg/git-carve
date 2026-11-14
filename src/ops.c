// SPDX-License-Identifier: MIT
/* Continue and abort, as the operation in progress does them. */
#include "ops.h"
#include "move.h"
#include "series.h"

int op_continue(struct repo *r)
{
	if (move_in_progress(r))
		return move_continue(r);
	return series_continue(r);
}

int op_abort(struct repo *r)
{
	if (move_in_progress(r))
		return move_abort(r);
	return series_abort(r);
}
