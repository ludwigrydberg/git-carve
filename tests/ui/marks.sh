#!/bin/sh
# SPDX-License-Identifier: MIT
# m marks lines that are not next to each other, to take one old and new
# pair out of a block where git lists both removals before both additions,
# keeping the line order.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'x\ntimeout = 100;\nprobed devcie\ny\n' >f.c
commit_all base
printf 'x\ntimeout = 500;\nprobed device\ny\n' >f.c

tui_start
keys Tab
wait_active "Unstaged: f.c"
# -timeout (old 2) and +timeout (new 2) are rows 1 and 3 of the block
keys : o 2 Enter m
wait_screen "1 line marked"
keys : 2 Enter m
wait_screen "2 lines marked"
[ "$(screen | cut -c34- | grep -c '^+')" = 2 ] || fail "mark markers"
# the title counts them, and the cursor on a marked line shows both
wait_screen "Unstaged: f.c  [2 marked]"
step_keys k
screen | cut -c33- | grep -q '^>+' || fail "cursor and mark: $(screen)"
keys Space
wait_index f.c 'x\ntimeout = 500;\nprobed devcie\ny\n'
wait_screen "staged 2 lines"

# Esc clears marks
keys : o 3 Enter m
wait_screen "1 line marked"
step_keys Escape
[ "$(screen | cut -c34- | grep -c '^+')" = 0 ] || fail "Esc kept marks"

# gm marks a changed line and its pair in one key (the old side of the pair
# here), and again unmarks both. Then the pair is staged.
keys : o 3 Enter
wait_screen "gm: mark both lines"
keys g m
wait_screen "2 lines marked"
[ "$(screen | cut -c34- | grep -c '^+')" = 2 ] || fail "gm: $(screen)"
keys : o 3 Enter g m
wait_screen "0 lines marked"
# a context line has no pair
keys : n 1 Enter
wait_gone "gm: mark both lines"
keys g m
wait_screen "gm marks a changed line and its pair"
keys : n 3 Enter g m
wait_screen "2 lines marked"
keys Space
wait_index f.c 'x\ntimeout = 500;\nprobed device\ny\n'
tui_quit
