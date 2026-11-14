#!/bin/sh
# SPDX-License-Identifier: MIT
# move on HEAD into a new commit right before or after it (a carve). The
# lines go into the new commit, HEAD keeps its message and author, and the
# tip tree stays. All of the commit is refused, and nothing changes.
. "$(dirname "$0")/../lib.sh"
new_repo

i=1; while [ $i -le 40 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
edit "l 10" "l 10 P1"
commit_all P1
# P2 does two things, its own change and a helper that belongs apart
edit "l 20" "l 20 P2"
edit "l 30" "l 30 helper"
git commit -q -a -m P2 -m "The P2 change." -m "Signed-off-by: A <a@a>"
tip=$(git rev-parse HEAD^{tree})

# before, the helper line into a new commit below P2
carve move HEAD a.c n30 --new-before HEAD -m "helper first" >"$tmp/out" ||
	fail "carve before: $(cat "$tmp/out")"
grep -q 'carved 1 changed line of a.c into a new commit before it' "$tmp/out" ||
	fail "message: $(cat "$tmp/out")"
[ "$(git log -1 --format=%s HEAD~1)" = "helper first" ] || fail "new commit"
git show HEAD~1 | grep -q '^+l 30 helper$' || fail "new commit content"
git show HEAD~1 | grep -q '^+l 20 P2$' && fail "P2's change in the new one"
[ "$(git log -1 --format=%B HEAD | head -n 1)" = P2 ] || fail "P2 message"
git log -1 --format=%B HEAD | grep -q '^Signed-off-by: A <a@a>$' || fail "trailer"
git show HEAD | grep -q "^+l 30 helper" && fail "helper still in P2: $(git show HEAD)"
[ -z "$(git status --porcelain --untracked-files=no)" ] || fail "dirty"

# after, P2's change into a new commit on top of it
carve move HEAD a.c n20 --new-after HEAD -m "P2 part two" >/dev/null ||
	fail "carve after"
[ "$(git log -1 --format=%s HEAD)" = "P2 part two" ] || fail "after: subject"
git show HEAD | grep -q '^+l 20 P2$' || fail "after: content"
# all of a commit is no carve
must_fail carve move HEAD a.c n20 --new-before HEAD -m x
grep -q 'that is all the commit changes' "$tmp/must_fail.out" ||
	fail "all: $(cat "$tmp/must_fail.out")"
[ "$(git log -1 --format=%s HEAD)" = "P2 part two" ] || fail "not restored"
[ "$(git rev-parse HEAD^{tree})" = "$tip" ] || fail "the tip changed"
[ "$(git log --format=%s -5 | tr '\n' ' ')" = \
  "P2 part two P2 helper first P1 base " ] ||
	fail "series: $(git log --oneline -5)"
exit 0
