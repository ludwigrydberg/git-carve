#!/bin/sh
# SPDX-License-Identifier: MIT
# Renames are found in the work tree without touching the index, staged as
# a move with the edits left unstaged as line changes against the old file,
# and unstaged in both halves. Line numbers refer to the diff against the
# old file both before and after the move is staged.
. "$(dirname "$0")/../lib.sh"
new_repo

seq_lines() {
	i=1; while [ $i -le "$1" ]; do echo "line $i"; i=$((i + 1)); done
}
seq_lines 30 >old.c
printf 'x\n' >other.c
commit_all base
mv old.c new.c
sed 's/^line 5$/LINE 5/; s/^line 25$/LINE 25/' new.c >new.tmp && mv new.tmp new.c
idx=$(git write-tree)

carve renames >"$tmp/out" || fail renames
grep -q '^R0[0-9][0-9] old.c -> new.c (in the work tree)$' "$tmp/out" ||
	fail "renames: $(cat "$tmp/out")"
[ "$(git write-tree)" = "$idx" ] || fail "detection touched the index"
set -- .git/carve-*
[ -e "$1" ] && fail "temporary index left"
carve diff new.c >"$tmp/diff"
head -1 "$tmp/diff" | grep -q '^# renamed from old.c (the move is not staged yet)$' ||
	fail "diff note: $(head -1 "$tmp/diff")"
grep -q '^ *5 *-line 5$' "$tmp/diff" || fail "not diffed against old.c"

# a line stages the move and that line, and the other edit stays unstaged
carve stage new.c o5,n5 || fail "stage line"
[ -z "$(git ls-files -s old.c)" ] || fail "old.c still staged"
git show :new.c | grep -q '^LINE 5$' || fail "line 5 not staged"
git show :new.c | grep -q '^line 25$' || fail "line 25 staged"
carve renames | grep -q '^R0[0-9][0-9] old.c -> new.c (staged)$' ||
	fail "staged rename: $(carve renames)"
carve diff --cached new.c | grep -q '^ *5 *-line 5$' ||
	fail "staged diff not against old.c"
git diff --quiet -- old.c 2>/dev/null
# unstaging its line leaves the pure move
carve unstage new.c o5,n5 || fail "unstage line"
[ "$(git show :new.c)" = "$(git show HEAD:old.c)" ] || fail "move lost"
carve unstage-move old.c new.c || fail "unstage-move"
[ "$(git write-tree)" = "$idx" ] || fail "unstage-move: index differs"

# only the move
carve stage-move old.c new.c || fail "stage-move"
[ "$(git show :new.c)" = "$(git show HEAD:old.c)" ] || fail "moved with edits"
carve diff new.c | grep -q '^ *25 *-line 25$' || fail "edits not unstaged"
carve unstage-move old.c new.c

# a failed selection changes nothing
must_fail carve stage new.c n99
[ "$(git write-tree)" = "$idx" ] || fail "failed stage moved"

# undo of a staged move
carve stage-move old.c new.c && carve undo >/dev/null || fail "undo move"
[ "$(git write-tree)" = "$idx" ] || fail "undo: index differs"

# commit the rename with its edits, and it is a rename in git's eyes
carve stage new.c o5,n5,o25,n25 && carve commit -m "rename" >/dev/null ||
	fail commit
git show --stat -M HEAD | grep -q 'old.c => new.c' || fail "not a rename"

# an exact copy is a new file, with a note
cp other.c copy.c
carve diff copy.c | head -1 | grep -q '^# new file, a copy of other.c$' ||
	fail "copy note: $(carve diff copy.c | head -1)"
carve renames | grep -q copy.c && fail "copy listed as a rename"
# staged, the copy is found from the index's blob
carve stage-file copy.c || fail "stage the copy"
carve diff --cached copy.c | head -1 |
	grep -q '^# new file, a copy of other.c$' ||
	fail "staged copy note: $(carve diff --cached copy.c | head -1)"
# a new file that is no copy has no note
echo different >fresh.c
carve diff fresh.c | grep -q 'a copy of' && fail "no copy: $(carve diff fresh.c)"
exit 0
