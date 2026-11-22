#!/bin/sh
# SPDX-License-Identifier: MIT
# Long lines scroll sideways with h and l. '<' marks text hidden on the
# left and '>' text going on past the right edge, a single angle quote in
# UTF-8. A diff over carve.diffLimit lines is not shown, and Space then
# takes the whole file.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'short\n' >long.c
i=1
while [ $i -le 30 ]; do echo "old $i"; i=$((i + 1)); done >big.c
commit_all base
{ echo "short"
  printf 'start'
  i=1; while [ $i -le 40 ]; do printf -- '-%02d-' $i; i=$((i + 1)); done
  echo "END"; } >long.c
i=1
while [ $i -le 30 ]; do echo "new $i"; i=$((i + 1)); done >big.c
git config carve.diffLimit 50

tui_start
wait_screen "M big.c"
keys j
wait_screen "Unstaged: long.c"
keys Tab
wait_active "Unstaged: long.c"
wait_screen "+start-01-"
screen | grep -q 'END' && fail "the end of the long line is visible"
screen | grep -q -- '-[0-9]*>x$' || fail "no mark on the right: $(screen | grep start)"
keys l
wait_gone "+start-01-"
screen | grep -q ' 2<+-' || fail "no scroll marker: $(screen | grep ' 2')"
keys l l l l l l
wait_screen "END"
screen | grep -q 'END>$' && fail "a right mark after the end: $(screen | grep END)"
keys h h h h h h h h h h
wait_screen "+start-01-"
screen | grep -q ' 2<+' && fail "marker left at the left edge"

# over the limit, a note, and Space stages the whole file
keys '{'
wait_active "Unstaged: big.c"
wait_screen "(diff of more than 50 lines, over carve.diffLimit; Space takes"
keys Space
wait_index big.c "$(cat big.c)\n"
tui_quit
