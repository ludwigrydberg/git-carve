#!/bin/sh
# SPDX-License-Identifier: MIT
# The status line of an operation in progress puts the keys first and
# shortens the commit subject with an ellipsis to fit, at 80 and 100
# columns. The keys are never cut.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\n' >a.c
commit_all base
printf 'b\n' >b.c
commit_all "drivers: fdrv: a subject long enough not to fit on the status line of a narrow terminal"
id=$(git rev-parse --short=7 HEAD)

dash=$(printf '\342\200\224')
ell=$(printf '\342\200\246')
# quit while an operation is in progress, after confirming
quit_op() {
	keys q
	wait_screen 'quit anyway'
	keys y
	_n=$WAIT_TRIES
	while tmux -L "$TMUX_SOCK" has-session -t t 2>/dev/null; do
		_n=$((_n - 1))
		[ "$_n" -gt 0 ] || fail "did not quit"
		sleep 0.1
	done
}
# the status line is the row above the message line
status_row() {
	screen | tail -2 | head -1
}

for w in 80 100; do
	carve split >/dev/null || fail "split"
	TUI_LOCALE=C.UTF-8 TUI_W=$w tui_start
	wait_screen 'SPLIT  ZZ: finish  X: abort'
	row=$(status_row)
	echo "$row" | grep -q "^ SPLIT  ZZ: finish  X: abort $dash 0 commits made" ||
		fail "$w: keys not first: $row"
	echo "$row" | grep -q "$id" || fail "$w: no commit id: $row"
	echo "$row" | grep -q "narrow terminal" && fail "$w: subject not shortened"
	# at 100 columns a shortened subject, and at 80 no room for one,
	# the id only
	if [ $w = 100 ]; then
		echo "$row" | grep -q "$id \"drivers: fdrv: .*$ell\"" ||
			fail "$w: no ellipsis: $row"
	else
		echo "$row" | grep -q "${id}[[:space:]]*$" || fail "$w: $row"
	fi
	quit_op
	carve split --abort >/dev/null || fail "abort"
done

# for a commit shown the keys are a hint, left out before the subject is
# cut
TUI_LOCALE=C.UTF-8 TUI_W=100 tui_start
wait_screen "HEAD $dash $id"
status_row | grep -q "^ HEAD $dash $id \"drivers: fdrv: .*$ell\"" ||
	fail "commit: $(status_row)"
tui_quit
exit 0
