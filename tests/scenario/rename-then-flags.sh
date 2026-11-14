#!/bin/sh
# SPDX-License-Identifier: MIT
# The work tree has bar(dev, FLAG_X) where HEAD has
# foo(dev). Build the series "rename foo() to bar()" + "add flags
# argument", where the intermediate bar(dev) never existed on disk.
. "$(dirname "$0")/../lib.sh"
new_repo

cat >drv.c <<'X'
#include "drv.h"

static int foo(struct device *dev)
{
	return dev->id;
}

int drv_probe(struct device *dev)
{
	return foo(dev);
}

void drv_remove(struct device *dev)
{
	foo(dev);
}
X
commit_all "drv: initial"

cat >drv.c <<'X'
#include "drv.h"

static int bar(struct device *dev, unsigned int flags)
{
	return dev->id | flags;
}

int drv_probe(struct device *dev)
{
	return bar(dev, FLAG_X);
}

void drv_remove(struct device *dev)
{
	bar(dev, FLAG_X);
}
X
final=$(file_hash drv.c)

# commit 1, the rename only. Edit each changed line as it will be staged.
o=$(old_no drv.c 'static int foo')
n=$(new_no drv.c 'static int bar')
printf 'static int bar(struct device *dev)\n' |
	carve set-staged drv.c "o$o,n$n" - || fail "definition"
o=$(old_no drv.c 'return foo(dev)')
n=$(new_no drv.c 'return bar(dev, FLAG_X)')
printf '\treturn bar(dev);\n' | carve set-staged drv.c "o$o,n$n" - ||
	fail "probe"
o=$(old_no drv.c '	foo(dev);')
n=$(new_no drv.c '	bar(dev, FLAG_X);')
printf '\tbar(dev);\n' | carve set-staged drv.c "o$o,n$n" - ||
	fail "remove"

cat >"$tmp/step1" <<'X'
#include "drv.h"

static int bar(struct device *dev)
{
	return dev->id;
}

int drv_probe(struct device *dev)
{
	return bar(dev);
}

void drv_remove(struct device *dev)
{
	bar(dev);
}
X
assert_index drv.c "$tmp/step1"
[ "$(file_hash drv.c)" = "$final" ] || fail "work tree changed"

# the unstaged diff now shows exactly the flags change
carve diff drv.c | grep -q '^ *[0-9]* *-	return bar(dev);$' ||
	fail "unstaged diff old side"
carve diff drv.c | grep -q '+	return bar(dev, FLAG_X);$' ||
	fail "unstaged diff new side"
carve diff drv.c | grep -q 'foo' && fail "foo still in the unstaged diff"
git commit -q -m "drv: rename foo() to bar()" || fail "commit 1"

# commit 2, everything else
carve stage-file drv.c || fail "stage rest"
git commit -q -m "drv: add flags argument to bar()" || fail "commit 2"
[ "$(git rev-parse HEAD:drv.c)" = "$final" ] || fail "final content"
[ "$(git rev-parse HEAD~1:drv.c)" = "$(file_hash "$tmp/step1")" ] ||
	fail "first commit content"
