#!/bin/sh
# SPDX-License-Identifier: MIT
# Space stages/unstages lines, v selects ranges, a selects a hunk, Enter
# stages whole files, and untracked files can be staged line by line.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\nb\nc\nd\ne\nf\n' >f.c
commit_all base
printf 'a\nB\nc\nd\nE\nf\ng\n' >f.c
printf 'n1\nn2\nn3\n' >new.c
before="$(file_hash f.c) $(file_hash new.c)"

tui_start
keys Tab
wait_active "Unstaged: f.c"

# Space on +B (new line 2) stages only that line
keys : 2 Enter Space
wait_index f.c 'a\nb\nB\nc\nd\ne\nf\n'
wait_screen "staged 1 line"
screen | grep -q 'Staged (1)' || fail "Staged section"

# a range from -b (old 2) with v down to +E stages -b, -e and +E
step_keys : o 2 Enter v j j j j j
[ "$(screen | cut -c34- | grep -c '^\*')" -ge 3 ] || fail "range markers"
keys Space
wait_index f.c 'a\nB\nc\nd\nE\nf\n'

# a selects the (remaining) hunk, Space stages it
keys a Space
wait_index f.c 'a\nB\nc\nd\nE\nf\ng\n'
wait_screen "(no unstaged changes)"

# in the Staged pane Space unstages, and a hunk header unstages the hunk
keys Tab
wait_active "Staged: f.c"
keys : 7 Enter Space
wait_index f.c 'a\nB\nc\nd\nE\nf\n'
keys g g Space
wait_index f.c 'a\nb\nc\nd\ne\nf\n'

# Enter on a file in the Files pane stages it whole
keys Tab Tab
wait_active "Files"
keys g g Enter
wait_index f.c 'a\nB\nc\nd\nE\nf\ng\n'
wait_screen "Staged (1)"
keys Enter
wait_screen "Unstaged (1)"
wait_screen "Untracked (1) [+]"
[ "$(index_blob f.c)" = "$(git rev-parse HEAD:f.c)" ] || fail "unstage file"

# for an untracked file, Space on a line makes it intent-to-add and stages
# it
keys G l j
wait_screen "Unstaged: new.c"
keys Tab : 2 Enter Space
wait_index new.c 'n2\n'
[ "$(short_status | grep new.c)" = "AM new.c" ] || fail "new.c status"

[ "$(file_hash f.c) $(file_hash new.c)" = "$before" ] ||
	fail "work tree modified"
tui_quit
