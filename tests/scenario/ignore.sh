#!/bin/sh
# SPDX-License-Identifier: MIT
# ignore puts an untracked file into .git/info/exclude, matched exactly,
# with glob characters and a leading # or ! taken literally. A tracked file
# is refused, since the exclude file cannot hide it.
. "$(dirname "$0")/../lib.sh"
new_repo

echo a >f.c
commit_all base
touch tags 'a*b.txt' '#x' axb.txt
carve ignore tags 'a*b.txt' '#x' || fail "ignore"
[ "$(git status --porcelain --untracked-files=all)" = '?? axb.txt' ] ||
	fail "status: $(git status --porcelain --untracked-files=all)"
must_fail carve ignore f.c
grep -q 'f.c is tracked' "$tmp/must_fail.out" || fail "tracked: $(cat "$tmp/must_fail.out")"
# in a sub-directory, the path from the top
mkdir sub
touch sub/tags
(cd sub && carve ignore tags) || fail "ignore in sub"
grep -q '^/sub/tags$' "$(git rev-parse --git-path info/exclude)" ||
	fail "sub: $(cat "$(git rev-parse --git-path info/exclude)")"
exit 0
