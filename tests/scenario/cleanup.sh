#!/bin/sh
# SPDX-License-Identifier: MIT
# Cleanup. git carve gc removes only dead material, after a dry run, also
# refs it no longer uses, and keeps what may hold work (parked changes).
# Everything git carve keeps survives git gc --prune=now, git clean -fdx
# and git worktree prune in the middle of operations.
. "$(dirname "$0")/../lib.sh"
new_repo

hostile() {
	git gc -q --prune=now || fail "git gc"
	git clean -q -fdx || fail "git clean"
	git worktree prune || fail "worktree prune"
	carve gc --force >/dev/null || fail "carve gc"
}

i=1; while [ $i -le 30 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
# --- gc, a dry run, then only the dead ----------------------------------
carve gc | grep -q '^nothing to clean up$' || fail "clean repo"
mkdir -p .git/carve && echo x >.git/carve/leftover
touch .git/index.carve-999999999-abcdef
git update-ref refs/worktree/carve/worktree HEAD
carve gc >"$tmp/out"
grep -q '^would remove state file carve/leftover' "$tmp/out" || fail "state"
grep -q '^would remove temporary index .*index.carve-999999999-abcdef' \
	"$tmp/out" || fail "index: $(cat "$tmp/out")"
grep -q '^kept refs/worktree/carve/worktree: a work-tree snapshot' "$tmp/out" ||
	fail "snapshot not kept"
grep -q '^(a dry run: --force does it)$' "$tmp/out" || fail "dry"
[ -e .git/carve/leftover ] || fail "the dry run removed something"
carve gc --force >/dev/null
[ -e .git/carve/leftover ] && fail "state not removed"
[ -e .git/index.carve-999999999-abcdef ] && fail "index not removed"
git rev-parse -q --verify refs/worktree/carve/worktree >/dev/null || fail "snapshot gone"
git update-ref -d refs/worktree/carve/worktree
# a live process's temporary index stays
touch ".git/index.carve-$$-abcdef"
carve gc | grep -q "index.carve-$$" && fail "a live owner's index"
rm ".git/index.carve-$$-abcdef"
# undo entries beyond a lowered limit
for n in 1 2 3; do edit "l $n" "l $n x"; carve stage-file a.c; done
git config carve.undoLimit 1
carve gc | grep -c '^would remove undo entry' | grep -q '^2$' || fail "undo"
carve gc --force >/dev/null
[ "$(carve undo --list | wc -l | tr -d ' ')" = 1 ] || fail "undo trimmed"
git config --unset carve.undoLimit
git reset -q --hard

# --- a split survives, and gc keeps its state ----------------------------
edit "l 5" "l 5 A"; edit "l 25" "l 25 B"
commit_all big
big=$(git rev-parse HEAD)
carve split >/dev/null || fail split
carve stage a.c o5,n5 && carve commit -m A >/dev/null
hostile
carve split --status | grep -q '^SPLIT' || fail "split state lost"
carve stage-file a.c && carve commit -m B >/dev/null
carve split --finish >/dev/null || fail "finish after gc"
[ "$(git rev-parse HEAD^{tree})" = "$(git rev-parse "$big^{tree}")" ] ||
	fail "split tree"

# --- a fixup-to stopped on a conflict survives -------------------------
printf 'untracked\n' >notes.txt
git reset -q --hard
edit "l 10" "l 10 C1"
commit_all C1
edit "l 11" "l 11 C2"
commit_all C2
edit "l 10 C1" "l 10 C1 fixed"
printf 'scratch\n' >scratch.c	# untracked: must survive our use of clean?
carve fixup-to HEAD~1 a.c o10,n10 >/dev/null 2>&1 && fail "no conflict"
hostile
carve status | grep -q '^FIXUP-TO' || fail "fixup state lost"
awk '/^<<<<<<< /{skip=1; next} /^=======$/{skip=0; next} /^>>>>>>> /{next}
     !skip' a.c >a.tmp && mv a.tmp a.c
carve stage-file a.c && carve continue >/dev/null || fail "continue after gc"
git show HEAD~1 | grep -q '^+l 10 C1 fixed$' || fail "fix lost"

# --- a parked change is kept, with a hint
git reset -q --hard
git update-ref refs/worktree/carve/park/1 HEAD
carve gc --force >"$tmp/gc" || fail "gc with a park"
grep -q 'kept parked changes' "$tmp/gc" || fail "park: $(cat "$tmp/gc")"
git rev-parse -q --verify refs/worktree/carve/park/1 >/dev/null ||
	fail "park removed"
git update-ref -d refs/worktree/carve/park/1

# --- a worktree of a git carve check whose process is gone is
# removed with --force
git worktree add -q --detach "$tmp/git-carve-check.999999.x" HEAD
carve gc >"$tmp/gc" || fail "gc dry run, check worktree"
grep -q '^would remove worktree .*git-carve-check.999999.x' "$tmp/gc" ||
	fail "check worktree not offered: $(cat "$tmp/gc")"
carve gc --force >/dev/null || fail "gc --force, check worktree"
[ ! -d "$tmp/git-carve-check.999999.x" ] || fail "check worktree kept"
git worktree list | grep -q git-carve-check && fail "check worktree listed"

# --- refs for comments are offered in a dry run
# and removed with --force, in one go
git update-ref refs/carve/review HEAD
git update-ref refs/carve/remote/origin/review HEAD
git update-ref refs/carve/kept/gone/1 HEAD
git update-ref refs/carve/versions/ser/1 HEAD
carve gc >"$tmp/gc"
for ref in refs/carve/review refs/carve/remote/origin/review \
	   refs/carve/kept/gone/1 refs/carve/versions/ser/1; do
	grep -q "would remove $ref: no longer used by git carve" "$tmp/gc" ||
		fail "$ref: $(cat "$tmp/gc")"
	git rev-parse -q --verify "$ref" >/dev/null ||
		fail "$ref removed by a dry run"
done
carve gc --force >/dev/null || fail "gc --force"
[ -z "$(git for-each-ref refs/carve/review refs/carve/remote/ \
	refs/carve/kept/ refs/carve/versions/)" ] || fail "comment refs left"
exit 0
