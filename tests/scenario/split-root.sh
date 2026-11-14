#!/bin/sh
# SPDX-License-Identifier: MIT
# Splitting the root commit, where the base is the empty tree.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'one\ntwo\n' >a.c
printf 'b\n' >b.c
commit_all "root: everything"
orig=$(git rev-parse HEAD)
orig_tree=$(tree_of HEAD)

carve split >/dev/null || fail "split root"
git rev-parse -q --verify HEAD >/dev/null && fail "HEAD should be unborn"
[ "$(git symbolic-ref HEAD)" = refs/heads/main ] || fail "branch lost"
[ -z "$(git ls-files)" ] || fail "index not empty"
[ "$(short_status | wc -l | tr -d ' ')" = 2 ] || fail "files not untracked"
carve split --status | grep -q 'remaining diff +3/-0' ||
	fail "status: $(carve split --status)"

# stage one line of a new file into the new root commit
carve stage a.c n1 || fail "stage in root split"
carve commit >/dev/null || fail "first commit"
[ "$(git rev-list --count HEAD)" = 1 ] || fail "new root"
[ "$(git log -1 --format=%s)" = "root: everything" ] || fail "message"
carve stage-file a.c b.c
carve commit -m "rest" >/dev/null || fail "second commit"
carve split --finish >/dev/null || fail "finish"
[ "$(tree_of HEAD)" = "$orig_tree" ] || fail "I6 tree"
[ "$(git rev-list --count HEAD)" = 2 ] || fail "count"

# abort a root split before anything was committed
git reset -q --hard "$orig"
carve split >/dev/null || fail "split root again"
carve split --abort >/dev/null || fail "abort root"
[ "$(git rev-parse HEAD)" = "$orig" ] || fail "abort HEAD"
[ "$(git write-tree)" = "$orig_tree" ] || fail "abort index"
[ -z "$(short_status)" ] || fail "abort worktree: $(short_status)"

# detached root cannot be split
git checkout -q --detach
must_fail carve split
grep -q 'detached' "$tmp/must_fail.out" || fail "detached message"
exit 0
