#!/bin/sh
# SPDX-License-Identifier: MIT
# Split HEAD into three commits, and finish only once the trees match (I6).
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'base\n' >README
commit_all "initial"
printf 'a1\na2\n' >a.c
printf 'b1\n' >b.c
printf 'base\nmore\n' >README
git add -A
git commit -q -m "drv: do three things" -m "Long explanation." ||
	fail "fat commit"
orig=$(git rev-parse HEAD)
orig_tree=$(tree_of HEAD)

# refusals before starting
printf 'dirty\n' >>README
must_fail carve split
grep -q 'commit or stash' "$tmp/must_fail.out" || fail "dirty message"
git checkout -q README
must_fail carve split --finish
must_fail carve split --abort

carve split >"$tmp/out" || fail "split"
grep -q 'splitting' "$tmp/out" || fail "start message"
[ "$(git rev-parse HEAD)" = "$(git rev-parse HEAD~0)" ] || fail "head"
[ "$(git rev-parse HEAD)" = "$(git rev-parse "$orig~1")" ] || fail "reset"
[ "$(git rev-parse refs/worktree/carve/orig)" = "$orig" ] || fail "orig ref"
must_fail carve split
grep -q 'already in progress' "$tmp/must_fail.out" || fail "double split"
carve split --status | grep -q 'SPLIT .* "drv: do three things" .* 0 commits made, remaining diff +4/-0' ||
	fail "status: $(carve split --status)"

# in the first commit the editor (EDITOR=true) keeps the original message
carve stage-file a.c
carve commit >/dev/null || fail "commit 1"
[ "$(git log -1 --format=%B)" = "$(printf 'drv: do three things\n\nLong explanation.')" ] ||
	fail "first message: $(git log -1 --format=%B)"

# finishing too early is refused and says what is left
must_fail carve split --finish
grep -q 'split not finished.*+2/-0' "$tmp/must_fail.out" ||
	fail "finish message: $(cat "$tmp/must_fail.out")"

# in the second commit, later commits start empty, and EDITOR=true aborts
# them
carve stage-file b.c
must_fail carve commit
carve commit -m "drv: add b" >/dev/null || fail "commit 2"
carve split --status | grep -q '1 commit made\|2 commits made' ||
	fail "status after 2"

# uncommitted changes block finishing even when the tree would match
carve stage README n2
must_fail carve split --finish
carve commit -m "readme: more" >/dev/null || fail "commit 3"
printf 'unrelated\n' >>b.c
must_fail carve split --finish
grep -q 'uncommitted changes in b.c' "$tmp/must_fail.out" ||
	fail "uncommitted message: $(cat "$tmp/must_fail.out")"
git checkout -q b.c

carve split --status | grep -q 'nothing left' || fail "status done"
carve split --finish >"$tmp/out" || fail "finish"
grep -q 'split finished: 3 commits' "$tmp/out" || fail "finish output"
[ "$(tree_of HEAD)" = "$orig_tree" ] || fail "I6: tree differs"
[ "$(git rev-list --count HEAD)" = 4 ] || fail "commit count"
git rev-parse -q --verify refs/worktree/carve/orig >/dev/null && fail "ref left"
[ -d .git/carve ] && fail "state left"
carve split --status | grep -q 'no split in progress' || fail "status end"

# splitting a pushed commit warns (but works)
git init -q --bare "$tmp/remote.git"
git remote add origin "$tmp/remote.git"
git push -q -u origin main 2>/dev/null || fail "push"
carve split >"$tmp/out" 2>"$tmp/err" || fail "split pushed"
grep -q "already in the upstream" "$tmp/err" || fail "no pushed warning"
carve split --abort >/dev/null || fail "abort pushed"

# merge commits cannot be split
git checkout -q -b side HEAD~1
printf 'side\n' >side.c
commit_all side
git checkout -q main
git merge -q --no-edit side >/dev/null 2>&1 || fail "merge"
must_fail carve split
grep -q 'merge commit' "$tmp/must_fail.out" || fail "merge message"
exit 0
