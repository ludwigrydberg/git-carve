#!/bin/sh
# SPDX-License-Identifier: MIT
# git carve remove runs git rm, local changes included. undo brings the
# file and the index back. Untracked files and splits are refused.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'a\n' >a.c
printf 'b\n' >b.c
commit_all base
printf 'a changed\n' >a.c
index=$(git write-tree)

carve remove a.c || fail remove
[ -e a.c ] && fail "a.c still there"
[ "$(git diff --cached --name-status)" = "$(printf 'D\ta.c')" ] ||
	fail "not staged: $(git diff --cached --name-status)"
carve undo >/dev/null || fail undo
[ "$(cat a.c)" = "a changed" ] || fail "undo: $(cat a.c 2>&1)"
[ "$(git write-tree)" = "$index" ] || fail "index not back"

printf 'u\n' >new.c
must_fail carve remove new.c
grep -q 'new.c is not tracked' "$tmp/must_fail.out" || fail "untracked"
rm new.c

git checkout -q a.c
carve split >/dev/null || fail split
must_fail carve remove b.c
grep -q 'not during a split' "$tmp/must_fail.out" || fail "split"
[ -e b.c ] || fail "b.c removed during a split"
carve split --abort >/dev/null || fail abort
