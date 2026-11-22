#!/bin/sh
# SPDX-License-Identifier: MIT
# / in the Files pane filters the list by path as it is typed, with smart
# case, in every section, and a rename by either path. The folded Untracked
# section shows its matches. Enter keeps the filter, Esc in the prompt puts
# back the one before, and Esc in the pane clears it. / in a diff pane
# still searches the diffs.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

mkdir -p drivers/net net
for p in drivers/net/e1000.c drivers/net/ice.c net/core.c README; do
	printf 'a\n' >"$p"
done
commit_all base
for p in drivers/net/e1000.c drivers/net/ice.c net/core.c README; do
	printf 'b\n' >"$p"
done
git add net/core.c
printf 'n\n' >drivers/net/Ice_new.c
printf 'n\n' >scratch.txt

tui_start
wait_screen "Untracked (2) [+]"
# as it is typed, "i" keeps the paths with an i, and "ice" two of them
keys /
wait_screen "filter files:"
keys i c e
wait_active "Files: /ice"
wait_gone "README"
wait_gone "core.c"
screen | grep -q 'e1000\.c' && fail "e1000.c passes /ice"
# with smart case, lower case matches Ice_new.c, unfolded under Untracked
wait_screen "Ice_new.c"
wait_screen "Untracked (1) [-]"
# Enter keeps it, with the cursor on a file that passes
keys Enter
wait_screen "2 files match /ice"
wait_screen "Unstaged: drivers/net/ice.c"
# with an upper case letter, case matters
keys / I
wait_active "Files: /I"
wait_gone "  ice.c"
wait_screen "Ice_new.c"
# Esc in the prompt brings back the filter before (/ice)
keys Escape
wait_active "Files: /ice"
wait_screen "  ice.c"
# Esc in the pane clears it, and the Untracked section is folded again
keys Escape
wait_screen "filter cleared"
wait_screen "README"
wait_screen "Untracked (2) [+]"
screen | grep -q 'Ice_new' && fail "Untracked not folded again"
# when nothing matches it tells
keys / z z z Enter
wait_screen "No file matches the filter"
keys Escape
wait_screen "README"
# / in a diff pane searches the diffs, the list stays whole
keys Tab / b Enter
wait_screen "matches in unstaged changes"
screen | grep -q 'Files: /' && fail "diff search filtered the list"
tui_quit
