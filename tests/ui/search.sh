#!/bin/sh
# SPDX-License-Identifier: MIT
# Search. / searches the diffs of all files, and n and N go on into the
# next or previous file and wrap, with smart case. Ctrl-F lists every hit
# to jump to. The Staged pane searches the staged diffs.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

for f in a b c d; do printf 'one\ntwo\n' >$f.c; done
commit_all base
printf 'one\ntwo\nneedle a\n' >a.c
printf 'one\nNeedle b\ntwo\n' >b.c
printf 'one\ntwo x\n' >c.c
printf 'needle d1\none\ntwo\nneedle d2\n' >d.c
git add c.c
printf 'one\ntwo x\nneedle c\n' >c.c	# staged: no needle; unstaged: one

tui_start
wait_screen "M a.c"
# c.c, staged, is first as in git status, and a.c is the first unstaged,
# past the Unstaged section's row
keys j j
wait_screen "Unstaged: a.c"
keys Tab / needle Enter
wait_screen "5 matches in unstaged changes"
wait_active "Unstaged: a.c"
cursor_row | grep -q '+needle a$' || fail "a: $(cursor_row)"
keys n
wait_active "Unstaged: b.c"
cursor_row | grep -q '+Needle b$' || fail "smart case: $(cursor_row)"
keys n
wait_active "Unstaged: c.c"
keys n
wait_active "Unstaged: d.c"
cursor_row | grep -q '+needle d1$' || fail "d1: $(cursor_row)"
keys n
cursor_row | grep -q '+needle d2$' || fail "d2: $(cursor_row)"
keys n
wait_screen "search wrapped to the top"
wait_active "Unstaged: a.c"
keys N
wait_screen "search wrapped to the bottom"
wait_active "Unstaged: d.c"
keys N
wait_cursor '+needle d1$'
cursor_row | grep -q '+needle d1$' || fail "N: $(cursor_row)"

# with an upper case letter the case is exact
keys / Needle Enter
wait_screen "1 match in unstaged changes"
wait_active "Unstaged: b.c"

# Ctrl-F shows the list, choose d.c's second hit
keys / needle Enter g /
wait_screen '5 matches for "needle" in unstaged changes'
# framed like the help, with nothing of the panes behind at its edges
screen | grep -q 'x 5 matches for "needle"' || fail "no frame at the title"
screen | grep -q 'lqqqqqqqqqq.*k' || fail "no top frame"
screen | grep -q 'mqqqqqqqqqq.*j' || fail "no bottom frame"
wait_screen "d.c:n4: +needle d2"
# coloured, the place as git grep colours it (36) and the line as a diff
esc=$(printf '\033')
sgr_row() {
	tmux -L "$TMUX_SOCK" capture-pane -e -p -t t | grep -F -- "$1"
}
sgr_row "+needle d2" | grep -q "$esc\[[0-9;]*36md\.c:n4: " ||
	fail "place: $(sgr_row "+needle d2" | od -c | head -n 4)"
sgr_row "+needle d2" | grep -q "$esc\[[0-9;]*32m+needle d2" || fail "text"
keys G Enter
wait_gone "5 matches for"
wait_active "Unstaged: d.c"
cursor_row | grep -q '+needle d2$' || fail "Ctrl-F: $(cursor_row)"

# the Staged pane searches the staged diffs (c.c's "two x")
keys Tab / "two x" Enter
wait_screen "1 match in staged changes"
wait_active "Staged: c.c"
tui_quit
