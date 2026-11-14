#!/bin/sh
# SPDX-License-Identifier: MIT
# git carve show <commit> lists the files a commit changes, as the Commit
# section lists them. A rename is one entry, R old new.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'a\n' >a.c
printf 'b\n' >b.c
printf 'c\n' >c.c
commit_all root
carve show HEAD >"$tmp/out" || fail "show root"
printf 'A\ta.c\nA\tb.c\nA\tc.c\n' | cmp -s - "$tmp/out" ||
	fail "root: $(cat "$tmp/out")"

printf 'a2\n' >a.c
git rm -q b.c
git mv c.c "d i r.c"
uw=$(printf 'n\303\244w.c')
printf 'new\n' >"$uw"
commit_all "change, delete, rename, add"
carve show HEAD >"$tmp/out" || fail "show"
printf 'M\ta.c\nD\tb.c\nR\tc.c\td i r.c\nA\t%s\n' "$uw" |
	cmp -s - "$tmp/out" || fail "files: $(cat "$tmp/out")"

# with a path it shows the annotated diff
carve show HEAD a.c | grep -q '^    1       -a$' || fail "diff: $(carve show HEAD a.c)"
# a rename's diff is between the old and the new content
carve show HEAD "d i r.c" >"$tmp/out" || fail "show rename"
grep -q 'rename' "$tmp/out" || fail "not a rename: $(cat "$tmp/out")"
grep -q '^rename from c.c$' "$tmp/out" || fail "from: $(cat "$tmp/out")"
must_fail carve show nosuchcommit
exit 0
