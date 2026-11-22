#!/bin/sh
# SPDX-License-Identifier: MIT
# Space after Space goes through a block of changes. After a staged line
# the cursor is on the next changed line, whether the staged one left the
# diff (a removal) or stayed in it as context (an addition).
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'keep\nold a\nold b\nkeep 2\n' >f.c
commit_all base
printf 'keep\nnew A\nnew B\nkeep 2\n' >f.c

tui_start
wait_screen "M f.c"
keys Tab
wait_active "Unstaged: f.c"
keys ')'
wait_cursor '-old a$' || fail "start: $(cursor_row)"
keys Space
wait_index f.c 'keep\nold b\nkeep 2\n'
wait_cursor '-old b$' || fail "after a removal: $(cursor_row)"
keys Space
wait_index f.c 'keep\nkeep 2\n'
wait_cursor '+new A$' || fail "after the second removal: $(cursor_row)"
keys Space
wait_index f.c 'keep\nnew A\nkeep 2\n'
# new A stays as context, and the cursor goes on to new B
wait_cursor '+new B$' || fail "after an addition: $(cursor_row)"
keys Space
wait_index f.c 'keep\nnew A\nnew B\nkeep 2\n'
tui_quit
exit 0
