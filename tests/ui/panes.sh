#!/bin/sh
# SPDX-License-Identifier: MIT
# Pane cycling, the old/new line-number gutter, :N jumps and search.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1
while [ $i -le 60 ]; do echo "line $i"; i=$((i + 1)); done >f.c
commit_all base
awk '{ if (NR == 2) print "TWO"; else if (NR == 50) print "FIFTY";
	else print } END { print "sixty-one" }' f.c >"$tmp/f" && cp "$tmp/f" f.c
printf 'x\n' >new.c

tui_start
wait_screen "> Files"
wait_screen "Unstaged (1)"
wait_screen "Untracked (1) [+]"
screen | grep -q '^>M f.c' || fail "f.c not selected first"

# the gutter has old and new numbers, blank for the absent side
screen | grep -q '     2       -line 2$' || fail "gutter for removed line"
screen | grep -q '           2 +TWO$' || fail "gutter for added line"
screen | grep -q '     3     3  line 3$' || fail "gutter for context"

# pane cycling, Files, Unstaged, Staged, Series, Files, and back
keys Tab
wait_screen "> Unstaged: f.c"
keys Tab
wait_screen "> Staged: f.c"
keys Tab
wait_screen "> Series"
keys Tab
wait_screen "> Files"
keys BTab BTab
wait_screen "> Staged: f.c"
keys BTab
wait_screen "> Unstaged: f.c"

# :N jumps to the new-side line, :-N to the old side
keys : 5 0 Enter
wait_cursor '      50 +FIFTY'
cursor_row | grep -q '      50 +FIFTY' || fail ":50 -> $(cursor_row)"
keys : o 5 0 Enter
wait_cursor '50       -line 50'
cursor_row | grep -q '50       -line 50' || fail ":-50 -> $(cursor_row)"
keys : 9 9 9 Enter
wait_screen "not in the diff"

# search in the diff pane
keys g g / s i x t y Enter
wait_cursor 'sixty-one'
cursor_row | grep -q 'sixty-one' || fail "search -> $(cursor_row)"

# in the Files pane j goes to the folded Untracked row, l unfolds it, and j
# selects the untracked file, whose diff is shown
keys BTab
wait_screen "> Files"
keys j l j
wait_screen "Unstaged: new.c"
screen | grep -q '^> ? new.c' || screen | grep -q '^>? new.c' ||
	fail "new.c not selected"
wait_screen '           1 +x'

# help overlay
keys '?'
wait_screen "Keys (bind with git config"
# grouped, a bold title per kind of work with its actions under it, the
# actions that change something first, moving around last
screen | grep -q ' x Staging and editing  *x$' || fail "no Staging section"
screen | sed -n '/Keys (bind/,/Selecting lines/p' | grep -q ' x Moving  *x$' &&
	fail "Moving before the actions"
tmux -L "$TMUX_SOCK" capture-pane -e -p -t t |
	grep -F 'Staging and editing' |
	grep -q "$(printf '\033')\[[0-9;]*1m" || fail "title not bold"
# on until the Series section is whole on the screen
n=0
until screen | grep -q ' x   gS  *hide the Series pane' &&
      screen | grep -q 'The Series pane'; do
	step_keys j
	n=$((n + 1))
	[ $n -lt 80 ] || fail "no Series section in the help"
done
screen | sed -n '/The Series pane/,/^ *x *x$/p' |
	grep -q ' x   gS  *hide the Series pane' || fail "gS not under Series"
keys G
wait_screen 'X gives up.'	# the last line
screen | grep -q ' x Moving  *x$' || fail "Moving not at the end"
screen | grep -q ' x Other  *x$' && fail "an action without a section"
keys q
wait_gone "Keys (bind with git config"
tui_quit
