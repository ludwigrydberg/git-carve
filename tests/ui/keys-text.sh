#!/bin/sh
# SPDX-License-Identifier: MIT
# Texts that name keys, in the status line, messages, notes and the help,
# name them as bound. Rebound keys show, and an unbound action shows as
# <name>.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1; while [ $i -le 30 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
for n in 1 2; do
	sed "s/^l $((n * 10))\$/l $((n * 10)) P$n/" a.c >a.tmp && mv a.tmp a.c
	commit_all "P$n"
done
git config carve.keys.next-commit F5
git config carve.keys.prev-commit F6
git config carve.keys.split F7
git config carve.keys.fixup-to none
git config carve.keys.undo U

# the help of git carve keys, with the undo key a description names
tui_cmd keys | grep -q '^discard .*after asking; U undoes it' ||
	fail "keys: $(tui_cmd keys | grep '^discard')"

# with nothing to stage, the split key in the hint
tui_start
wait_screen 'Nothing to stage'
screen | grep -q -F '`F7` to split HEAD' ||
	fail "hint: $(screen | tail -n 2)"
tui_quit

# with a commit shown, the status line names the keys as bound
git branch -q up HEAD~2
git branch -q --set-upstream-to=up
tui_start
wait_screen 'Commit HEAD'
# refresh lost its key (F5): a warning at startup
wait_screen 'action refresh has no key any more'
close_box
wait_screen 'HEAD  F5/F6: commits  F7: split'
keys F5
wait_screen 'Commit ~1'
wait_screen '~1  F5/F6: commits  F7: split'
tui_quit
exit 0
