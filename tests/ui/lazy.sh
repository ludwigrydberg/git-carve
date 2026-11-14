#!/bin/sh
# SPDX-License-Identifier: MIT
# Diffs load lazily. Moving through the Files list with keys arriving
# faster than the settle time loads only the diff of the file the cursor
# ends on.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=10
while [ $i -lt 50 ]; do printf 'a\n' >"f$i.c"; i=$((i + 1)); done
commit_all base
i=10
while [ $i -lt 50 ]; do printf 'b\n' >"f$i.c"; i=$((i + 1)); done
git_shim "printf '%s\n' \"\$*\" >>\"$tmp/gitlog\""

tui_start
wait_screen "M f10.c"
wait_screen "+b"
wait_idle
: >"$tmp/gitlog"
# 30 steps in one go, like a held key
keys j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j
wait_screen "Unstaged: f40.c"
wait_screen "+b"
wait_idle
n=$(grep -c ' diff ' "$tmp/gitlog")
[ "$n" -le 4 ] || fail "$n git diff runs for one cursor move: $(cat "$tmp/gitlog")"

# an action right after moving still acts on the file under the cursor
keys j Enter
wait_screen "staged f41.c"
[ "$(git diff --cached --name-only)" = f41.c ] ||
	fail "staged $(git diff --cached --name-only)"
tui_quit
