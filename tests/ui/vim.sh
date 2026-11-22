#!/bin/sh
# SPDX-License-Identifier: MIT
# Vim navigation by default, gg and G, Ctrl-F and Ctrl-B, Ctrl-E and
# Ctrl-Y, zt, zz and zb, H, M and L, 0 and $. g/ lists the search hits. A
# sequence that means nothing says so, and Esc after its first key cancels
# it.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

: >f.c
commit_all base
{
	printf 'long %0300d END\n' 0
	i=1; while [ $i -le 200 ]; do echo "l $i"; i=$((i + 1)); done
} >f.c

# the diff pane's rows on the screen, inside its frame
pane() {
	screen | cut -c33- | sed -n '/Unstaged: f.c/,/Staged: f.c/p' |
		sed '1d;$d' | sed '$d'
}
# the pane row (1 = top) with the cursor
cur_at() {
	pane | grep -n '^>' | head -1 | cut -d: -f1
}
rows() {
	pane | wc -l | tr -d ' '
}

tui_start
wait_screen 'M f.c'
keys Tab
wait_active 'Unstaged: f.c'
h=$(rows)

keys G
wait_screen '+l 200'
keys g g
wait_screen '+long'
[ "$(cur_at)" = 1 ] || fail "gg: $(cur_at)"

# Ctrl-F and Ctrl-B, a page down and back
step_keys C-f
cursor_row | grep -q -E "\+l ($((h - 3))|$((h - 2))|$((h - 1)))\$" ||
	fail "Ctrl-F: $(cursor_row) (pane of $h)"
step_keys C-b
[ "$(cur_at)" = 1 ] || fail "Ctrl-B: $(cur_at)"

# Ctrl-E moves the view a line, and the cursor keeps its line
step_keys j j j j j
line=$(cursor_row)
top=$(pane | sed -n 1p)
step_keys C-e
[ "$(cursor_row)" = "$line" ] || fail "Ctrl-E moved the cursor: $(cursor_row)"
[ "$(pane | sed -n 1p)" != "$top" ] || fail "Ctrl-E did not scroll"
step_keys C-y
[ "$(pane | sed -n 1p)" = "$top" ] || fail "Ctrl-Y"

# zt, zb and zz put the cursor line at the top, bottom and middle
step_keys : n 4 0 Enter z t
[ "$(cur_at)" = 1 ] || fail "zt: $(cur_at)"
step_keys z b
[ "$(cur_at)" = "$h" ] || fail "zb: $(cur_at) of $h"
step_keys z z
[ "$(cur_at)" = $(( (h - 1) / 2 + 1 )) ] || fail "zz: $(cur_at) of $h"

# H, M and L put the cursor at the top, middle and bottom of the view
step_keys H
[ "$(cur_at)" = 1 ] || fail "H: $(cur_at)"
step_keys L
[ "$(cur_at)" = "$h" ] || fail "L: $(cur_at) of $h"
step_keys M
[ "$(cur_at)" = $(( (h - 1) / 2 + 1 )) ] || fail "M: $(cur_at) of $h"

# $ and 0 on the long line
keys g g j '$'

wait_screen 'END'
keys 0
wait_gone 'END'

# g/ lists the search hits (Ctrl-F pages now)
keys / 'l 19' Enter g /
wait_screen 'matches for "l 19"'
keys Escape
wait_gone 'matches for'

# a sequence that means nothing, and Esc after g cancels
keys g q
wait_screen 'no key gq'
step_keys g Escape j
screen | grep -q 'no key' && fail "Esc did not cancel g"
tui_quit
exit 0
