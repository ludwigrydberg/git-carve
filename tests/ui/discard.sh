#!/bin/sh
# SPDX-License-Identifier: MIT
# d discards after asking, the selected lines in the Unstaged pane or the
# whole file in the Files pane, and u brings them back.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\nb\nc\n' >f.c
commit_all base
printf 'A\nb\nC\n' >f.c
printf 'scratch\n' >tmp.txt
orig=$(git hash-object f.c)

tui_start
wait_screen "M f.c"
keys Tab : n3 Enter d
wait_screen "Discard 1 changed line from the work tree? (u undoes it) [y/N]"
keys n
wait_screen "not discarded"
[ "$(git hash-object f.c)" = "$orig" ] || fail "n discarded"
keys v k d
wait_screen "Discard 2 changed lines"
keys y
wait_screen "discarded 2 lines"
[ "$(cat f.c)" = "$(printf 'A\nb\nc')" ] || fail "content: $(cat f.c)"
keys u
wait_screen "undid discard f.c"
[ "$(git hash-object f.c)" = "$orig" ] || fail "undo: $(cat f.c)"

# the Staged pane refuses
keys Tab d
wait_screen "discard works on unstaged changes"
close_box

# in the Files pane an untracked file is deleted after asking
keys Tab Tab
wait_screen "Untracked (1) [+]"
keys G l j d
wait_screen "Discard all unstaged changes of tmp.txt (an untracked file: it is deleted)?"
keys y
wait_screen "discarded the changes of tmp.txt"
screen | grep -q "no such path" && fail "an error for the deleted file"
wait_gone "? tmp.txt"
[ -e tmp.txt ] && fail "tmp.txt still there"
keys u
wait_screen "undid discard-file tmp.txt"
[ "$(cat tmp.txt)" = scratch ] || fail "tmp.txt not restored"
tui_quit
