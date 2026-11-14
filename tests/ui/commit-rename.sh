#!/bin/sh
# SPDX-License-Identifier: MIT
# A file a commit of the Series pane renames is one entry of its section,
# "R old -> new", its diff between the old and the new content.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1; while [ $i -le 20 ]; do echo "l $i"; i=$((i + 1)); done >old.c
commit_all base
git branch -q up
git branch -q --set-upstream-to=up
git mv old.c new.c
sed 's/^l 10$/l 10 renamed/' new.c >n.tmp && mv n.tmp new.c
commit_all "move and change"

tui_start
wait_screen 'Commit HEAD (1)'
wait_screen 'R old.c -> new.c'
# the content diff, not a whole new file
wait_screen '+l 10 renamed'
wait_screen '-l 10'
screen | grep -q '+l 1$' && fail "shown as a new file: $(screen)"
tui_quit
exit 0
