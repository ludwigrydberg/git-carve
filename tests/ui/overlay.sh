#!/bin/sh
# SPDX-License-Identifier: MIT
# Text overlays. A long one says which lines it shows, as the help does,
# and D shows the commit as a coloured diff. o (zoom) puts an overlay on
# the whole screen, and the next one opens the same way.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

esc=$(printf '\033')
# the lines of the screen (with SGR codes) that contain text
i=1
while [ $i -le 60 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
git branch -q up
git branch -q --set-upstream-to=up
sed 's/^l 30$/l 30 changed/' a.c >a.tmp && mv a.tmp a.c
i=1
while [ $i -le 60 ]; do echo "n $i"; i=$((i + 1)); done >b.c
commit_all P1
git commit -q --amend -m P1 -m "Why it changes." -m "Signed-off-by: T <t@t>"
sed 's/^l 30 changed$/l 30 again/' a.c >a.tmp && mv a.tmp a.c
commit_all P2

TUI_H=24
tui_start
wait_screen 'Commit HEAD'
keys J
wait_screen 'Commit ~1'
keys D
wait_screen "PgUp/PgDn: scroll (lines 1-"
# the message as git show colours it, the id yellow (33), the subject bold
# (1), a trailer cyan (36), and the + of the stat green
row_sgr "commit " | grep -q "$esc\[[0-9;]*33m" || fail "id: $(row_sgr "commit " | od -c | head -n 3)"
row_sgr "P1" | grep -q "1mP1" || fail "subject not bold: $(row_sgr "P1" | od -c | head -n 20)"
row_sgr "Signed-off-by: T" | grep -q "$esc\[[0-9;]*36m" || fail "trailer"
row_sgr "b.c | 60" | grep -q "$esc\[[0-9;]*32m+" || fail "stat +"
# (from the text on, since the Files pane shows beside the overlay)
row_sgr "Why it changes." | sed 's/.*Why it/Why it/' |
	grep -q "$esc\[[0-9;]*3[0-9]m" && fail "body coloured"
screen | grep -q 'scroll (lines 1-[0-9]* of [0-9]*)  o: full screen  other keys: close' ||
	fail "footer: $(screen | grep scroll)"
keys PgDn
wait_gone "scroll (lines 1-"
screen >"$tmp/scr"
screen | grep -q 'scroll (lines [0-9][0-9]*-[0-9]* of' || fail "PgDn"
# the added lines in green (SGR 32), the removed in red (31)
row_sgr "+l 30 changed" | grep -q "$esc\[[0-9;]*32m" ||
	fail "+ not green: $(row_sgr "+l 30 changed" | od -c | head -n 5)"
row_sgr "-l 30" | grep -q "$esc\[[0-9;]*31m" || fail "- not red"
# o takes the whole screen, with the frame in the first row and column
keys o
wait_screen "o: smaller"
screen | head -n 1 | grep -q '^[l+]' || fail "not full screen: $(screen | head -n 2)"
keys x
wait_gone "other keys: close"
# the next overlay opens as large, and o makes it smaller again
keys D
wait_screen "o: smaller"
keys o
wait_screen "o: full screen"
screen | head -n 1 | grep -q '^[l+]' && fail "still full screen"
keys x
wait_gone "other keys: close"

tui_quit
exit 0
