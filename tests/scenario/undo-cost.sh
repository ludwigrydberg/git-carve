#!/bin/sh
# SPDX-License-Identifier: MIT
# The cost of undo on a large index, as on a kernel tree of 96k files.
# Recording must not run git write-tree twice per command when the index
# has not changed since the last entry, which costs 0.17 s each there.
# Undo must keep the stat data of the entries it does not change. A git
# read-tree would drop it all, and the next git status would hash the
# whole tree again, 4.8 s instead of 0.4 s.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'a\nb\nc\n' >f.c
printf 'other\n' >other.c
commit_all base
printf 'A\nb\nC\n' >f.c

git_shim 'case "$1" in write-tree) echo write-tree >>"'"$tmp"'/calls";; esac'
export PATH="$tmp/bin:$PATH"

carve stage f.c o1,n1 >/dev/null || fail "stage 1"
: >"$tmp/calls"
carve stage f.c o3,n3 >/dev/null || fail "stage 2"
[ "$(grep -c write-tree "$tmp/calls")" = 1 ] ||
	fail "write-tree $(grep -c write-tree "$tmp/calls") times, want 1"
# git touched the index in between, back to write-tree, and still right
git update-index --refresh >/dev/null
touch f.c
git update-index -q --refresh
carve unstage f.c o3,n3 >/dev/null || fail unstage
carve undo >/dev/null && git show :f.c | grep -q '^C$' || fail "undo"

# undo keeps the stat data of other.c
before=$(git ls-files --debug other.c)
carve undo >/dev/null || fail "undo 2"
[ "$(git ls-files --debug other.c)" = "$before" ] ||
	fail "stat data of other.c dropped:
$before
$(git ls-files --debug other.c)"
assert_index_text f.c 'A\nb\nc\n'
exit 0
