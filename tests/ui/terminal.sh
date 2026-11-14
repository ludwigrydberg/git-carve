#!/bin/sh
# SPDX-License-Identifier: MIT
# Terminal handling, a small 80x24 terminal, resizing down to "too small"
# and back, a resize while the editor runs, and Ctrl-Z and fg.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'one\ntwo\nthree\n' >f.c
printf 'x\n' >g.c
commit_all base
printf 'one\nTWO\nthree\nfour\n' >f.c
printf 'y\n' >g.c

# --- 80x24, everything fits and works
TUI_W=80 TUI_H=24 tui_start
wait_screen "M f.c"
wait_screen "Unstaged: f.c"
wait_screen "Staged: f.c"
[ "$(screen_row 'Tab: pane')" = 23 ] || fail "status line at 80x24"
keys Tab : 2 Enter Space
wait_index f.c 'one\ntwo\nTWO\nthree\n'
keys '?'
wait_screen "Keys (bind with git config"
wait_screen "j/k: scroll"
screen | grep -q "Ctrl-L" && fail "help fits at 80x24?"
keys j j
wait_screen "(3-"
keys G
wait_screen "gives up."
keys g g
wait_screen "(1-"
keys q
wait_gone "j/k: scroll"
# a list to pick from at 80x24, f in the Staged pane
keys Tab t
wait_screen "Move the change into which commit?"
wait_screen "Esc: cancel"
keys Escape
wait_gone "Move the change into which commit?"
keys BTab

# wait_row <text> <row>: until text is on that row (after a resize the
# old screen, with the text on its old row, can still be there)
wait_row() {
	_n=$WAIT_TRIES
	while [ "$(screen_row "$1")" != "$2" ]; do
		_n=$((_n - 1))
		[ "$_n" -gt 0 ] ||
			fail "'$1' on row $(screen_row "$1"), not $2: $(screen)"
		sleep 0.1
	done
}

# --- resize bigger, too small (keys ignored), and back
tui_resize 120 40
wait_row 'Tab: pane' 39
tui_resize 30 8
wait_screen "terminal too small"
wait_screen "need 40x10, have 30x8"
before=$(index_blob f.c)
step_keys Space j Space c
[ "$(index_blob f.c)" = "$before" ] || fail "keys acted while too small"
tui_resize 100 30
wait_gone "terminal too small"
wait_row 'Tab: pane' 29
keys : 4 Enter Space
wait_index f.c 'one\ntwo\nTWO\nthree\nfour\n'
tui_quit

# --- a resize while the editor has the terminal
cat >"$tmp/editor" <<X
#!/bin/sh
tmux -L "carve-test-$$" resize-window -t t -x 90 -y 26
sleep 0.3
exit 0
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"
tui_start
wait_screen "M f.c"
keys e
# the status line on the last row of the new size once the editor is done.
# Wait for it, since the editor may take a while on a busy machine.
_n=$WAIT_TRIES
while [ "$(screen_row 'Tab: pane')" != 25 ] && [ "$_n" -gt 0 ]; do
	sleep 0.1
	_n=$((_n - 1))
done
[ "$(screen_row 'Tab: pane')" = 25 ] ||
	fail "size change during the editor not picked up: $(screen_row 'Tab: pane')"
tui_quit
export GIT_EDITOR=true

# --- Ctrl-Z and fg from an interactive shell
TMUX_SOCK="carve-test-$$"
tmux -L "$TMUX_SOCK" -f /dev/null new-session -d -x 100 -y 30 -s t \
	"PS1='SHELL> ' LC_ALL=C TERM=screen sh -i" ||
	fail "tmux shell"
trap 'tmux -L "$TMUX_SOCK" kill-server 2>/dev/null; cleanup_tmp' EXIT
wait_screen "SHELL>"
keys "cd '$PWD' && '$CARVE' 2>>'$tmp/tui.err'" Enter
wait_screen "M f.c"
keys C-z
wait_gone "Tab: pane"
wait_screen "SHELL>"
# resize while stopped, then come back
tui_resize 90 28
keys fg Enter
wait_row 'Tab: pane' 27
keys g g Enter
wait_screen "Staged (1)"
keys q
wait_screen "SHELL>"
check_sanitizers
