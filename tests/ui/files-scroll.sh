#!/bin/sh
# SPDX-License-Identifier: MIT
# Ctrl-E and Ctrl-Y in the Files pane scroll the list a line at a time, and
# the cursor stays on a file that is in view, never on a section header.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=10; while [ $i -le 69 ]; do echo a >f$i.c; i=$((i + 1)); done
commit_all base
i=10; while [ $i -le 69 ]; do echo b >>f$i.c; i=$((i + 1)); done

TUI_H=24; export TUI_H
tui_start
wait_screen '>M f10.c'
# with the cursor on the first file scrolled off, it moves to the first in
# view
keys C-e C-e C-e
# f10.c is gone after two of the three keys, wait for the third
wait_screen '>M f12.c'
wait_screen 'Unstaged: f12.c'
# back up, the list as it was and the cursor where it went
keys C-y C-y C-y
wait_screen 'Unstaged (60)'
screen | grep -q ' M f10.c' || fail "Ctrl-Y: $(screen | head -n 12)"
screen | grep -q '>M f12.c' || fail "cursor moved back: $(screen | head -n 12)"
tui_quit
