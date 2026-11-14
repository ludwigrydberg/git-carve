#!/bin/sh
# SPDX-License-Identifier: MIT
# f while splitting HEAD puts the selected lines into the commit below the
# split, after asking.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1; while [ $i -le 20 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
sed 's/^l 5$/l 5 P2/' a.c >a.tmp && mv a.tmp a.c
commit_all P2
sed 's/^l 15$/l 15 P3/; s/^l 5 P2$/l 5 P2 fixed/' a.c >a.tmp && mv a.tmp a.c
commit_all P3
carve split >/dev/null

tui_start
wait_screen "SPLIT"
keys Tab
wait_screen "+l 5 P2 fixed"
keys / "P2 fixed" Enter k v j t
wait_screen "the commit below the split? [y/N]"
keys y
wait_screen 'moved into "P2"'
git show HEAD | grep -q '^+l 5 P2 fixed$' || fail "not in P2"
git diff | grep -q 'P2 fixed' && fail "still unstaged"
keys q
wait_screen "quit anyway"
keys y
_n=$WAIT_TRIES
while tmux -L "$TMUX_SOCK" has-session -t t 2>/dev/null; do
	_n=$((_n - 1))
	[ "$_n" -gt 0 ] || fail "did not quit"
	sleep 0.1
done
check_sanitizers
