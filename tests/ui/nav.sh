#!/bin/sh
# SPDX-License-Identifier: MIT
# Navigation. ] [ move by hunks, ) ( by blocks of changes, } { by files
# with changes in the active pane, and Ctrl-D and Ctrl-U by half pages.
# Motions extend a v range. A key stops a diff that has been loading for
# 300 ms, and the file then stays unloaded until gd. Under load that can be
# any diff, and every move to a file waits for its diff before the next
# key.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1
while [ $i -le 80 ]; do echo "line $i"; i=$((i + 1)); done >a.c
printf 'b\n' >b.c
printf 'c\n' >c.c
printf 'd\n' >d.c
i=1
while [ $i -le 100 ]; do echo "z $i"; i=$((i + 1)); done >z.c
commit_all base
# a.c has two hunks, the first with two blocks of changes
awk '{ if (NR == 5) print "FIVE"; else if (NR == 8) print "EIGHT";
	else if (NR == 60) print "SIXTY"; else print }' a.c >a.new &&
	mv a.new a.c
printf 'B\n' >b.c
printf 'C\n' >c.c
git add c.c		# c.c has staged changes only
printf 'D\n' >d.c
sed "s/z/Z/" z.c >z.new && mv z.new z.c

tui_start
wait_screen "M a.c"
# c.c, staged, is first as in git status, and a.c is the first unstaged,
# past the Unstaged section's row
keys j j
wait_screen "Unstaged: a.c"
wait_screen "+FIVE"
keys Tab
wait_screen "> Unstaged: a.c"
keys ')'
wait_cursor '5       -line 5$'
cursor_row | grep -q '5       -line 5$' || fail ") -> $(cursor_row)"
keys ')'
wait_cursor '8       -line 8$'
cursor_row | grep -q '8       -line 8$' || fail ") ) -> $(cursor_row)"
keys ']'
wait_cursor '^> *@@ -57'
cursor_row | grep -q '^> *@@ -57' || fail "] -> $(cursor_row)"
keys ']'
wait_screen "no next hunk"
keys '['
wait_cursor '^> *@@ -2'
cursor_row | grep -q '^> *@@ -2' || fail "[ -> $(cursor_row)"
keys '('
wait_screen "no previous change"

# motions extend a range, v on line 5's block, ) to line 8's, Space
keys ')' v ')' j Space
wait_index a.c "$(awk '{ if (NR == 5) print "FIVE"; else if (NR == 8)
	print "EIGHT"; else print }' a.c | sed 's/SIXTY/line 60/')\n"

# } { move between files with unstaged changes, a.c b.c d.c z.c but not c.c
keys '}'
wait_screen "> Unstaged: b.c"
wait_cursor '1 *-b$'
cursor_row | grep -q '1 *-b$' || fail "} lands on the change: $(cursor_row)"
keys '}'
wait_screen "> Unstaged: d.c"
wait_screen "1 +D"
keys '}'
wait_screen "> Unstaged: z.c"
wait_screen "1       -z 1"
keys '}'
wait_screen "no next file with unstaged changes"
keys '{' '{' '{'
wait_screen "> Unstaged: a.c"
wait_screen "+FIVE"
# in the Staged pane, } goes by the Staged section
keys Tab
wait_screen "> Staged: a.c"
keys '}'
wait_screen "> Staged: c.c"
wait_screen "1 +C"

# half pages, in z.c's 200-line diff
keys BTab
wait_screen "> Unstaged: c.c"
keys '{'
wait_screen "> Unstaged: z.c"
wait_screen "1       -z 1"
step_keys g g C-d
r1=$(cursor_row)
[ -n "$r1" ] || fail "no cursor"
step_keys C-d
[ "$(cursor_row)" != "$r1" ] || fail "Ctrl-D did not move"
step_keys C-u
[ "$(cursor_row)" = "$r1" ] || fail "Ctrl-U: $(cursor_row), want $r1"
keys C-u
wait_cursor '^> *@@ -1'
cursor_row | grep -q '^> *@@ -1' || fail "Ctrl-U -> $(cursor_row)"
tui_quit
