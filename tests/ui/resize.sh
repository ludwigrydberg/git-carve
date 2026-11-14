#!/bin/sh
# SPDX-License-Identifier: MIT
# Ctrl-W resizes the panes. l and h move the divider between the lists and
# the diffs, j and k the divider of the active pane, and = puts the default
# layout back. Enter ends it and the keys move again. The sizes stay within
# the screen.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1
while [ $i -le 30 ]; do echo "line $i"; i=$((i + 1)); done >a.c
commit_all base
sed 's/line 5/LINE 5/' a.c >a.new && mv a.new a.c

# the column of the Unstaged pane's title, the row of the Staged one
title_col() {
	screen | awk '/> Unstaged: a.c|  Unstaged: a.c/ {
		print index($0, "Unstaged: a.c"); exit }'
}
staged_row() {
	screen | awk '/Staged: a.c/ && !/Unstaged/ { print NR; exit }'
}
# wait until f (a function) prints something other than $1
wait_change() {
	_n=30
	while [ "$($2)" = "$1" ]; do
		_n=$((_n - 1))
		[ "$_n" -gt 0 ] || fail "$2 stayed $1: $(screen)"
		sleep 0.1
	done
}

tui_start
wait_screen "M a.c"
wait_screen "+LINE 5"
col0=$(title_col)
row0=$(staged_row)
[ -n "$col0" ] && [ -n "$row0" ] || fail "no titles: $(screen)"

keys C-w
wait_screen "RESIZE"
keys l l
wait_change "$col0" title_col
[ "$(title_col)" -eq $((col0 + 4)) ] || fail "l l: $(title_col), was $col0"
keys h
wait_change "$((col0 + 4))" title_col
[ "$(title_col)" -eq $((col0 + 2)) ] || fail "h: $(title_col)"

# the diffs' divider, from the Unstaged pane j moves it down
keys Tab
wait_screen "> Unstaged: a.c"
keys j j j
wait_change "$row0" staged_row
[ "$(staged_row)" -eq $((row0 + 3)) ] || fail "j j j: $(staged_row), was $row0"
# far beyond the screen it stops with a row of the Staged pane left
step_keys j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j
[ "$(staged_row)" -lt 40 ] || fail "past the screen: $(staged_row)"

# = puts it all back
keys '='
wait_screen "the default layout"
[ "$(title_col)" -eq "$col0" ] || fail "=: column $(title_col)"
[ "$(staged_row)" -eq "$row0" ] || fail "=: row $(staged_row)"

# Enter ends it, j moves the cursor again, and the layout stays
keys l l Enter
wait_gone "RESIZE"
step_keys j
[ "$(title_col)" -eq $((col0 + 4)) ] || fail "after Enter: $(title_col)"
tui_quit

# stacked in a narrow terminal there are no columns to move, and in the
# Files pane j moves the divider between the lists and the diffs
unstaged_row() {
	screen | awk '/Unstaged: a.c/ { print NR; exit }'
}
TUI_W=80 tui_start
wait_screen "+LINE 5"
r0=$(unstaged_row)
keys C-w
wait_screen "RESIZE"
keys l
wait_screen "the panes are stacked"
keys j j
wait_change "$r0" unstaged_row
[ "$(unstaged_row)" -eq $((r0 + 2)) ] || fail "stacked j j: $(unstaged_row), was $r0"
keys Escape
wait_gone "RESIZE"
tui_quit
