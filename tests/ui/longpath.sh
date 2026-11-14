#!/bin/sh
# SPDX-License-Identifier: MIT
# Long paths, as on the kernel tree, in a narrow Files pane. The file keeps
# its name in full under its directory's row, and the directory is cut in
# the middle as file browsers do, its first directory, "~" (an ellipsis in
# UTF-8), and its end. h scrolls the selected row to its start.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

d=drivers/net/ethernet/intel/e1000e
mkdir -p $d
w=drivers/net/wireless/intel/iwlwifi/mvm
mkdir -p $d $w
printf 'a\n' >$d/80003es2lan.c
printf 'a\n' >$w/ops.c
printf 'a\n' >short.c
commit_all base
printf 'b\n' >$d/80003es2lan.c
printf 'b\n' >$w/ops.c
printf 'b\n' >short.c

TUI_W=100 TUI_H=24 tui_start
wait_screen "M short.c"
screen | grep -q '^ drivers/~.*intel/e1000e/' ||
	fail "middle cut: $(screen | grep e1000e)"
screen | grep -q '^[ >]M   80003es2lan\.c' ||
	fail "the name: $(screen | grep 80003)"
# h scrolls the selected file and its directory's row only, as far as its
# own text goes, and the other rows keep their end. More h changes nothing,
# l goes back, and another file selected starts unscrolled.
step_keys h h
screen | grep -q '^ drivers/net' && fail "h on short.c moved a row: $(screen)"
keys j
wait_match '^>M   80003es2lan\.c' || fail "j: $(screen | grep 80003)"
keys h h h h
wait_match '^ drivers/net/ethernet' || fail "h: $(screen | grep e1000e)"
screen | grep -q '^ drivers/net/ethernet.*>' ||
	fail "no right mark: $(screen | grep ethernet)"
screen | grep -q '^ drivers/~.*iwlwifi/mvm/' ||
	fail "a row not selected moved: $(screen | grep mvm)"
screen | grep -q '^ M short\.c' ||
	fail "a short path moved: $(screen | grep short)"
step_keys h
screen | grep -q '^ drivers/net/ethernet' || fail "scrolled past the start"
keys l l l l l
wait_match '^ drivers/~.*intel/e1000e/' || fail "l: $(screen | grep e1000e)"
keys h h
wait_match '^ drivers/net/ethernet' || fail "h again: $(screen | grep e1000e)"
keys j
wait_match '^ drivers/~.*intel/e1000e/' || fail "another file, still scrolled: $(screen | grep e1000e)"
tui_quit

ell=$(printf '\342\200\246')
TUI_LOCALE=C.UTF-8 TUI_W=100 TUI_H=24 tui_start
wait_screen "M short.c"
screen | grep -q "^ drivers/$ell.*intel/e1000e/" ||
	fail "UTF-8 ellipsis: $(screen | grep e1000e)"
tui_quit
