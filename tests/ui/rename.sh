#!/bin/sh
# SPDX-License-Identifier: MIT
# Renames. A file moved in the work tree with a plain mv is one entry "R
# old -> new" whose diff is the content change. Staging the entry stages
# only the move, and a line stages the move and the line. A staged rename
# unstages both halves, and d discards both halves.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1
while [ $i -le 20 ]; do echo "line $i"; i=$((i + 1)); done >old.c
commit_all base
mv old.c new.c
sed 's/line 5$/LINE 5/; s/line 15$/LINE 15/' new.c >new.tmp && mv new.tmp new.c

tui_start
wait_screen "R old.c -> new.c"
screen | grep -q ' D old.c' && fail "old.c listed on its own"
keys Tab
wait_active "Unstaged: old.c -> new.c"
wait_screen "+LINE 5"
wait_screen "+LINE 15"
# a line stages the move and that line
keys / "LINE 5" Enter k v j Space
wait_screen "staged 2 lines"
[ "$(git ls-files -s old.c)" = "" ] || fail "old.c still in the index"
git show :new.c | grep -q '^LINE 5$' || fail "line not staged"
git show :new.c | grep -q '^LINE 15$' && fail "other line staged"
wait_screen "Staged: old.c -> new.c"
wait_screen "M new.c"
# the Staged pane shows the content change against the old file
keys Tab
wait_screen "+LINE 5"
# Files pane, staged section (the first): Enter unstages both halves
keys Tab Tab
wait_active "Files"
keys g g Enter
wait_screen "unstaged new.c"
[ -n "$(git ls-files -s old.c)" ] || fail "old.c not back in the index"
[ -z "$(git ls-files -s new.c)" ] || fail "new.c still in the index"
# Enter on the work-tree rename stages only the move, once the list has it
# again, since the untracked scan may still come back meanwhile
wait_screen "R old.c -> new.c"
wait_gone "scanning untracked"
keys g g j Enter
wait_screen "staged the move old.c -> new.c; its edits stay unstaged"
[ "$(git show :new.c)" = "$(git show HEAD:old.c)" ] || fail "move with edits"
keys Tab Tab Enter
wait_screen "unstaged new.c"
tui_quit

# d on the rename discards both halves after asking, and u brings them back
tui_start
wait_screen "R old.c -> new.c"
keys j d
wait_screen "Discard the rename old.c -> new.c?"
keys y
wait_gone "old.c -> new.c"
[ -e new.c ] && fail "new.c not deleted"
[ "$(cat old.c)" = "$(git show HEAD:old.c)" ] || fail "old.c not back"
keys u
wait_screen "R old.c -> new.c"
grep -q '^LINE 15$' new.c || fail "undo lost the edits"
tui_quit
