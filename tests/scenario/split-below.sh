#!/bin/sh
# SPDX-License-Identifier: MIT
# While splitting HEAD, a part of it that belongs in the commit below goes
# there with fixup-to HEAD, before the first commit of the split. The split
# still has to add up to the original tree.
. "$(dirname "$0")/../lib.sh"
new_repo

i=1; while [ $i -le 20 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
awk '{ if ($0 == "l 5") $0 = "l 5 P2"; print }' a.c >a.tmp && mv a.tmp a.c
commit_all P2
awk '{ if ($0 == "l 15") $0 = "l 15 P3"; if ($0 == "l 5 P2") $0 = "l 5 P2 fixed"
       print }' a.c >a.tmp && mv a.tmp a.c
commit_all P3
orig=$(git rev-parse HEAD)

carve split >/dev/null || fail split
must_fail carve fixup-to HEAD~1 a.c o15,n15
grep -q 'only go into the commit below the split' "$tmp/must_fail.out" ||
	fail "older target: $(cat "$tmp/must_fail.out")"
[ -z "$(git diff --cached)" ] || fail "selection left staged"
carve fixup-to HEAD a.c o5,n5 >/dev/null || fail "fixup-to HEAD"
git show HEAD | grep -q '^+l 5 P2 fixed$' || fail "not in P2"
[ "$(git log -1 --format=%s)" = P2 ] || fail "P2 message"
carve split --status | grep -q '0 commits made' || fail "status"
carve stage-file a.c && carve commit -m P3 >/dev/null
carve split --status | grep -q '1 commit made' || fail "count"
# after the first commit it would amend a split commit, and is refused
printf 'x\n' >>a.c && carve stage-file a.c
must_fail carve fixup-to --cached HEAD
grep -q 'commits were made in this split already' "$tmp/must_fail.out" ||
	fail "after a commit: $(cat "$tmp/must_fail.out")"
git reset -q && git checkout -q -- a.c
carve split --finish >/dev/null || fail finish
[ "$(git rev-parse HEAD^{tree})" = "$(git rev-parse "$orig^{tree}")" ] ||
	fail "tree"
git show HEAD | grep -q '^+l 15 P3$' || fail "P3 kept its change"
git show HEAD | grep -q 'P2 fixed' && fail "P3 still has the P2 part"

# abort after moving puts everything back
carve split >/dev/null
carve fixup-to HEAD a.c o15,n15 >/dev/null
carve split --abort >/dev/null || fail abort
[ "$(git rev-parse HEAD^{tree})" = "$(git rev-parse "$orig^{tree}")" ] ||
	fail "abort tree"
exit 0
