#!/bin/sh
# SPDX-License-Identifier: MIT
# Stage and unstage keep their undo entries back while keys come fast
# and write them together when the keys pause (for a second): every key
# is still one entry, u undoes exactly the last one, and nothing is lost on quit.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

awk 'BEGIN { for (i = 1; i <= 60; i++) print "line " i }' >f.c
commit_all base
awk 'BEGIN { for (i = 1; i <= 60; i++)
	print (i % 10 == 5 ? "LINE " : "line ") i }' >f.c
# wait_lines <n>: until n changed lines are unstaged
wait_lines() {
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ]; do
		[ "$(git diff | grep -c '^+LINE')" = "$1" ] && return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	fail "$1 unstaged lines expected: $(git diff)"
}

git_shim "case \" \$* \" in *' commit-tree '*) echo x >>\"$tmp/ct\" ;; esac"

tui_start
wait_screen "M f.c"
keys Tab
wait_active "Unstaged: f.c"
# six hunks, one key each, as fast as tmux sends them
keys a Space a Space a Space a Space a Space a Space
wait_screen "(no unstaged changes)"
# written once the keys pause for a second
n=$WAIT_TRIES
while [ "$n" -gt 0 ] &&
	[ "$(carve undo --list | grep -c ' stage f.c')" != 6 ]; do
	sleep 0.1
	n=$((n - 1))
done
[ "$(carve undo --list | grep -c ' stage f.c')" = 6 ] ||
	fail "entries: $(carve undo --list)"
# at most the first went through git commit-tree, the rest in one go
[ "$(wc -l <"$tmp/ct" 2>/dev/null | tr -d ' ')" -le 1 ] ||
	fail "commit-tree per key: $(cat "$tmp/ct")"
# u takes back the last hunk only
keys u
wait_screen "undid stage f.c"
wait_lines 1
wait_idle
[ "$(git diff | grep -c '^+LINE')" = 1 ] || fail "u undid more: $(git diff)"
keys u
wait_lines 2

# kept back at quit, written on the way out
keys Tab
wait_active "Staged: f.c"
keys a Space a Space
wait_screen "unstaged 2 lines"
keys q
n=$WAIT_TRIES
while [ "$n" -gt 0 ] && tmux -L "$TMUX_SOCK" has-session -t t 2>/dev/null; do
	sleep 0.1
	n=$((n - 1))
done
check_sanitizers
[ "$(carve undo --list | grep -c ' unstage f.c')" -ge 2 ] ||
	fail "quit lost entries: $(carve undo --list)"
carve undo || fail "undo after quit"
[ "$(git diff | grep -c '^+LINE')" = 3 ] || fail "undo after quit: $(git diff)"
exit 0
