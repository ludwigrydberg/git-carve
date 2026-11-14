#!/bin/sh
# SPDX-License-Identifier: MIT
# Aborting a split restores HEAD, index and work tree exactly (I8), and
# refuses to throw away changes that are not in the original commit. With
# nothing changed it puts back the index saved at the start and writes no
# file.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'keep\n' >k.c
commit_all base
printf 'x1\nx2\nx3\n' >x.c
printf 'keep\nmore\n' >k.c
commit_all "fat"
orig=$(git rev-parse HEAD)
orig_tree=$(tree_of HEAD)
state=$(worktree_state)
printf 'untracked\n' >notes.txt

check_restored() {
	[ "$(git rev-parse HEAD)" = "$orig" ] || fail "$1: HEAD"
	[ "$(git write-tree)" = "$orig_tree" ] || fail "$1: index"
	[ "$(worktree_state)" = "$state" ] || fail "$1: work tree"
	[ "$(short_status)" = "?? notes.txt" ] ||
		fail "$1: status $(short_status)"
	[ "$(cat notes.txt)" = untracked ] || fail "$1: untracked file"
	git rev-parse -q --verify refs/worktree/carve/orig >/dev/null &&
		fail "$1: ref left"
	[ -d .git/carve ] && fail "$1: state left"
	return 0
}

# abort after partial staging and a commit
carve split >/dev/null || fail "split"
carve stage x.c n2 || fail "stage"
carve commit -m "part" >/dev/null || fail "commit"
carve stage k.c n2 || fail "stage 2"
# with nothing changed, the index saved at the start goes back, and git
# writes no file of the work tree (no reset --hard)
env GIT_CARVE_TRACE="$tmp/trace" "$CARVE_CLI" split --abort >/dev/null ||
	fail "abort"
grep -q 'reset -q --hard' "$tmp/trace" && fail "abort rewrote the work tree"
check_restored "abort after commit"

# a file touched but not changed takes the same way
carve split >/dev/null || fail "split touched"
touch -t 202001010000 k.c x.c
rm -f "$tmp/trace"
env GIT_CARVE_TRACE="$tmp/trace" "$CARVE_CLI" split --abort >/dev/null ||
	fail "abort touched"
grep -q 'reset -q --hard' "$tmp/trace" && fail "touched: rewrote"
check_restored "abort of touched files"

# without the saved index the abort takes the long way, with the same end
carve split >/dev/null || fail "split no copy"
rm .git/carve/abort-index
carve split --abort >/dev/null || fail "abort without the copy"
check_restored "abort without the saved index"

# an unrelated edit to a file blocks the abort
carve split >/dev/null || fail "split 2"
printf 'keep\nmore\nmine\n' >k.c
must_fail carve split --abort
grep -q 'changes that are not in' "$tmp/must_fail.out" || fail "message"
grep -q 'k.c' "$tmp/must_fail.out" || fail "path in message"
git rev-parse -q --verify refs/worktree/carve/orig >/dev/null || fail "ref gone"
carve split --abort --force >/dev/null || fail "abort --force"
check_restored "forced abort"

# a new file added to the index also blocks it
carve split >/dev/null || fail "split 3"
printf 'n\n' >new.c
git add new.c
must_fail carve split --abort
grep -q 'new.c' "$tmp/must_fail.out" || fail "new file message"
git rm -q --cached new.c
rm new.c
# work tree content equal to the original is fine (x.c is untracked now)
carve split --abort >/dev/null || fail "abort 3"
check_restored "abort with orig files untracked"

# a commit that deletes a file, whose parent's copy is back in the index
# but not on disk once the split starts, and aborting must still work
git rm -q k.c
git commit -q -m "remove k"
orig=$(git rev-parse HEAD)
orig_tree=$(tree_of HEAD)
state=$(worktree_state)
carve split >/dev/null || fail "split deletion"
[ "$(short_status | grep k.c)" = " D k.c" ] || fail "deletion unstaged"
carve split --abort >/dev/null || fail "abort deletion"
check_restored "abort of a deletion"
# but recreating the deleted file blocks it
carve split >/dev/null || fail "split deletion 2"
printf 'mine\n' >k.c
must_fail carve split --abort
grep -q k.c "$tmp/must_fail.out" || fail "recreated file message"
rm k.c
carve split --abort >/dev/null || fail "abort deletion 2"
exit 0
