#!/bin/sh
# SPDX-License-Identifier: MIT
# Taking lines out of a split commit. Lines discarded during a split of
# HEAD stop being part of what the split must add up to. Finishing accepts
# the original tree or the original minus those lines, and nothing else.
# Undo, abort and a file edited during the split keep working.
. "$(dirname "$0")/../lib.sh"
new_repo

i=1; while [ $i -le 40 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
awk '{ print } $0 == "l 5" { print "fix" } $0 == "l 20" { print "debug" }
     $0 == "l 35" { print "later" }' a.c >a.tmp && mv a.tmp a.c
commit_all "C1: fix, with a debug line and a line for C2"
orig=$(git rev-parse HEAD)
without_debug=$(grep -v '^debug$' a.c | git hash-object --stdin)

# --- discard during a split, and finish without the discarded line ---
carve split >/dev/null || fail "split"
carve discard a.c "n$(new_no a.c debug)" >/dev/null || fail "discard"
carve split --status | grep -q 'remaining diff +2/-0' ||
	fail "remaining: $(carve split --status)"
carve stage-file a.c && carve commit -m C1 >/dev/null || fail "commit"
carve split --finish >/dev/null || fail "finish without the debug line"
[ "$(git rev-parse HEAD:a.c)" = "$without_debug" ] || fail "tree"
[ "$(git rev-parse HEAD~1)" = "$(git rev-parse "$orig~1")" ] || fail "base"
git reset -q --hard "$orig"

# --- a discard undone makes the line part of the split again ---
carve split >/dev/null || fail "split 2"
carve discard a.c "n$(new_no a.c debug)" >/dev/null || fail "discard 2"
carve undo >/dev/null || fail "undo the discard"
grep -qx debug a.c || fail "undo did not bring the line back"
carve stage-file a.c && carve commit -m C1 >/dev/null || fail "commit 2"
carve split --finish >/dev/null || fail "finish with the line"
[ "$(git rev-parse HEAD^{tree})" = "$(git rev-parse "$orig^{tree}")" ] ||
	fail "tree 2"
git reset -q --hard "$orig"

# --- a discard that leaves something else behind still does not finish ---
carve split >/dev/null || fail "split 3"
carve discard a.c "n$(new_no a.c debug)" >/dev/null || fail "discard 3"
carve stage a.c "n$(new_no a.c fix)" && carve commit -m part >/dev/null ||
	fail "commit 3"
must_fail carve split --finish
grep -q 'split not finished.*+1/-0' "$tmp/must_fail.out" ||
	fail "unfinished: $(cat "$tmp/must_fail.out")"

# --- abort after a discard restores the commit exactly (I8) ---
carve split --abort >/dev/null || fail "abort: $(cat "$tmp/must_fail.out")"
[ "$(git rev-parse HEAD)" = "$orig" ] || fail "abort HEAD"
[ "$(git write-tree)" = "$(git rev-parse "$orig^{tree}")" ] || fail "index"
[ "$(git hash-object a.c)" = "$(git rev-parse "$orig:a.c")" ] ||
	fail "abort work tree"

# --- in a file edited during the split the lines are not the commit's ---
carve split >/dev/null || fail "split 4"
echo mine >>a.c
must_fail carve discard a.c "n$(new_no a.c debug)"
grep -q 'a.c was edited during the split' "$tmp/must_fail.out" ||
	fail "edited: $(cat "$tmp/must_fail.out")"
grep -qx debug a.c || fail "refused discard changed the file"
carve split --abort --force >/dev/null || fail "abort 4"

# --- not while splitting an older commit, whose descendants keep trees ---
echo top >top.c
commit_all "C2: top"
carve split HEAD~1 >/dev/null || fail "split older"
must_fail carve discard a.c "n$(new_no a.c debug)"
grep -q 'only while splitting HEAD' "$tmp/must_fail.out" ||
	fail "older: $(cat "$tmp/must_fail.out")"
carve split --abort >/dev/null || fail "abort older"
git reset -q --hard "$orig"
exit 0
