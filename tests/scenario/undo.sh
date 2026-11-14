#!/bin/sh
# SPDX-License-Identifier: MIT
# Every change to the index or HEAD can be undone, newest first. Operations
# spanning several commands, a split or a fixup-to stopped at a conflict,
# are one entry. The work tree is never touched, entries survive git gc,
# and carve.undoLimit bounds the history.
. "$(dirname "$0")/../lib.sh"
new_repo

entries() {
	carve undo --list | wc -l | tr -d ' '
}

printf 'a\nb\nc\n' >f.c
commit_all base
base=$(git rev-parse HEAD)
printf 'A\nb\nc\nd\n' >f.c
wt=$(worktree_state)

# --- line staging, file staging, set-staged ------------------------------
must_fail carve undo
grep -q 'nothing to undo' "$tmp/must_fail.out" || fail "empty undo"
carve stage f.c o1,n1 || fail stage
s1=$(git write-tree)
printf 'D\n' | carve set-staged f.c n4 - || fail set-staged
carve stage f.c o4,n4 || fail stage2
[ "$(entries)" = 3 ] || fail "3 entries: $(carve undo --list)"
carve undo --list | sed -n 2p | grep -q '^2 .*  set-staged f.c n4 -$' ||
	fail "list: $(carve undo --list)"
carve undo >/dev/null || fail "undo stage2"
assert_index_text f.c 'A\nb\nc\nD\n'
carve undo >"$tmp/out" || fail "undo set-staged"
grep -q '^undid set-staged f.c n4 -$' "$tmp/out" || fail "undo message"
[ "$(git write-tree)" = "$s1" ] || fail "index after undo"
carve undo >/dev/null || fail "undo stage"
assert_index_text f.c 'a\nb\nc\n'
[ "$(worktree_state)" = "$wt" ] || fail "work tree touched"
[ "$(entries)" = 0 ] || fail "entries left"
carve stage-file f.c && carve unstage-file f.c && carve undo >/dev/null &&
	assert_index_text f.c 'A\nb\nc\nd\n' || fail "unstage-file undo"
carve undo >/dev/null && assert_index_text f.c 'a\nb\nc\n' ||
	fail "stage-file undo"

# when nothing changed nothing is recorded, also for failing commands
carve unstage-file f.c >/dev/null 2>&1
must_fail carve stage f.c n99
[ "$(entries)" = 0 ] || fail "no-op recorded: $(carve undo --list)"

# --- undo the last commit, a reset --soft
# ---------------------------------
carve stage-file f.c
carve commit -m one >/dev/null || fail commit
one=$(git rev-parse HEAD)
carve undo >"$tmp/out" || fail "undo commit"
grep -q '^undid commit -m one$' "$tmp/out" || fail "commit message"
[ "$(git rev-parse HEAD)" = "$base" ] || fail "HEAD after undo commit"
[ "$(short_status)" = "M  f.c" ] || fail "status: $(short_status)"
git reflog -1 --format=%gs | grep -q 'carve: undo commit' || fail reflog

# amend goes back to the commit before the amend, which gc must not take
carve commit -m one >/dev/null
one=$(git rev-parse HEAD)
printf 'A\nb\nc\nd\ne\n' >f.c
carve stage-file f.c
carve commit --amend -m "one, amended" >/dev/null || fail amend
git reflog expire --expire=now --all
git gc -q --prune=now
carve undo >/dev/null || fail "undo amend after gc"
[ "$(git rev-parse HEAD)" = "$one" ] || fail "HEAD after undo amend"
assert_index_text f.c 'A\nb\nc\nd\ne\n'

# --- refused when the state moved on, unless --force ----------------------
carve undo >/dev/null   # the stage-file
carve undo >/dev/null   # the commit
carve stage-file f.c
git reset -q
must_fail carve undo
grep -q 'index has changed' "$tmp/must_fail.out" ||
	fail "changed index: $(cat "$tmp/must_fail.out")"
carve undo --force >/dev/null || fail "--force"
assert_index_text f.c 'A\nb\nc\nd\n'
carve unstage-file f.c
carve stage-file f.c && carve commit -m two >/dev/null
git commit -q --allow-empty -m "outside carve"
must_fail carve undo
grep -q 'HEAD has moved' "$tmp/must_fail.out" || fail "moved HEAD"
git reset -q --soft HEAD~1
carve undo >/dev/null || fail "undo after HEAD came back"
[ "$(git rev-parse HEAD)" = "$base" ] || fail "HEAD after undo two"
carve unstage-file f.c

