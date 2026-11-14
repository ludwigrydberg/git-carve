#!/bin/sh
# SPDX-License-Identifier: MIT
# Conflicts. Take ours or theirs for one conflict region or a whole file,
# and the file is staged once no conflict is left. Undo puts the conflict
# back, the file with its markers and the unmerged index.
. "$(dirname "$0")/../lib.sh"
new_repo

printf '1\na\n2\n3\n4\n5\n6\nb\n7\n' >f.c
printf 'g\n' >g.c
commit_all base
git checkout -q -b other
printf '1\na-theirs\n2\n3\n4\n5\n6\nb-theirs\n7\n' >f.c
printf 'g-theirs\n' >g.c
commit_all theirs
git checkout -q -
printf '1\na-ours\n2\n3\n4\n5\n6\nb-ours\n7\n' >f.c
printf 'g-ours\n' >g.c
commit_all ours
git merge -q other >/dev/null 2>&1 && fail "no conflict"
unmerged() {
	git diff --name-only --diff-filter=U
}

must_fail carve resolve --ours f.c 3
grep -q 'f.c has 2 conflicts, not 3' "$tmp/must_fail.out" ||
	fail "range: $(cat "$tmp/must_fail.out")"
must_fail carve resolve f.c
must_fail carve resolve --ours --theirs f.c

carve resolve --theirs f.c 2 || fail "resolve 2"
grep -q '^b-theirs$' f.c || fail "theirs not taken"
grep -q 'b-ours' f.c && fail "ours left"
[ "$(grep -c '^<<<<<<<' f.c)" = 1 ] || fail "conflict 1 touched"
unmerged | grep -q '^f.c$' || fail "staged too early"

carve resolve --ours f.c 1 || fail "resolve 1"
[ "$(cat f.c)" = "$(printf '1\na-ours\n2\n3\n4\n5\n6\nb-theirs\n7')" ] ||
	fail "content: $(cat f.c)"
unmerged | grep -q '^f.c$' && fail "not staged once resolved"
[ "$(git rev-parse :f.c)" = "$(git hash-object f.c)" ] || fail "index"

# a whole file
carve resolve --theirs g.c || fail "whole file"
[ "$(cat g.c)" = g-theirs ] || fail "g.c: $(cat g.c)"
[ -z "$(unmerged)" ] || fail "left: $(unmerged)"

# undo makes g.c conflicted again, then brings f.c's first conflict back
g_resolved=$(git hash-object g.c)
carve undo >/dev/null || fail "undo whole file"
grep -q '^<<<<<<<' g.c || fail "g.c markers not back: $(cat g.c)"
unmerged | grep -q '^g.c$' || fail "g.c not unmerged again"
[ "$(git ls-files -s g.c | wc -l | tr -d ' ')" = 3 ] ||
	fail "g.c stages: $(git ls-files -s g.c)"
carve undo >/dev/null || fail "undo conflict 1"
[ "$(grep -c '^<<<<<<<' f.c)" = 1 ] || fail "f.c conflict 1 not back"
grep -q '^b-theirs$' f.c || fail "f.c lost conflict 2's resolution"
unmerged | grep -q '^f.c$' || fail "f.c not unmerged again"
# a change since stops the undo
carve resolve --ours f.c 1 || fail "resolve 1 again"
echo extra >>f.c
must_fail carve undo
grep -q 'f.c has changed since' "$tmp/must_fail.out" ||
	fail "changed: $(cat "$tmp/must_fail.out")"
git checkout -q --ours -- f.c 2>/dev/null || :
printf '1\na-ours\n2\n3\n4\n5\n6\nb-theirs\n7\n' >f.c
git add f.c
carve resolve --theirs g.c || fail "g.c again"
[ "$(git hash-object g.c)" = "$g_resolved" ] || fail "g.c again: $(cat g.c)"
git commit -q -m merge || fail "merge commit"
exit 0
