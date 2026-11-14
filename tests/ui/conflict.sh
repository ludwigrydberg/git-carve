#!/bin/sh
# SPDX-License-Identifier: MIT
# A conflicted file's Unstaged pane lists its conflict regions. O and T
# take ours or theirs for the one under the cursor, or in the Files pane
# after asking for the whole file. A resolved file is staged.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf '1\na\n2\n3\n4\n5\n6\nb\n7\n' >f.c
printf 'g\n' >g.c
commit_all base
git checkout -q -b other
printf '1\na-theirs\n2\n3\n4\n5\n6\nb-theirs\n7\n' >f.c
printf 'g-theirs\n' >g.c
commit_all theirs
git checkout -q -
printf '1\na-ours\n2\n3\n4\n5\n6\nb-ours\n7\n' >f.c
printf 'g-ours\n' >g.c
commit_all ours
git merge -q other >/dev/null 2>&1

tui_start
wait_screen "U f.c"
keys Tab
wait_screen "(unmerged: 2 conflicts; O / T take ours / theirs"
wait_screen "conflict 2 of 2, line 12: < ours HEAD, > theirs other"
wait_screen "< b-ours"
wait_screen "> b-theirs"
# a search counts and lists the conflicts' lines, in every unmerged file
keys / theirs Enter
wait_screen "3 matches in unstaged changes"
keys g /
wait_screen '3 matches for "theirs" in unstaged changes'
wait_screen "f.c:c1: > a-theirs"
wait_screen "g.c:c1: > g-theirs"
keys Escape
wait_gone "3 matches for"
keys n n
wait_screen "Unstaged: g.c"
wait_screen "> g-theirs"
keys BTab g g Tab
wait_screen "Unstaged: f.c"
# ] and [ go from conflict to conflict
keys g g ']'
wait_cursor 'conflict 1 of 2'
cursor_row | grep -q 'conflict 1 of 2' || fail "]: $(cursor_row)"
keys ']'
wait_cursor 'conflict 2 of 2'
cursor_row | grep -q 'conflict 2 of 2' || fail "] again: $(cursor_row)"
keys [
wait_cursor 'conflict 1 of 2'
cursor_row | grep -q 'conflict 1 of 2' || fail "[: $(cursor_row)"
# on conflict 1's ours line
keys / a-ours Enter O
wait_screen "took ours for conflict 1 of 2 in f.c"
grep -q '^a-ours$' f.c || fail "O"
# the one left keeps its number, not a new "conflict 1 of 1"
wait_screen "(unmerged: 1 of 2 conflicts left;"
wait_screen "conflict 2 of 2, line"
screen | grep -q "conflict 1 of 1" && fail "renumbered"
keys / b-theirs Enter T
wait_screen "took theirs for conflict 2 of 2 in f.c; resolved and staged"
grep -q '^b-theirs$' f.c || fail "T"
[ -z "$(git diff --name-only --diff-filter=U -- f.c)" ] || fail "f.c unmerged"

# in the Files pane, the whole file, after asking
keys BTab
wait_screen "> Files"
# f.c, resolved, is in Staged above, and g.c last
keys G
wait_screen ">U g.c"
keys O
wait_screen "Take ours for all of g.c?"
keys y
wait_screen "took ours for g.c"
[ "$(cat g.c)" = g-ours ] || fail "g.c: $(cat g.c)"
[ -z "$(git diff --name-only --diff-filter=U)" ] || fail "still unmerged"
# after u the conflict is back
keys u
wait_screen "undid resolve g.c"
grep -q '^<<<<<<<' g.c || fail "g.c markers not back"
git diff --name-only --diff-filter=U | grep -q '^g.c$' ||
	fail "g.c not unmerged again"
tui_quit
