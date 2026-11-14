#!/bin/sh
# SPDX-License-Identifier: MIT
# R in the Files pane runs git rm on the file after asking, and u brings it
# back.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\n' >a.c
printf 'b\n' >b.c
commit_all base
printf 'a changed\n' >a.c

tui_start
wait_screen 'M a.c'
keys R
wait_screen 'Delete a.c and stage its deletion?'
keys n
wait_screen 'not removed'
[ -e a.c ] || fail "removed on n"
keys R
wait_screen 'Delete a.c and stage its deletion?'
keys y
wait_screen 'removed a.c (staged)'
wait_screen 'D a.c'
[ -e a.c ] && fail "a.c still there"
keys u
wait_screen 'undid remove'
[ "$(cat a.c)" = "a changed" ] || fail "undo: $(cat a.c 2>&1)"
tui_quit
