#!/bin/sh
# SPDX-License-Identifier: MIT
# The work tree holds the final state (new function, all
# callers converted, old function gone) and git's diff turns the old
# function into the new one. Carve it into:
#   1. add ctx_set_flags()        (old API and callers untouched)
#   2. convert subsystem A
#   3. convert subsystem B
#   4. remove ctx_set()
# using only line staging and set-staged.
. "$(dirname "$0")/../lib.sh"
new_repo

cat >ctx.c <<'X'
// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include "ctx.h"

int ctx_set(struct ctx *c, int val)
{
	int ret;

	ret = ctx_check(c);
	if (ret)
		return ret;

	c->val = val;
	return 0;
}

/* subsystem A */
static int a_init(struct ctx *c)
{
	return ctx_set(c, 1);
}

static void a_reset(struct ctx *c)
{
	ctx_set(c, 0);
}

/* subsystem B */
static int b_setup(struct ctx *c, int v)
{
	int ret = ctx_set(c, v);

	if (ret)
		pr_err("b: setup failed\n");
	return ret;
}
X
commit_all "ctx: initial"
cp ctx.c "$tmp/orig"

cat >ctx.c <<'X'
// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include "ctx.h"

int ctx_set_flags(struct ctx *c, int val, unsigned long flags)
{
	int ret;

	ret = ctx_check(c);
	if (ret)
		return ret;

	c->val = val;
	c->flags = flags;
	return 0;
}

/* subsystem A */
static int a_init(struct ctx *c)
{
	return ctx_set_flags(c, 1, 0);
}

static void a_reset(struct ctx *c)
{
	ctx_set_flags(c, 0, CTX_RESET);
}

/* subsystem B */
static int b_setup(struct ctx *c, int v)
{
	int ret = ctx_set_flags(c, v, 0);

	if (ret)
		pr_err("b: setup failed\n");
	return ret;
}
X
final=$(file_hash ctx.c)

# git shows the old signature turning into the new one, a mixed hunk
o=$(old_no ctx.c 'int ctx_set(struct ctx *c, int val)')
n=$(new_no ctx.c 'int ctx_set_flags(struct ctx')
[ "$o" = 5 ] && [ "$n" = 5 ] || fail "unexpected diff shape ($o/$n)"

# 1. add the new function in front of the old one, by editing the signature
# pair "as it will be staged" into the new function and the old signature
cat >"$tmp/edit" <<'X'
int ctx_set_flags(struct ctx *c, int val, unsigned long flags)
{
	int ret;

	ret = ctx_check(c);
	if (ret)
		return ret;

	c->val = val;
	c->flags = flags;
	return 0;
}

int ctx_set(struct ctx *c, int val)
X
carve set-staged ctx.c "o$o,n$n" "$tmp/edit" || fail "set-staged"
{
	sed -n '1,4p' "$tmp/orig"
	cat "$tmp/edit"
	sed -n '6,$p' "$tmp/orig"
} >"$tmp/step1"
assert_index ctx.c "$tmp/step1"
git commit -q -m "ctx: add ctx_set_flags()" || fail "commit 1"

# the remaining diff is the callers and the removal of the old function
# only
carve diff ctx.c >"$tmp/d"
grep -q '^ *[0-9]* *-int ctx_set(struct ctx \*c, int val)$' "$tmp/d" ||
	fail "old function removal not shown"
grep -q '^ *[0-9]* *+int ctx_set_flags' "$tmp/d" &&
	fail "new function should be context now"

# 2. convert subsystem A, both calls, each a - and + pair
for pat in 'return ctx_set(c, 1);' 'ctx_set(c, 0);'; do
	o=$(old_no ctx.c "$pat")
	[ -n "$o" ] || fail "no old line for $pat"
	carve stage ctx.c "o$o" || fail "stage o$o"
done
for pat in 'ctx_set_flags(c, 1, 0);' 'ctx_set_flags(c, 0, CTX_RESET);'; do
	n=$(new_no ctx.c "$pat")
	[ -n "$n" ] || fail "no new line for $pat"
	carve stage ctx.c "n$n" || fail "stage n$n"
done
sed -e 's/return ctx_set(c, 1);/return ctx_set_flags(c, 1, 0);/' \
	-e 's/	ctx_set(c, 0);/	ctx_set_flags(c, 0, CTX_RESET);/' \
	"$tmp/step1" >"$tmp/step2"
assert_index ctx.c "$tmp/step2"
git commit -q -m "a: convert to ctx_set_flags()" || fail "commit 2"

# 3. convert subsystem B with one range over the -/+ pair
n=$(new_no ctx.c 'int ret = ctx_set_flags(c, v, 0);')
carve stage ctx.c "n$n-$n" || fail "stage B"
sed -e 's/int ret = ctx_set(c, v);/int ret = ctx_set_flags(c, v, 0);/' \
	"$tmp/step2" >"$tmp/step3"
assert_index ctx.c "$tmp/step3"
git commit -q -m "b: convert to ctx_set_flags()" || fail "commit 3"

# 4. what is left is exactly the removal of the old function
carve diff ctx.c | grep -E '^ {6}[ 0-9]{5} \+' && fail "additions left over"
carve stage-file ctx.c || fail "stage removal"
git commit -q -m "ctx: remove ctx_set()" || fail "commit 4"
[ "$(git rev-parse HEAD:ctx.c)" = "$final" ] || fail "final tree"
[ "$(git rev-list --count HEAD)" = 5 ] || fail "commit count"

# every intermediate state keeps the old API until the last commit
for c in HEAD~3 HEAD~2 HEAD~1; do
	git show "$c:ctx.c" | grep -q '^int ctx_set(struct ctx' ||
		fail "$c lost ctx_set()"
	git show "$c:ctx.c" | grep -q '^int ctx_set_flags(struct ctx' ||
		fail "$c lacks ctx_set_flags()"
done
git show HEAD:ctx.c | grep -q '^int ctx_set(struct ctx' && fail "not removed"
exit 0
