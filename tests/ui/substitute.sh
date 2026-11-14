#!/bin/sh
# SPDX-License-Identifier: MIT
# :s/pattern/replacement/ in the TUI, on the line under the cursor of a
# commit of the Series pane (its work-tree line) and on marked lines of the
# Unstaged pane, and :%s on the whole file. u undoes it.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1; while [ $i -le 30 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
sed 's/^l 10$/l 10 timeout 100/' a.c >a.tmp && mv a.tmp a.c
commit_all P1
sed 's/^l 20$/l 20 P2/' a.c >a.tmp && mv a.tmp a.c
commit_all P2
git branch -q up HEAD~2
git branch -q --set-upstream-to=up

tui_start
wait_screen 'Commit HEAD'
keys J
wait_screen 'Commit ~1'
wait_screen '+l 10 timeout 100'
keys Tab
wait_screen '> Commit ~1: a.c'
# on the commit's line the work tree changes, the commit not yet
keys / 'timeout 100' Enter : 's/100/500/' Enter
wait_screen "replaced in 1 line of a.c (u undoes it)"
grep -q '^l 10 timeout 500$' a.c || fail "work tree: $(grep 'l 10' a.c)"
wait_screen 'Unstaged (1)'
# u puts it back
keys u
wait_screen 'undid substitute'
grep -q '^l 10 timeout 100$' a.c || fail "undo: $(grep 'l 10' a.c)"
# again, then stage and amend, and HEAD has the new value
keys : 's/100/500/' Enter
wait_screen "replaced in 1 line"
keys BTab G Enter
wait_screen 'Staged (1)'
keys A
wait_screen 'amended HEAD'
git show HEAD | grep -q '^+l 10 timeout 500$' || fail "amend"

# marked lines of the Unstaged pane, where a removed line is not in the
# file
sed -e 's/^l 3$/l 3 x/' -e 's/^l 5$/l 5 x/' -e 's/^l 7$/l 7 x/' a.c >a.tmp &&
	mv a.tmp a.c
keys C-l
wait_screen 'Unstaged (1)'
# the work tree's a.c is the last entry of the Files pane
keys G Tab
wait_screen '> Unstaged: a.c'
keys / 'l 3 x' Enter m
keys / 'l 7 x' Enter m
keys : 's/x$/y/' Enter
wait_screen "replaced in 2 lines of a.c"
grep -q '^l 3 y$' a.c && grep -q '^l 5 x$' a.c && grep -q '^l 7 y$' a.c ||
	fail "marked: $(sed -n '3,7p' a.c)"
# :%s changes every line of the file
keys : '%s/^l /L /' Enter
wait_screen "replaced in 30 lines of a.c"
[ "$(grep -c '^L ' a.c)" = 30 ] || fail "%s"
# the Staged pane is the index, not the work tree
keys Tab
keys : 's/L/M/' Enter
wait_screen ':s changes the work tree'
close_box
keys : 's/(/x/' Enter
wait_screen 'pattern'
tui_quit
exit 0
