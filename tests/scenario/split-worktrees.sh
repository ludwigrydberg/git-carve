#!/bin/sh
# SPDX-License-Identifier: MIT
# A split belongs to its worktree. Its ref is per worktree
# (refs/worktree/carve/orig), and another worktree of the repository sees
# nothing in progress and can split on its own. A split begun by an older
# git carve with the shared refs/carve/orig goes on in the worktree that
# has its state, under the new name.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'a\n' >a.c
commit_all base
printf 'b\n' >a.c
commit_all "drv: change a"
git worktree add -q --detach "$tmp/other" HEAD || fail "worktree add"

carve split >/dev/null || fail "split"
carve split --status | grep -q 'drv: change a' || fail "status here"
cd "$tmp/other" || fail cd
carve status | grep -q 'nothing in progress' ||
	fail "the other worktree: $(carve status)"
carve split >/dev/null || fail "split in the other worktree"
carve split --abort >/dev/null || fail "abort there"
cd "$tmp/repo" || fail cd
carve split --status | grep -q 'drv: change a' || fail "still in progress here"

# the old shared name is moved to the new one by this worktree
orig=$(git rev-parse refs/worktree/carve/orig)
git update-ref refs/carve/orig "$orig"
git update-ref -d refs/worktree/carve/orig
carve split --status | grep -q 'drv: change a' || fail "old ref: status"
[ "$(git rev-parse refs/worktree/carve/orig)" = "$orig" ] || fail "not moved"
git rev-parse -q --verify refs/carve/orig >/dev/null && fail "old ref left"
carve split --abort >/dev/null || fail "abort"
[ "$(git log -1 --format=%s)" = "drv: change a" ] || fail "abort: HEAD"
git worktree remove --force "$tmp/other"
exit 0
