#!/bin/sh
# SPDX-License-Identifier: MIT
# Colours are optional. On a monochrome terminal and with NO_COLOR nothing
# is coloured and every state stays visible. With carve.asciiLines lines
# are drawn with ASCII. carve.colors = 256 picks shades of its own where
# the terminal has 256 colours, and the basic ones where it has not.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\nb\nc\n' >f.c
commit_all base
printf 'a\nB\nc\nd\n' >f.c

# as a control, the default terminal does get colours
tui_start
wait_screen "M f.c"
wait_screen "+B"
screen_has_colors || fail "no colours on a colour terminal (test is broken)"
tui_quit

check_mono() {
	wait_screen "M f.c"
	wait_screen "+B"
	screen_has_colors && fail "$1: colours used"
	# cursor, active pane and change markers are characters
	screen | grep -q '^>M f.c' || fail "$1: file cursor"
	screen | grep -q '^> Files' || fail "$1: active pane"
	keys Tab
	wait_screen "> Unstaged: f.c"
	step_keys : 2 Enter
	screen | cut -c32- | grep -q '^>  *2 +B' || fail "$1: diff cursor"
	step_keys v j
	screen | cut -c33- | grep -q '^\*' || fail "$1: range marker"
	keys Escape : 2 Enter m
	wait_screen "1 line marked"
	screen | cut -c33- | grep -q '^+ *2 +B' || fail "$1: mark marker"
	keys Space
	wait_index f.c 'a\nb\nB\nc\n'
	screen_has_colors && fail "$1: colours after an action"
	tui_quit
	git reset -q
}

TUI_TERM=vt100 tui_start
check_mono "TERM=vt100"

TUI_ENV="NO_COLOR=1" tui_start
check_mono "NO_COLOR"

# an assignment before a function call may outlive the call in sh
TUI_TERM='' TUI_ENV=''

# with ASCII lines the pane separator is '|'
git config carve.asciiLines true
tui_start
wait_screen "M f.c"
[ "$(screen | sed -n '3,20p' | cut -c31 | sort -u)" = "|" ] ||
	fail "separator: $(screen | sed -n '3,5p')"
tui_quit

# carve.colors = 256: an added line in the 256 colour green, and in the
# basic green on a terminal with 8 colours
git config --unset carve.asciiLines
git config carve.colors 256
# wait_sgr <regex>: wait until the row of +B has that colour code
wait_sgr() {
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ]; do
		row_sgr "+B" | grep -q -- "$1" && return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	fail "no $1 on the +B row: $(row_sgr "+B" | od -c | head)"
}
TUI_TERM=screen-256color tui_start
wait_sgr '38;5;114m+B'
tui_quit
TUI_TERM=screen
tui_start
wait_sgr '32m+B'
row_sgr "+B" | grep -q '38;5;' && fail "256 colours on a terminal with 8"
tui_quit
