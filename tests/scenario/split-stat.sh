#!/bin/sh
# SPDX-License-Identifier: MIT
# Starting a split moves HEAD back and gives the index HEAD's entries with
# reset --soft and then restore --staged, without an index update per
# entry, with the same result as reset --mixed. That holds also when files
# are stat-dirty, only touched as after a checkout on a file system that
# ignores case, and when run from a subdirectory.
. "$(dirname "$0")/../lib.sh"
new_repo

mkdir sub
printf 'a\n' >sub/a.c
printf 'b\n' >b.c
printf 'c\n' >c.c
commit_all base
printf 'a2\n' >sub/a.c
printf 'n\n' >sub/new.c
git rm -q c.c
commit_all "drv: change"
orig=$(git rev-parse HEAD)
# same content, new stat data
touch b.c
sleep 1
touch b.c

cd sub || fail cd
carve split >/dev/null || fail "split"
cd .. || fail cd
[ "$(git rev-parse HEAD)" = "$(git rev-parse "$orig~1")" ] || fail "HEAD"
[ "$(git write-tree)" = "$(tree_of HEAD)" ] || fail "index not HEAD's"
# the commit's changes are unstaged, the work tree untouched
[ "$(git diff --name-only | tr '\n' ' ')" = "c.c sub/a.c " ] ||
	fail "unstaged: $(git diff --name-only)"
git status --porcelain | grep -q '^?? sub/new.c$' || fail "new file: $(git status --porcelain)"
[ "$(cat sub/a.c)" = a2 ] || fail "work tree"
[ ! -e c.c ] || fail "deleted file back"
git status --porcelain | grep -q 'b\.c' && fail "touched file listed"
carve split --abort >/dev/null || fail "abort"
[ "$(git rev-parse HEAD)" = "$orig" ] || fail "abort: HEAD"
exit 0
