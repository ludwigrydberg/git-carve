#!/bin/sh
# SPDX-License-Identifier: MIT
# git older than 2.26 is refused with a clear message. A git binary that
# passed is remembered in ~/.cache/git-carve/git-version by path, mtime and
# size, and the next run spares git version. Another binary is checked.
. "$(dirname "$0")/../lib.sh"
new_repo
echo a >f.c
commit_all base

git_shim 'case "$1" in version) echo "git version 2.20.1"; exit 0 ;; esac'
must_fail carve status
carve status 2>&1 | grep -q 'git 2.26 or newer is required (found 2.20.1)' ||
	fail "message: $(carve status 2>&1)"

rm "$tmp/bin/git"
carve status >/dev/null || fail "real git refused"
[ -s "$HOME/.cache/git-carve/git-version" ] || fail "not remembered"
[ -d .git/carve ] && fail "state in the repository"
# remembered, with no git version on the next run
git_shim "case \"\$1\" in version) echo ran >>\"$tmp/version\" ;; esac"
# the shim is another binary, checked once and then remembered
carve status >/dev/null || fail "shim refused"
carve status >/dev/null || fail "second run"
[ "$(wc -l <"$tmp/version" | tr -d ' ')" = 1 ] ||
	fail "git version runs: $(cat "$tmp/version" 2>/dev/null)"
exit 0
