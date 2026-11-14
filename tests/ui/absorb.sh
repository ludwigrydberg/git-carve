#!/bin/sh
# SPDX-License-Identifier: MIT
# ga shows absorb's plan, where each change block goes or why it stays, and
# Enter does it. The series is @{upstream}..HEAD.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1; while [ $i -le 30 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
sed 's/^l 5$/l 5 C1/' a.c >a.tmp && mv a.tmp a.c
commit_all C1
sed 's/^l 20$/l 20 C2/' a.c >a.tmp && mv a.tmp a.c
commit_all C2
git branch -q up HEAD~2
git branch -q --set-upstream-to=up
sed 's/^l 5 C1$/l 5 C1 fixed/; s/^l 25$/l 25 new/' a.c >a.tmp && mv a.tmp a.c

tui_start
wait_screen "M a.c"
keys g a
wait_screen "Absorb 1 of 2 change blocks?"
# a question, not a list to choose from
wait_screen "Enter: absorb  Esc: cancel"
screen | grep -q "move: up/down" && fail "absorb reads like a picker"
# other keys do not answer it
step_keys j x
screen | grep -q "Enter: absorb" || fail "closed by another key"
wait_screen "a.c:5 -> "
wait_screen "a.c:25 stays: its lines come from"
# where each goes in green, what stays dim (blue)
esc=$(printf '\033')
tmux -L "$TMUX_SOCK" capture-pane -e -p -t t | grep -F "a.c:5 " |
	grep -q "$esc\[[0-9;]*32m" || fail "-> not green"
tmux -L "$TMUX_SOCK" capture-pane -e -p -t t | grep -F "a.c:25" |
	grep -q "$esc\[[0-9;]*34mstays: " || fail "stays not dim"
keys Escape
wait_screen "not absorbed"
keys g a
wait_screen "Absorb 1 of 2"
keys Enter
wait_screen "absorbed 1 change block"
git show HEAD~1 | grep -q '^+l 5 C1 fixed$' || fail "C1 not fixed"
git diff | grep -q '^+l 25 new$' || fail "the other block moved"
tui_quit
