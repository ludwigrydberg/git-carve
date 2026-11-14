#!/bin/sh
# SPDX-License-Identifier: MIT
# The diff cache, one git diff of the whole tree once the keys pause, runs
# for an ordinary change set but not for one of more than 3,000 files.
# There it cost seconds and hundreds of MB of git only to go over its cap.
# Diffs then load per file.
. "$(dirname "$0")/../lib-tmux.sh"

# tree <files>: that many changed files
tree() {
	i=0
	while [ $i -lt "$1" ]; do printf 'a\n' >"f$i.c"; i=$((i + 1)); done
	commit_all base
	i=0
	while [ $i -lt "$1" ]; do printf 'b\n' >"f$i.c"; i=$((i + 1)); done
}

# cache_runs counts the whole-tree diffs in the background, once the TUI
# has nothing left to do (it must get there, also without the cache)
cache_runs() {
	: >"$tmp/trace"
	TUI_ENV="GIT_CARVE_TRACE=$tmp/trace" tui_start
	wait_screen "Unstaged ($1)" 300
	wait_idle
	runs=$(grep -c '^bg .* diff ' "$tmp/trace")
	tui_quit
}

new_repo small
tree 50
cache_runs 50
[ "$runs" -ge 1 ] || fail "no cache for 50 files: $(cat "$tmp/trace")"

new_repo big
tree 3100
cache_runs 3100
[ "$runs" = 0 ] || fail "$runs cache runs for 3,100 files: $(grep '^bg' "$tmp/trace")"
exit 0
