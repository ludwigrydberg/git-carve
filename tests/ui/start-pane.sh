#!/bin/sh
# SPDX-License-Identifier: MIT
# carve.startPane picks the pane git carve starts in: the diff of the
# cursor's section, or the Series pane. A hidden pane starts in Files, and
# an unknown value says so and starts in Files too.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

echo one >a.c
commit_all base
echo two >a.c

git config carve.startPane diff
tui_start
wait_active "Unstaged: a.c"
tui_quit

git config carve.startPane series
tui_start
wait_active "Series"
tui_quit

git config carve.seriesPane false
tui_start
wait_active "Files"
tui_quit

git config carve.startPane nowhere
tui_start
wait_active "Files"
wait_screen "carve.startPane = nowhere is not files"
tui_quit
