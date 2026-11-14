#!/bin/sh
# SPDX-License-Identifier: MIT
# The Series pane names the branches at its commits, local green and
# remote-tracking red, but not HEAD's own. It ends the series where no
# remote branch has it ("pushed"), and its title says what is shown, the
# work tree, HEAD, or a commit below it. Only the active pane title
# gets a background in the default colour palette.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1; while [ $i -le 30 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
for n in 1 2 3 4; do
	edit "l $((n * 5))" "l $((n * 5)) P$n"
	commit_all "P$n"
done
git update-ref refs/remotes/origin/main HEAD~2
git branch -q master HEAD~3
printf 'x\n' >>a.c

TUI_W=110 TUI_H=34 tui_start
wait_screen "M a.c"
wait_screen "Series: work tree"
wait_screen "~1 "
wait_screen " -- pushed --"
keys J J J J
wait_screen "(master)"
wait_screen "(origin/ma"
screen | grep -q '(main)' && fail "HEAD's own branch listed"
esc=$(printf '\033')
row_sgr "master" | grep -q "$esc\[[0-9;]*32mmaster" || fail "local not green"
row_sgr "origin/ma" | grep -q "$esc\[[0-9;]*31morigin/ma" ||
	fail "remote not red"
# the commits below the divider are pushed, and ~2 is the first of them
screen | awk '/-- pushed --/ { d = 1 } d && /~2 / { ok = 1 } END { exit !ok }' ||
	fail "divider: $(screen | head -12)"
keys K K K
wait_screen "Series: HEAD"
wait_screen "Commit HEAD: a.c"
keys J
wait_screen "Series: ~1 below HEAD"
wait_screen "Commit ~1: a.c"
# The program header and inactive Series title use the terminal background.
for title in "git-carve" "Series: ~1 below HEAD"; do
	row_sgr "$title" | grep -q "$esc\[[0-9;]*4[0-7]m" &&
		fail "$title has a background"
done
keys BTab
wait_screen "> Series: ~1 below HEAD"
row_sgr "> Series:" | grep -q "$esc\[[0-9;]*46m" ||
	fail "active Series title has no cyan background"
tui_quit
