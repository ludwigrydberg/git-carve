#!/bin/sh
# SPDX-License-Identifier: MIT
# Splitting a commit of more than 1,000 files. The status line counts the
# files left, not lines, since diff-tree --numstat reads every blob,
# seconds on a kernel release, and the count goes down as pieces are
# committed.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=0
while [ $i -lt 1100 ]; do printf 'a\n' >"f$i.c"; i=$((i + 1)); done
commit_all base
i=0
while [ $i -lt 1100 ]; do printf 'b\n' >"f$i.c"; i=$((i + 1)); done
commit_all "big: change everything"
cat >"$tmp/editor" <<'X'
#!/bin/sh
printf 'big: part\n' >"$1"
X
chmod +x "$tmp/editor"
TUI_ENV="GIT_EDITOR=$tmp/editor"

tui_start
wait_screen "Commit HEAD (1100)" 300
# the position among the files in the title
wait_screen "Files: 1/1100"
keys s y
wait_screen "1100 files left" 300
wait_screen "Unstaged (1100)" 300
keys Enter
wait_screen "Staged (1)"
keys c
wait_screen "1099 files left" 300
keys X
wait_screen "Abort the split"
keys y
wait_screen "split aborted" 300
tui_quit
