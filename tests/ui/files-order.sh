#!/bin/sh
# SPDX-License-Identifier: MIT
# The Files list in git status's order, Staged, Unstaged (tracked files
# only), and Untracked last, folded to one row. Enter or l on that row
# unfolds it, and h or Enter folds it again. Moving skips the folded files
# and stops on the section rows. Space on the Untracked row stages all of
# its files.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\n' >staged.c
printf 'a\n' >mod.c
commit_all base
printf 'b\n' >staged.c
git add staged.c
printf 'b\n' >mod.c
mkdir sub
printf 'n\n' >new1.c
printf 'n\n' >sub/new2.c

tui_start
wait_screen "Untracked (2) [+]"
# the order of the sections and their counts
screen | awk '/Staged \(1\)/ { s = NR } /Unstaged \(1\)/ { u = NR }
	/Untracked \(2\)/ { n = NR }
	END { exit !(s && u && n && s < u && u < n) }' ||
	fail "sections not Staged, Unstaged, Untracked: $(screen)"
# folded, the untracked files are not listed
screen | grep -q 'new1\.c' && fail "folded Untracked lists its files"
# the cursor goes down onto the Untracked row and stops there
keys G
wait_fcur 'Untracked (2) \[+\]'
keys j
wait_fcur 'Untracked (2) \[+\]'
# Enter unfolds, with the files below it, and j reaches them
keys Enter
wait_screen "Untracked (2) [-]"
wait_screen "? new1.c"
wait_screen "new2.c"
keys j
wait_fcur 'new1\.c'
wait_screen "Unstaged: new1.c"
# h on a file scrolls, with nothing to fold, and on the row it folds
keys k h
wait_screen "Untracked (2) [+]"
screen | grep -q 'new1\.c' && fail "h did not fold"
# l unfolds, h folds, from the row
keys l
wait_screen "? new1.c"
keys Left
wait_gone "? new1.c"
# moving up from the row reaches the tracked files, Unstaged then Staged
keys k
wait_fcur 'M mod\.c'
keys k
wait_fcur 'Unstaged (1)'
keys k
wait_fcur 'M staged\.c'
# Space on the row stages its files, folded or not
keys G Space
wait_screen "staged 2 files"
[ "$(git diff --cached --name-only -- new1.c sub/new2.c | wc -l)" -eq 2 ] ||
	fail "Space on the Untracked row: $(git diff --cached --name-only)"
tui_quit
