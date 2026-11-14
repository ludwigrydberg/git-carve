#!/bin/sh
# SPDX-License-Identifier: MIT
# The Series pane above the Files pane, the tip on top. Shift-Tab reaches
# it, j and k move its cursor (>) and show that commit once it settles, and
# Enter goes on to the commit's files. Tab goes back to the Files pane.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1; while [ $i -le 30 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
git branch -q up
git branch -q --set-upstream-to=up
for n in 1 2 3; do
	sed "s/^l $((n * 10))\$/l $((n * 10)) P$n/" a.c >a.tmp && mv a.tmp a.c
	commit_all "P$n"
done

tui_start
wait_screen ' Series'
wait_screen 'Commit HEAD (1)'
# the tip on top, older ones below
[ "$(screen_row ' P3')" -lt "$(screen_row ' P1')" ] ||
	fail "P3 not above P1: $(screen)"
keys BTab
wait_screen '> Series'
screen | grep -q -E '^> +HEAD +[0-9a-f]+ P3' || fail "cursor: $(screen)"
keys j j
wait_screen 'Commit ~2 (1)'
wait_screen '+l 10 P1'
screen | grep -q -E '^> +~2 +[0-9a-f]+ P1' || fail "cursor on P1: $(screen)"
keys Enter
wait_screen '> Files'
screen | grep -q -E '^\* +~2 +[0-9a-f]+ P1' || fail "shown row: $(screen)"
keys BTab
wait_screen '> Series'
keys Tab
wait_screen '> Files'
tui_quit
exit 0
