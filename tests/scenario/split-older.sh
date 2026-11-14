#!/bin/sh
# SPDX-License-Identifier: MIT
# Split a commit in the middle of a series with 3 descendants, which replay
# with identical trees (I7). Abort restores the tip.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'x\n' >base.c
commit_all C1
printf 'a\n' >a.c
printf 'b\n' >b.c
commit_all "C2: add a and b"
for i in 3 4 5; do
	printf 'c%s\n' "$i" >"c$i.c"
	printf 'more %s\n' "$i" >>a.c
	commit_all "C$i"
done
tip=$(git rev-parse HEAD)
c2=$(git rev-parse HEAD~3)
c2_tree=$(tree_of "$c2")
old_trees=$(git log --format=%T "$c2..HEAD")

carve split HEAD~3 >/dev/null || fail "split older"
git rev-parse -q --verify refs/worktree/carve/orig >/dev/null || fail "no orig ref"
[ "$(git rev-parse refs/worktree/carve/orig)" = "$c2" ] || fail "orig is C2"
[ "$(git rev-parse HEAD)" = "$(git rev-parse "$c2~1")" ] || fail "at C1"
carve split --status | grep -q 'older commit' || fail "status older"
carve status | grep -q '^SPLIT' || fail "carve status"

carve stage-file a.c
carve commit -m "C2a: add a" >/dev/null || fail "commit a"
carve stage-file b.c
carve commit -m "C2b: add b" >/dev/null || fail "commit b"
carve split --finish >"$tmp/out" || fail "finish: $(cat "$tmp/out")"
grep -q 'split finished: 2 commits' "$tmp/out" || fail "finish output"

# the series now has C1, C2a, C2b, C3, C4, C5 and the same final tree
[ "$(git rev-list --count HEAD)" = 6 ] || fail "count"
[ "$(tree_of HEAD)" = "$(tree_of "$tip")" ] || fail "tip tree"
[ "$(tree_of HEAD~3)" = "$c2_tree" ] || fail "C2b tree = C2 tree (I6)"
[ "$(git log --format=%T HEAD~3..HEAD)" = "$old_trees" ] ||
	fail "I7: descendant trees changed"
[ "$(git log -1 --format=%s HEAD~4)" = "C2a: add a" ] || fail "C2a"
[ "$(git symbolic-ref HEAD)" = refs/heads/main ] || fail "branch"
[ -z "$(short_status)" ] || fail "status after"

# abort goes back to the original tip, branch and all
new_tip=$(git rev-parse HEAD)
carve split HEAD~2 >/dev/null || fail "split older 2"
carve stage-file c3.c
carve commit -m partial >/dev/null
carve split --abort >/dev/null || fail "abort older"
[ "$(git rev-parse HEAD)" = "$new_tip" ] || fail "abort HEAD"
[ "$(git symbolic-ref HEAD)" = refs/heads/main ] || fail "abort branch"
[ -z "$(short_status)" ] || fail "abort status"

# refused when not an ancestor, with merges above, and with a dirty tree
git checkout -q -b side HEAD~1
printf 's\n' >s.c
commit_all side
must_fail carve split main
grep -q 'not an ancestor' "$tmp/must_fail.out" || fail "ancestor message"
git checkout -q main
printf 'dirty\n' >>a.c
must_fail carve split HEAD~1
git checkout -q a.c
# a merge, neither as the commit nor above it
below=$(git rev-parse HEAD~1)
git merge -q --no-ff --no-edit side
must_fail carve split HEAD
grep -q 'is a merge commit' "$tmp/must_fail.out" ||
	fail "merge message: $(cat "$tmp/must_fail.out")"
must_fail carve split "$below"
grep -q 'merges between' "$tmp/must_fail.out" ||
	fail "merges message: $(cat "$tmp/must_fail.out")"
exit 0
