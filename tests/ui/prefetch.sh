#!/bin/sh
# SPDX-License-Identifier: MIT
# At startup git status, the HEAD line, the configuration and the upstream
# run side by side in the background, and their answers are used: none of
# them runs again in the foreground, not even the upstream lookup that
# fails in a repository without one.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

echo one >a.c
commit_all "first commit"
echo two >a.c

: >"$tmp/trace"
TUI_ENV="$TUI_ENV GIT_CARVE_TRACE=$tmp/trace" tui_start
wait_screen "M a.c"
wait_screen "first commit"
wait_screen "+two"
for q in 'status --porcelain=v1 -z --no-renames --untracked-files=no' \
	'log -1 --no-color --decorate-refs=HEAD' 'config -z --list' \
	'rev-parse -q --verify --symbolic-full-name @{upstream}'; do
	grep '^bg ' "$tmp/trace" | grep -qF "$q" ||
		fail "not in the background: $q
$(cat "$tmp/trace")"
	grep -v '^bg ' "$tmp/trace" | grep -qF "$q" &&
		fail "asked again: $q
$(cat "$tmp/trace")"
done
tui_quit
