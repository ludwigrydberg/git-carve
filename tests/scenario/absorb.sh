#!/bin/sh
# SPDX-License-Identifier: MIT
# absorb puts each unstaged change block into the commit of the series that
# last touched its lines. Blocks without one clear target stay and are
# listed. It is a dry run by default, the work tree is unchanged, and one
# undo entry takes it all back.
. "$(dirname "$0")/../lib.sh"
new_repo

i=1; while [ $i -le 60 ]; do echo "l $i"; i=$((i + 1)); done >a.c
printf 'b\n' >b.c
i=1; while [ $i -le 10 ]; do echo "c $i"; i=$((i + 1)); done >c.c
commit_all base
git branch -q upstream
git branch -q --set-upstream-to=upstream
edit "l 5" "l 5 C1"; edit "l 10" "l 10 C1"
commit_all C1
edit "l 20" "l 20 C2"; edit "l 21" "l 21 C2"
sed 's/^c 5$/c 5 C2/' c.c >c.tmp && mv c.tmp c.c
commit_all C2
edit "l 40" "l 40 C3"; edit "l 41" "l 41 C3"
commit_all C3
tip=$(git rev-parse HEAD)

# the fixes, one each for C1 and C2, one between two C3 lines, one on a
# line of the base, and one spanning C1's and a base line
edit "l 5 C1" "l 5 C1 fixed"
edit "l 20 C2" "l 20 C2 fixed"
awk '{ print } $0 == "l 40 C3" { print "l 40.5 new" }' a.c >a.tmp && mv a.tmp a.c
edit "l 50" "l 50 changed"
edit "l 10 C1" "l 10 C1 fixed"; edit "l 11" "l 11 fixed"
printf 'b changed\n' >b.c
# a second file with a block for C2, and one patch for both files
sed 's/^c 5 C2$/c 5 C2 fixed/' c.c >c.tmp && mv c.tmp c.c
wt=$(worktree_state)

carve absorb >"$tmp/plan" || fail "dry run"
cat "$tmp/plan" >"$tmp/plan.txt"
grep -q '^a.c:5 -> [0-9a-f]* C1$' "$tmp/plan" || fail "C1: $(cat "$tmp/plan")"
grep -q '^a.c:20 -> [0-9a-f]* C2$' "$tmp/plan" || fail "C2"
grep -q '^c.c:5 -> [0-9a-f]* C2$' "$tmp/plan" || fail "c.c: $(cat "$tmp/plan")"
grep -q '^a.c:41 -> [0-9a-f]* C3$' "$tmp/plan" || fail "insertion"
grep -q '^a.c:51 stays: its lines come from [0-9a-f]*, outside the series$' \
	"$tmp/plan" || fail "base line"
grep -q 'stays: its lines come from 2 commits$' "$tmp/plan" || fail "mixed"
grep -q '^b.c:1 stays: its lines come from' "$tmp/plan" || fail "b.c"
grep -q '^(a dry run: --apply moves them)$' "$tmp/plan" || fail "dry"
[ "$(git rev-parse HEAD)" = "$tip" ] || fail "the dry run changed HEAD"

carve absorb --apply >"$tmp/out" || fail "apply: $(cat "$tmp/out")"
grep -q '^absorbed 4 changes$' "$tmp/out" || fail "count: $(cat "$tmp/out")"
[ "$(git log --format=%s | tr '\n' ' ')" = "C3 C2 C1 base " ] ||
	fail "commits: $(git log --format=%s)"
git show HEAD~2 | grep -q '^+l 5 C1 fixed$' || fail "C1 not fixed"
git show HEAD~1 | grep -q '^+l 20 C2 fixed$' || fail "C2 not fixed"
git show HEAD~1 | grep -q '^+c 5 C2 fixed$' || fail "c.c not fixed in C2"
git show HEAD | grep -q '^+l 40.5 new$' || fail "C3 not fixed"
[ "$(worktree_state)" = "$wt" ] || fail "work tree changed"
git diff | grep -q '^+l 50 changed$' || fail "base-line change lost"
git diff | grep -q '^+l 11 fixed$' || fail "mixed change lost"
[ -z "$(git diff --cached)" ] || fail "left staged"

# one undo entry takes it back
carve undo >/dev/null || fail undo
[ "$(git rev-parse HEAD)" = "$tip" ] || fail "undo: HEAD"

# staged changes are refused
carve stage-file b.c
must_fail carve absorb
grep -q 'staged changes' "$tmp/must_fail.out" || fail "staged"
carve unstage-file b.c
# without an upstream or a remote branch the newest commits are the series
git branch -q --unset-upstream
carve absorb | grep -q ' C2$' || fail "no upstream: $(carve absorb)"
# without an upstream the series is what no remote branch has, C3 only
git update-ref refs/remotes/origin/main HEAD~1
carve absorb | grep -q ' C2$' && fail "into a pushed commit"
carve absorb HEAD~3..HEAD | grep -q 'C2$' || fail "explicit range"
git update-ref -d refs/remotes/origin/main

# --- several targets in one rebase with a later commit in the way. It
# stops and says why, and abort puts everything back.
new_repo conflict
i=1; while [ $i -le 30 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
git branch -q upstream
git branch -q --set-upstream-to=upstream
edit "l 3" "l 3 T1"
commit_all T1
awk '{ print } $0 == "l 10" { print "int helper(void)"; print "{";
	print "	retrun 1;"; print "}" }' a.c >a.tmp && mv a.tmp a.c
commit_all "T2: helper"
edit "}" "} /* helper */"
commit_all "T3: near"
tip2=$(git rev-parse HEAD)
edit "l 3 T1" "l 3 T1 fixed"
awk '{ sub(/retrun/, "return"); print }' a.c >a.tmp && mv a.tmp a.c
carve absorb | grep -c ' -> ' | grep -q '^2$' || fail "two targets: $(carve absorb)"
wt=$(worktree_state)
must_fail carve absorb --apply
carve status | grep -q 'FIXUP-TO' || fail "no operation in progress: $(carve status)"
carve abort >/dev/null || fail "abort"
[ "$(git rev-parse HEAD)" = "$tip2" ] || fail "abort: HEAD"
[ "$(worktree_state)" = "$wt" ] || fail "abort: work tree"
[ -z "$(git diff --cached)" ] || fail "abort: left staged"
exit 0
