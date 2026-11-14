#!/bin/sh
# SPDX-License-Identifier: MIT
# git carve <base>.. shows your branch from that base in the Series pane,
# with the divider below it. A range ending elsewhere than HEAD is refused.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1
while [ $i -le 5 ]; do
	echo "c$i" >"f$i.c"
	commit_all "c$i"
	i=$((i + 1))
done

CARVE_ALL=$CARVE
CARVE="$CARVE_ALL HEAD~2.." tui_start
wait_screen "~1"
wait_screen "-- HEAD~2 --"
tui_quit

CARVE="$CARVE_ALL HEAD~2..HEAD" tui_start
wait_screen "-- HEAD~2 --"
tui_quit

# not your branch's tip and not a range are refused, saying what works
CARVE=$CARVE_ALL
git branch -q other HEAD~1
must_fail tui_cmd main..other
grep -q "the series is your branch: <base>.. or <base>..HEAD" \
	"$tmp/must_fail.out" || fail "message: $(cat "$tmp/must_fail.out")"
must_fail tui_cmd HEAD~1...HEAD
must_fail tui_cmd ..HEAD
exit 0
