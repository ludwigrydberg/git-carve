#!/bin/sh
# SPDX-License-Identifier: MIT
# J on a commit whose files sit in a directory, with changes of your own in
# the work tree. The commit's first file is selected and its diff shown,
# not the directory row above it.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

mkdir -p drv
printf 'one\ntwo\n' >drv/a.c
commit_all base
git branch -q up
git branch -q --set-upstream-to=up
printf 'one\ntwo P1\n' >drv/a.c
commit_all P1
printf 'one mine\ntwo P1\n' >drv/a.c

tui_start
wait_screen 'Unstaged (1)'
# after the untracked scan, whose reload would pick a file again
wait_gone 'scanning untracked'
wait_idle
keys J
wait_screen 'Commit HEAD (1)'
wait_screen 'Commit HEAD: drv/a.c'
wait_screen '+two P1'
wait_match '^>M  *a\.c' || fail "a.c not selected: $(screen)"
# K shows the work tree again with its file selected
keys K
wait_screen 'Unstaged: drv/a.c'
wait_screen '+one mine'
tui_quit
exit 0
