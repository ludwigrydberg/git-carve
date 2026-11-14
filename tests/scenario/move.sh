#!/bin/sh
# SPDX-License-Identifier: MIT
# Lines of any commit of the series, moved without stopping there by hand.
# They go out into the work tree, are dropped, go into an older or a newer
# commit, or into a new commit in a gap, and a commit gets a new message.
# The tip keeps its tree, or loses just the lines, and your own staged and
# unstaged changes stay as they were. A conflict stops, and continues or
# aborts. undo takes a move back. With edits of your own, a drop or an edit
# reaches your files too. A whole file moves without a selection.
. "$(dirname "$0")/../lib.sh"
new_repo

i=1; while [ $i -le 40 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
edit "l 10" "l 10 P1"; commit_all P1
edit "l 20" "l 20 P2"; edit "l 30" "l 30 P2x"; commit_all P2
edit "l 35" "l 35 P3"; commit_all P3
orig=$(git rev-parse HEAD)
tip=$(tree_of HEAD)
subjects() { git log --format=%s | tr '\n' ' '; }
# into an older commit, P2's l 30 into P1, and the tip stays the same
carve move HEAD~1 a.c o30,n30 --into HEAD~2 >"$tmp/out" || fail "into older"
grep -q '^moved from ' "$tmp/out" || fail "message: $(cat "$tmp/out")"
[ "$(tree_of HEAD)" = "$tip" ] || fail "into older: tip changed"
[ "$(changes HEAD~2)" = "-l 10 +l 10 P1 -l 30 +l 30 P2x " ] ||
	fail "P1: $(changes HEAD~2)"
[ "$(changes HEAD~1)" = "-l 20 +l 20 P2 " ] || fail "P2: $(changes HEAD~1)"
# undo brings the series back as it was
carve undo >/dev/null || fail undo
[ "$(git rev-parse HEAD)" = "$orig" ] || fail "undo: $(subjects)"

# into a later commit
carve move HEAD~1 a.c o30,n30 --into HEAD >/dev/null || fail "into newer"
[ "$(tree_of HEAD)" = "$tip" ] || fail "into newer: tip changed"
[ "$(changes HEAD)" = "-l 30 +l 30 P2x -l 35 +l 35 P3 " ] ||
	fail "P3: $(changes HEAD)"
git reset -q --hard "$orig"

# into a new commit, before P2 and after it
carve move HEAD~1 a.c o30,n30 --new-before HEAD~1 -m "P2a" >/dev/null ||
	fail "new before"
[ "$(subjects)" = "P3 P2 P2a P1 base " ] || fail "new before: $(subjects)"
[ "$(tree_of HEAD)" = "$tip" ] || fail "new before: tip changed"
[ "$(changes HEAD~2)" = "-l 30 +l 30 P2x " ] || fail "P2a: $(changes HEAD~2)"
git reset -q --hard "$orig"
carve move HEAD~1 a.c o30,n30 --new-after HEAD~1 -m "P2b" >/dev/null ||
	fail "new after"
[ "$(subjects)" = "P3 P2b P2 P1 base " ] || fail "new after: $(subjects)"
[ "$(tree_of HEAD)" = "$tip" ] || fail "new after: tip changed"
git reset -q --hard "$orig"

# out, the lines leave the series and wait in the work tree, unstaged, and
# your own changes, staged and not, stay as they were
edit "l 1" "l 1 S"; git add a.c
echo "tail" >>a.c
carve move HEAD~1 a.c o30,n30 --out >/dev/null || fail out
[ "$(changes HEAD~1)" = "-l 20 +l 20 P2 " ] || fail "out: $(changes HEAD~1)"
[ "$(git diff --cached | grep '^[-+]l' | tr '\n' ' ')" = "-l 1 +l 1 S " ] ||
	fail "staged: $(git diff --cached)"
git diff | grep -q '^+l 30 P2x$' || fail "not in the work tree: $(git diff)"
git diff | grep -q '^+tail$' || fail "your edit lost"
git reset -q --hard "$orig"

# drop, gone from the series and the work tree
carve drop HEAD~1 a.c o30,n30 >/dev/null || fail drop
grep -q 'P2x' a.c && fail "dropped line still in the work tree"
[ -z "$(git status --porcelain --untracked-files=no)" ] || fail "drop: dirty"
git reset -q --hard "$orig"

# HEAD straight, without a rebase, also where a rebase of yours stopped
carve move HEAD a.c o35,n35 --new-after HEAD -m x >/dev/null 2>&1 &&
	fail "all of HEAD moved"
carve reword HEAD~2 -m "P1 better" >/dev/null || fail reword
[ "$(subjects)" = "P3 P2 P1 better base " ] || fail "reword: $(subjects)"
[ "$(tree_of HEAD)" = "$tip" ] || fail "reword: tip changed"
git reset -q --hard "$orig"

# a conflict, P4 changes the line P2 gives up, and continue after resolving
edit "l 30 P2x" "l 30 P4"; commit_all P4
must_fail carve move HEAD~2 a.c o30,n30 --out
grep -q 'conflicts' "$tmp/must_fail.out" || fail "stop: $(cat "$tmp/must_fail.out")"
carve status | grep -q '^MOVE ' || fail "status: $(carve status)"
awk '!/^(<<<<<<<|=======|>>>>>>>)/ && $0 != "l 30" { print }' a.c >a.tmp &&
	mv a.tmp a.c
git add a.c
carve continue >/dev/null || fail "continue: $(carve status)"
[ "$(git log -1 --format=%s)" = P4 ] || fail "after continue: $(subjects)"
grep -q '^l 30 P4$' a.c || fail "P4 lost"
carve status 2>&1 | grep -q '^MOVE' && fail "still in progress"
# and abort takes it all back
p4=$(git rev-parse HEAD)
must_fail carve move HEAD~2 a.c o20,n20 --into HEAD~3
carve abort >/dev/null 2>&1
[ "$(git rev-parse HEAD)" = "$p4" ] || fail "abort: $(subjects)"
# nothing of the moves left behind in $GIT_DIR/carve
carve move HEAD a.c o20,n20 --into HEAD~1 >/dev/null 2>&1 || :
left=
for f in "$(git rev-parse --git-path carve)"/*; do
	[ -e "$f" ] || continue
	case ${f##*/} in undo*) ;; *) left="$left ${f##*/}" ;; esac
