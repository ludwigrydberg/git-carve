#!/bin/sh
# SPDX-License-Identifier: MIT
# squash puts the fixup!, amend! and squash! commits of the series into
# their commits, as rebase --autosquash does, with amend! on any git. The
# series ends in the same tree, the work tree and index stay as they were,
# and a fixup without its commit stays where it is. --dry-run only lists. A
# conflict stops, with SQUASH in the status, abort puts all back and
# continue goes on. undo takes a squash back.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'base\n' >base.c
commit_all base
git branch -q up
git branch -q --set-upstream-to=up
printf 'a\n' >a.c
commit_all "drv: add a"
printf 'x = 1\n' >b.c
commit_all "drv: add b"
printf 'c\n' >c.c
commit_all "drv: add c"

# fixup! b with content, amend! a with message and content, a fixup of the
# fixup (fixup! fixup! drv: add b), squash! c, and a fixup of a commit not
# in the series
printf 'x = 1\ny = 2\n' >b.c
git add b.c
carve fixup HEAD~1 >/dev/null || fail "fixup b"
printf 'a fixed\n' >a.c
git add a.c
carve fixup --amend -m "drv: add a, properly

The longer story." HEAD~3 >/dev/null || fail "amend a"
printf 'x = 1\ny = 3\n' >b.c
git commit -q -am "fixup! fixup! drv: add b"
printf 'c more\n' >c.c
git commit -q -am "squash! drv: add c

and more of c."
printf 'w\n' >w.c
git add w.c
git commit -q -m "fixup! not in the series"
tip=$(git rev-parse HEAD)
tip_tree=$(tree_of HEAD)

# work in progress, staged and unstaged, both kept as they are
printf 'staged\n' >base.c
git add base.c
printf 'unstaged\n' >>c.c

# --dry-run shows the plan, and nothing changes
carve squash --dry-run >"$tmp/plan" || fail "dry run"
grep -q 'fixup! drv: add b -> [0-9a-f]* drv: add b' "$tmp/plan" ||
	fail "plan: $(cat "$tmp/plan")"
grep -q 'amend! drv: add a -> [0-9a-f]* drv: add a' "$tmp/plan" ||
	fail "plan amend: $(cat "$tmp/plan")"
grep -q 'kept' "$tmp/plan" || fail "plan: the kept fixup"
[ "$(git rev-parse HEAD)" = "$tip" ] || fail "dry run changed HEAD"

carve squash >"$tmp/out" || fail "squash: $(cat "$tmp/out")"
grep -q "squashed 4 fixups into 3 commits" "$tmp/out" ||
	fail "output: $(cat "$tmp/out")"
# base, a, b, c, the kept fixup
[ "$(git rev-list --count HEAD)" = 5 ] || fail "count: $(git log --oneline)"
[ "$(git log -1 --format=%s HEAD~3)" = "drv: add a, properly" ] ||
	fail "amend! subject: $(git log -1 --format=%s HEAD~3)"
git log -1 --format=%B HEAD~3 | grep -q '^The longer story\.$' ||
	fail "amend! body"
git log -1 --format=%B HEAD~3 | grep -q 'amend!' && fail "amend! line kept"
[ "$(git show HEAD~3:a.c)" = "a fixed" ] || fail "amend! content"
[ "$(git log -1 --format=%s HEAD~2)" = "drv: add b" ] || fail "b subject"
[ "$(git show HEAD~2:b.c)" = "$(printf 'x = 1\ny = 3')" ] || fail "b content"
git log -1 --format=%B HEAD~1 | grep -q '^and more of c\.$' ||
	fail "squash! message: $(git log -1 --format=%B HEAD~1)"
[ "$(git log -1 --format=%s)" = "fixup! not in the series" ] ||
	fail "kept fixup moved"
[ "$(tree_of HEAD)" = "$tip_tree" ] || fail "tree changed"
[ "$(git diff --cached --name-only)" = base.c ] || fail "staged lost"
[ "$(git diff --name-only)" = c.c ] || fail "unstaged lost"

# undo brings the series back as it was
carve undo >/dev/null || fail "undo"
[ "$(git rev-parse HEAD)" = "$tip" ] || fail "undo: HEAD"
git reset -q --hard
git clean -qfd

# with nothing to squash it says that
git reset -q --hard HEAD~6 2>/dev/null || git reset -q --hard "$(git rev-parse up)"
carve squash 2>"$tmp/err" && fail "squash without fixups"
grep -q "no fixup!" "$tmp/err" || fail "message: $(cat "$tmp/err")"

# --- a conflict, c changes the line the fixup of b changes
git reset -q --hard "$(git rev-parse up)"
printf 'x = 1\n' >b.c
commit_all "drv: add b"
printf 'x = 2\n' >b.c
commit_all "drv: change b"
printf 'x = 3\n' >b.c
git commit -q -am "fixup! drv: add b"
tip=$(git rev-parse HEAD)
carve squash >/dev/null 2>"$tmp/err"
[ $? = 1 ] || [ -n "$(git ls-files -u)" ] || fail "no conflict"
carve status | grep -q SQUASH || fail "status: $(carve status)"
carve abort >/dev/null || fail "abort"
[ "$(git rev-parse HEAD)" = "$tip" ] || fail "abort: HEAD"
[ -z "$(git status --porcelain)" ] || fail "abort: $(git status --porcelain)"

# again, resolved each time as x = 3, for the same tree in the end
carve squash >/dev/null 2>&1
printf 'x = 3\n' >b.c
git add b.c
_n=3
until carve continue >/dev/null 2>&1; do
	_n=$((_n - 1))
	[ "$_n" -gt 0 ] || fail "continue: $(carve status)"
	printf 'x = 3\n' >b.c
	git add b.c
done
# (drv: change b has nothing left to change then, and git drops it)
git log --format=%s up..HEAD | grep -q 'fixup!' && fail "fixup left"
[ "$(git show HEAD:b.c)" = "x = 3" ] || fail "content after continue"
carve status | grep -q "nothing in progress" || fail "still in progress"
exit 0
