#!/bin/sh
# SPDX-License-Identifier: MIT
# The Series pane cuts subjects to its width. With the cursor in it the
# pane widens to show them whole, and the status line always carries the
# whole subject of the commit shown. At 100 and 120 columns. At 80 the
# panes stack and the Series pane is as wide as the screen.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

echo a >a.c
commit_all "fdrv: retry the probe on -EAGAIN"
echo b >>a.c
commit_all "fdrv: fix a typo in the probe message"
echo c >>a.c
commit_all "fdrv: sleep instead of busy-waiting"

TUI_W=80 TUI_H=24 tui_start
wait_screen 'Commit HEAD'
screen | grep -q 'HEAD .*fdrv: sleep instead of busy-waiting' ||
	fail "80: cut although stacked: $(screen | head -n 5)"
tui_quit

for TUI_W in 100 120; do
	export TUI_W
	TUI_H=24; export TUI_H
	tui_start
	wait_screen 'Commit HEAD'
	# without the cursor, cut in the pane and whole on the status line
	inner | grep -q '^\* HEAD .*busy-waiting' &&
		fail "$TUI_W: not cut: $(screen | head -n 5)"
	wait_screen "\"fdrv: sleep instead of busy-waiting\""
	keys J
	wait_screen "\"fdrv: fix a typo in the probe message\""
	screen | tail -n 2 | grep -q "^ ~1 " || fail "$TUI_W: status: $(screen | tail -n 2)"
	# the pane with the cursor shows every subject whole
	keys BTab
	wait_active "Series"
	inner | grep -q "^> ~1 .*fdrv: fix a typo in the probe message" ||
		fail "$TUI_W: ~1 cut: $(screen | head -n 6)"
	screen | grep -q '  HEAD .*fdrv: sleep instead of busy-waiting' ||
		fail "$TUI_W: HEAD cut: $(screen | head -n 6)"
	screen | grep -q '  ~2 .*fdrv: retry the probe on -EAGAIN' ||
		fail "$TUI_W: ~2 cut: $(screen | head -n 6)"
	# and narrow again when it loses the cursor
	keys Tab
	wait_active 'Files'
	screen | grep -q '  HEAD .*busy-waiting' &&
		fail "$TUI_W: still wide: $(screen | head -n 6)"
	tui_quit
done
