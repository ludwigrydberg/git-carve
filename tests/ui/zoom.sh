#!/bin/sh
# SPDX-License-Identifier: MIT
# o zooms the active pane (a diff, Files) to the whole screen and back. Tab
# goes to the next pane, still zoomed. On a commit of the Series pane the
# zoom stays on from commit to commit (J and K).
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1; while [ $i -le 30 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
for n in 1 2; do
	sed "s/^l $((n * 10))\$/l $((n * 10)) P$n/" a.c >a.tmp && mv a.tmp a.c
	commit_all "P$n"
done
sed 's/^l 5$/l 5 staged/' a.c >a.tmp && mv a.tmp a.c
git add a.c
sed 's/^l 25$/l 25 unstaged/' a.c >a.tmp && mv a.tmp a.c

tui_start
wait_screen '+l 25 unstaged'
# the Files pane alone first, then the upper diff
keys o
wait_gone 'Unstaged: a.c'
wait_screen '> Files'
keys o
wait_screen 'Unstaged: a.c'
keys Tab o
wait_screen '> Unstaged: a.c  [zoom]'
screen | grep -q 'Files' && fail "Files pane still there: $(screen)"
screen | grep -q '^>  *22 *22  l 22' || screen | grep -q '^  *22 *22  l 22' ||
	fail "not from the left edge: $(screen | head -n 5)"
wait_screen 'zoomed in: o zooms out, Tab goes to the next pane'
# Tab goes to the other diff pane, zoomed
keys Tab
wait_screen '> Staged: a.c  [zoom]'
screen | grep -q 'Unstaged: a.c' && fail "Unstaged still shown"
# o again goes back to the panes
keys o
wait_screen '  Files'
wait_screen '> Staged: a.c'
screen | grep -q '\[zoom\]' && fail "still zoomed"
tui_quit

# a commit, zoomed on its diff and on the next one too
git reset -q --hard
git branch -q up HEAD~2
git branch -q --set-upstream-to=up
tui_start
wait_screen 'Commit HEAD'
keys J
wait_screen 'Commit ~1'
keys Tab o
wait_screen '> Commit ~1: a.c  [zoom]'
wait_screen '+l 10 P1'
keys K
wait_screen '+l 20 P2'
wait_screen '> Commit HEAD: a.c  [zoom]'
keys o
wait_screen ' Series'
tui_quit
exit 0
