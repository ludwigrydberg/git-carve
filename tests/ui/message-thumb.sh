#!/bin/sh
# SPDX-License-Identifier: MIT
# The Message pane grows with the message up to a third of the rows, at
# most 12 lines. With more than it shows, a thumb on the right line of its
# frame says how much there is and where the view is. With all of it
# shown there is none.
CARVE_TEST_LAYOUT=default
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

echo one >a.c
commit_all base
echo two >a.c

# the right line of the frame on the row with text, x in the C locale,
# where the thumb is a blank
frame_at() {
	screen | grep -F -- "$1" | grep -q 'x$'
}

tui_start
wait_screen "No message yet."
frame_at "No message yet." || fail "a thumb with no message: $(screen)"
tui_quit

draft=$(git rev-parse --git-path CARVE_MESSAGE)
{
	echo "a: the subject"
	echo
	i=1
	while [ $i -le 30 ]; do echo "body line $i"; i=$((i + 1)); done
} >"$draft"
tui_start
wait_screen "a: the subject"
# 12 lines at 40 rows
wait_screen "body line 10"
screen | grep -q "body line 11" && fail "more than 12 lines: $(screen)"
frame_at "a: the subject" && fail "no thumb at the top: $(screen)"
frame_at "body line 10" || fail "the thumb is too long: $(screen)"

# at the end the thumb is at the bottom
step_keys Tab
wait_active "Message"
step_keys G
wait_screen "body line 30"
frame_at "body line 30" && fail "no thumb at the bottom: $(screen)"
frame_at "body line 19" || fail "the thumb is too long: $(screen)"
tui_quit
