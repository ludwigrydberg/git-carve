#!/bin/sh
# SPDX-License-Identifier: MIT
# Where a rebase of yours stopped, lines move into a commit below HEAD, or
# a new commit between them, at once. The move runs in a worktree of its
# own, HEAD then points at the result with the same tree, and your rebase
# goes on. Nothing waits for later, and nothing can be applied to the wrong
# commit. git rebase --abort drops the move with the rest of the rebase,
# and a move that cannot be made changes nothing.
. "$(dirname "$0")/../lib.sh"
new_repo

i=1; while [ $i -le 40 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
edit "l 10" "l 10 P1"
commit_all P1
edit "l 20" "l 20 P2"
commit_all P2
edit "l 35" "l 35 P3"
edit "l 37" "l 37 P3"
commit_all P3
edit "l 40" "l 40 P4"
commit_all P4
orig=$(git rev-parse HEAD)
tip=$(git rev-parse HEAD^{tree})

# Newer git (as on macOS) writes the todo as "pick <id> # <subject>". This
# test uses that form on any git, rebase-stop.sh the plain one, and the
# sequence editor takes both.
git config rebase.instructionFormat "# %s"
cat >"$tmp/seq" <<'EOF'
#!/bin/sh
awk '$1 == "pick" && ($3 == "P3" || ($3 == "#" && $4 == "P3")) {
	$1 = "edit" } { print }' "$1" \
	>"$1.tmp" && mv "$1.tmp" "$1"
EOF
chmod +x "$tmp/seq"
stop() {
	GIT_SEQUENCE_EDITOR="$tmp/seq" git rebase -q -i HEAD~4 >/dev/null 2>&1
	[ "$(git log -1 --format=%s)" = P3 ] || fail "not stopped at P3"
}
worktrees() { git worktree list | wc -l | tr -d ' '; }

# into P1, two commits below HEAD, done now while the rebase goes on
stop
at=$(git rev-parse HEAD^{tree})
carve move HEAD a.c o35,n35 --into HEAD~2 >"$tmp/out" ||
	fail "move: $(cat "$tmp/out")"
[ -d "$(git rev-parse --git-path rebase-merge)" ] || fail "rebase gone"
[ "$(git rev-parse HEAD^{tree})" = "$at" ] || fail "tree at the stop changed"
[ "$(worktrees)" = 1 ] || fail "worktree left: $(git worktree list)"
git show HEAD~2:a.c | grep -q '^l 35 P3$' || fail "not in P1"
[ "$(git log -1 --format=%s HEAD~2)" = P1 ] || fail "P1 subject"
git show HEAD | grep -q '^+l 35 P3$' && fail "still in P3"
git show HEAD | grep -q '^+l 37 P3$' || fail "P3 lost its other line"
git rebase --continue >/dev/null 2>&1 || fail continue
[ "$(git rev-parse HEAD^{tree})" = "$tip" ] || fail "tip tree"
[ "$(git log --format=%s | tr '\n' ' ')" = "P4 P3 P2 P1 base " ] ||
	fail "history: $(git log --format=%s)"
git show HEAD~3:a.c | grep -q '^l 35 P3$' || fail "P1 after the rebase"

# a new commit between P1 and P2, from HEAD's lines
git reset -q --hard "$orig"
stop
carve move HEAD a.c o37,n37 --new-before HEAD~1 -m "P1b" >/dev/null ||
	fail "new before"
git rebase --continue >/dev/null 2>&1 || fail "continue 2"
[ "$(git log --format=%s | tr '\n' ' ')" = "P4 P3 P2 P1b P1 base " ] ||
	fail "history 2: $(git log --format=%s)"
[ "$(git rev-parse HEAD^{tree})" = "$tip" ] || fail "tip tree 2"

# git rebase --abort after a move leaves the branch exactly as before the
# rebase, the move with it
git reset -q --hard "$orig"
stop
carve move HEAD a.c o35,n35 --into HEAD~2 >/dev/null || fail "move 3"
git rebase --abort
[ "$(git rev-parse HEAD)" = "$orig" ] || fail "abort: not the original"

# staged changes at the stop after a move, which git rebase --continue
# amends into the commit it stopped at, as without the move
git reset -q --hard "$orig"
stop
carve move HEAD a.c o35,n35 --into HEAD~2 >/dev/null || fail "move 4"
edit "l 38" "l 38 fix"
git add a.c
git rebase --continue >/dev/null 2>"$tmp/err" ||
	fail "continue with staged changes: $(cat "$tmp/err")"
[ "$(git log --format=%s | tr '\n' ' ')" = "P4 P3 P2 P1 base " ] ||
	fail "history 4: $(git log --format=%s)"
git show HEAD~1 | grep -q '^+l 38 fix$' || fail "not amended into P3"
git show HEAD~3:a.c | grep -q '^l 35 P3$' || fail "P1 after continue"

# a move that cannot be made, since P2 changes l 34 next to l 35 and the
# lines' context is not in P1. It says so and nothing changes.
git checkout -q -b other "$orig~3"
edit "l 20" "l 20 P2"
edit "l 34" "l 34 P2"
commit_all P2
edit "l 35" "l 35 P3"
edit "l 37" "l 37 P3"
commit_all P3
edit "l 40" "l 40 P4"
commit_all P4
other=$(git rev-parse HEAD)
stop
head=$(git rev-parse HEAD)
must_fail carve move HEAD a.c o35,n35 --into HEAD~2
grep -q 'do not apply to the commit there' "$tmp/must_fail.out" ||
	fail "conflict: $(cat "$tmp/must_fail.out")"
[ "$(git rev-parse HEAD)" = "$head" ] || fail "conflict moved HEAD"
[ "$(worktrees)" = 1 ] || fail "worktree left after a conflict"
[ -d "$(git rev-parse --git-path rebase-merge)" ] || fail "rebase gone 2"
git rebase --continue >/dev/null 2>&1 || fail "continue after conflict"
[ "$(git rev-parse HEAD)" = "$other" ] || fail "series changed"
exit 0