# --- split, one entry once finished, and inside it the steps are undoable
# ---
printf 'A\nb\nc\nd\n' >f.c
carve stage-file f.c && carve commit -m big >/dev/null
big=$(git rev-parse HEAD)
n0=$(entries)
carve split >/dev/null || fail split
carve stage f.c o1,n1 && carve commit -m p1 >/dev/null
carve undo >/dev/null && carve undo >/dev/null || fail "undo inside split"
must_fail carve undo
grep -q "nothing to undo since the split in progress began; 'git-carve-cli abort'" \
	"$tmp/must_fail.out" || fail "undo past split start"
carve stage f.c o1,n1 && carve commit -m p1 >/dev/null
carve stage-file f.c && carve commit -m p2 >/dev/null
carve split --finish >/dev/null || fail finish
[ "$(entries)" = $((n0 + 1)) ] || fail "split not folded: $(carve undo --list)"
carve undo --list | head -1 | grep -q ' split$' || fail "split entry"
carve undo >/dev/null || fail "undo split"
[ "$(git rev-parse HEAD)" = "$big" ] || fail "HEAD after undo split"
[ "$(short_status)" = "" ] || fail "status after undo split: $(short_status)"
git for-each-ref refs/worktree/carve/undo-pending | grep -q . &&
	fail "pending left"
# aborted, nothing is left behind
carve split >/dev/null && carve stage-file f.c &&
	carve commit -m x >/dev/null && carve abort >/dev/null || fail abort
[ "$(entries)" = "$n0" ] || fail "abort left entries: $(carve undo --list)"

# --- split the root commit, then undo it ----------------------------------
new_repo root
printf 'r\n' >r.c
commit_all root
root=$(git rev-parse HEAD)
carve split >/dev/null && carve stage-file r.c &&
	carve commit -m "root again" >/dev/null && carve split --finish >/dev/null ||
	fail "split root"
carve undo >/dev/null || fail "undo root split"
[ "$(git rev-parse HEAD)" = "$root" ] || fail "root HEAD"
carve undo >/dev/null 2>&1 && fail "undo past the root split"
# undo the first commit of a repository, back to unborn
new_repo unborn
printf 'x\n' >x.c
carve stage-file x.c && carve commit -m first >/dev/null
carve undo >/dev/null || fail "undo first commit"
git rev-parse -q --verify HEAD >/dev/null && fail "HEAD still born"
assert_index_text x.c 'x\n'

# --- fixup-to with a conflict, one entry after continue -----------------
new_repo fixup
printf 'one\n' >f.c
commit_all C1
printf 'one\nint helper(void)\n{\n\tretrun 1;\n}\nlast\n' >f.c
commit_all C2
printf 'one\nint helper(void)\n{\n\tretrun 1;\n} /* helper */\nlast\n' >f.c
commit_all C3
tip=$(git rev-parse HEAD)
printf 'one\nint helper(void)\n{\n\treturn 1;\n} /* helper */\nlast\n' >f.c
must_fail carve fixup-to HEAD~1 f.c o4,n4
grep -q 'conflicted file' "$tmp/must_fail.out" ||
	fail "no conflict: $(cat "$tmp/must_fail.out")"
git for-each-ref refs/worktree/carve/undo-pending | grep -q . ||
	fail "no pending entry"
printf 'one\nint helper(void)\n{\n\treturn 1;\n} /* helper */\nlast\n' >f.c
carve stage-file f.c && carve continue >/dev/null || fail continue
carve undo --list | head -1 | grep -q ' fixup-to HEAD~1 f.c o4,n4$' ||
	fail "fixup entry: $(carve undo --list)"
[ "$(carve undo --list | wc -l | tr -d ' ')" = 1 ] ||
	fail "stage-file during the conflict not folded: $(carve undo --list)"
wt=$(worktree_state)
carve undo >/dev/null || fail "undo fixup-to"
[ "$(git rev-parse HEAD)" = "$tip" ] || fail "HEAD after undo fixup-to"
[ "$(worktree_state)" = "$wt" ] || fail "undo touched the work tree"
[ "$(short_status)" = " M f.c" ] || fail "fix not back in the work tree"

# --- carve.undoLimit ------------------------------------------------------
new_repo limit
printf '1\n2\n3\n4\n5\n' >f.c
commit_all base
printf 'a\nb\nc\nd\ne\n' >f.c
git config carve.undoLimit 3
for i in 1 2 3 4 5; do
	carve stage f.c "o$i,n$i" || fail "stage $i"
done
[ "$(entries)" = 3 ] || fail "limit: $(carve undo --list)"
carve undo --list | tail -1 | grep -q 'stage f.c o3,n3$' || fail "oldest kept"
git config carve.undoLimit 0
carve stage-file f.c
[ "$(entries)" = 3 ] || fail "undoLimit 0 still records"
exit 0
