#!/bin/sh
# SPDX-License-Identifier: MIT
# Search and replace on work-tree lines (git carve substitute, :s in the
# TUI): only the lines given, line ends kept, the index untouched, and
# undo puts the file back.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'a 100\nb 100\nc 100\n' >f.c
printf 'x 1\r\ny 1\r\n' >crlf.c
commit_all base
index=$(git write-tree)

carve substitute f.c n2 's/100/500/' >"$tmp/out" || fail "substitute"
grep -q '^1 line changed$' "$tmp/out" || fail "out: $(cat "$tmp/out")"
[ "$(cat f.c)" = "$(printf 'a 100\nb 500\nc 100')" ] || fail "f.c: $(cat f.c)"
[ "$(git write-tree)" = "$index" ] || fail "index changed"
# undo puts the work tree back
carve undo >/dev/null || fail undo
[ "$(cat f.c)" = "$(printf 'a 100\nb 100\nc 100')" ] || fail "undo: $(cat f.c)"

# several ranges, g, groups
carve substitute f.c n1,3 's/([a-z]) (1)00/\2\1/g' >/dev/null || fail ranges
[ "$(cat f.c)" = "$(printf '1a\nb 100\n1c')" ] || fail "ranges: $(cat f.c)"
git checkout -q f.c

# the whole file, where CRLF lines keep their ends
carve substitute --all crlf.c 's/1$/2/' >/dev/null || fail "--all"
[ "$(git hash-object crlf.c)" = "$(printf 'x 2\r\ny 2\r\n' | git hash-object --stdin)" ] ||
	fail "crlf: $(od -c crlf.c)"

# refused for the old side, without a match, and for a bad expression
must_fail carve substitute f.c o2 's/1/2/'
grep -q 'old-side' "$tmp/must_fail.out" || fail "old side"
must_fail carve substitute f.c n1 's/zzz/y/'
grep -q 'no match' "$tmp/must_fail.out" || fail "no match"
must_fail carve substitute f.c n1 's/(/y/'
must_fail carve substitute --all f.c n1 's/a/b/'
[ "$(cat f.c)" = "$(printf 'a 100\nb 100\nc 100')" ] || fail "changed by a refusal"