done
[ -z "$left" ] || fail "state left: $left"

# with your own edits elsewhere in the file, drop takes the lines out of
# the series and of your work tree, and edit puts them in both, while your
# edits stay
git reset -q --hard "$orig"
edit "l 1" "l 1 mine"
carve drop HEAD~1 a.c o30,n30 >/dev/null || fail "drop, dirty"
git show HEAD~1 | grep -q 'P2x' && fail "drop, dirty: still in P2"
grep -q 'P2x' a.c && fail "drop, dirty: the line in the work tree"
grep -q '^l 1 mine$' a.c || fail "drop, dirty: your edit lost"
git reset -q --hard "$orig"
edit "l 1" "l 1 mine"
printf 'l 30 P2y\n' >"$tmp/new"
carve edit HEAD~1 a.c n30 "$tmp/new" >/dev/null || fail "edit, dirty"
git show HEAD~1:a.c | grep -q '^l 30 P2y$' || fail "edit: the commit"
grep -q '^l 30 P2y$' a.c || fail "edit: the work tree"
grep -q '^l 1 mine$' a.c || fail "edit: your edit lost"

# a whole file out of an older commit (no selection): into the work tree
git reset -q --hard "$orig"
printf 'b\n' >b.c
edit "l 39" "l 39 P5"
git add b.c a.c && git commit -q -m P5
edit "l 2" "l 2 P6"; commit_all P6
carve move HEAD~1 b.c --out >/dev/null || fail "whole file out"
git cat-file -e HEAD~1:b.c 2>/dev/null && fail "b.c still in P5"
git show HEAD~1 | grep -q '^+l 39 P5$' || fail "P5 lost its a.c"
[ "$(cat b.c)" = b ] || fail "b.c not in the work tree"

exit 0
