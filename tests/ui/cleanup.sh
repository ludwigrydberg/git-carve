#!/bin/sh
# SPDX-License-Identifier: MIT
# What an interrupted operation left behind. The TUI offers to clean it up
# at startup, n keeps it and y removes it. It names a kept snapshot of the
# work tree, which is never removed.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\n' >a.c
commit_all base
# state files of an operation that is no longer in progress
mkdir -p .git/carve
printf 'x\n' >.git/carve/target

tui_start
wait_screen "An interrupted operation left files behind. Clean them up? [y/N]"
keys n
wait_gone "Clean them up?"
[ -f .git/carve/target ] || fail "n removed the state"
tui_quit

tui_start
wait_screen "Clean them up? [y/N]"
keys y
wait_screen "cleaned up"
[ -e .git/carve/target ] && fail "y kept the state"
tui_quit

# with nothing left there is no question
tui_start
wait_screen "Nothing to stage"
screen | grep -q "Clean them up" && fail "asked again"
tui_quit

# a snapshot of the work tree is named, not removed
git update-ref refs/worktree/carve/worktree HEAD
tui_start
wait_screen "kept a snapshot of your work tree"
screen | grep -q "Clean them up" && fail "asked for the snapshot"
tui_quit
git rev-parse -q --verify refs/worktree/carve/worktree >/dev/null || fail "snapshot removed"
exit 0
