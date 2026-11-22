#!/bin/sh
# SPDX-License-Identifier: MIT
# Key bindings from carve.keys.<action>: a remapped key acts, the replaced
# default no longer does, help shows the user's keys, and a bad config is
# reported in the status line without breaking the defaults.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\nb\nc\n' >f.c
commit_all base
printf 'a\nB\nc\n' >f.c

git config carve.keys.stage y
git config carve.keys.next-pane "Tab F2"
git config carve.keys.quit Ctrl-Q
tui_start
wait_screen "M f.c"
wait_screen "Tab: pane  y: stage/unstage"
wait_screen "Ctrl-Q: quit"
keys F2
wait_active "Unstaged: f.c"
step_keys : n2 Enter Space
assert_index_text f.c 'a\nb\nc\n'
keys y
wait_index f.c 'a\nb\nB\nc\n'
keys '?'
wait_screen "Keys (bind with git config"
# next-pane is under "Panes and the view", pages down
_n=5
until screen | grep -q -F "Tab, F2"; do
	_n=$((_n - 1))
	[ "$_n" -gt 0 ] || { screen >&2; fail "next-pane: no Tab, F2"; }
	step_keys PgDn
done
keys Escape
wait_gone "Keys (bind with git config"
# q is free now, and nothing happens
step_keys q
tmux -L "$TMUX_SOCK" has-session -t t 2>/dev/null || fail "q still quits"
keys C-q
_n=$WAIT_TRIES
while tmux -L "$TMUX_SOCK" has-session -t t 2>/dev/null; do
	_n=$((_n - 1))
	[ "$_n" -gt 0 ] || { screen >&2; fail "Ctrl-Q did not quit"; }
	sleep 0.1
done
check_sanitizers

# a broken config is warned about, and the defaults stay
git config --unset carve.keys.quit
git config carve.keys.nosuch y
tui_start
wait_screen "unknown action 'nosuch'"
close_box
keys Tab
wait_active "Unstaged: f.c"
tui_quit
