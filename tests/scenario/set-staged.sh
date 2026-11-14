#!/bin/sh
# SPDX-License-Identifier: MIT
# set-staged replaces a selected region of the index with new content,
# without touching the work tree.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'a\nb\nc\nd\ne\n' >f
commit_all base
printf 'a\nB\nc\nd\nE\n' >f
before=$(worktree_state)

# the region as it would be staged
[ "$(carve set-staged --print f o2,n2)" = "B" ] || fail "print pair"
[ "$(carve set-staged --print f n2)" = "B" ] ||
	fail "print + only"
[ "$(carve set-staged --print f n1-3)" = "$(printf 'a\nB\nc')" ] ||
	fail "print with context"

# stage something that exists neither in HEAD nor in the work tree
printf 'b2\n' >"$tmp/new"
carve set-staged f o2,n2 "$tmp/new" || fail "set-staged"
assert_index_text f 'a\nb2\nc\nd\ne\n'
[ "$(worktree_state)" = "$before" ] || fail "worktree modified"
carve diff f | grep -q '^    2       -b2$' || fail "unstaged diff after"

# stdin, a region with several lines, deleting lines
printf 'x\ny\n' | carve set-staged f n4-5 - || fail "stdin"
assert_index_text f 'a\nb2\nc\nx\ny\n'
: >"$tmp/empty"
carve set-staged f o4-5,n5 "$tmp/empty" || fail "delete region"
assert_index_text f 'a\nb2\nc\n'

# on the staged side, edit what is staged now
[ "$(carve set-staged --print --cached f n2)" = "b2" ] || fail "print cached"
printf 'b3\n' | carve set-staged --cached f n2 - || fail "cached"
assert_index_text f 'a\nb3\nc\n'

# content without a trailing newline gets one when lines follow
printf 'b4' | carve set-staged --cached f n2 - || fail "no eol content"
assert_index_text f 'a\nb4\nc\n'

# an untracked file becomes intent-to-add, then gets the content
printf 'n1\nn2\n' >new.c
printf 'N1\n' | carve set-staged new.c n1 - || fail "untracked"
assert_index_text new.c 'N1\n'
[ "$(worktree_state)" != "" ] || fail "state"

# file without newline at the end
printf 'p\nq' >eol
commit_all eol
printf 'p\nQ' >eol
[ "$(carve set-staged --print eol o2,n2)" = "Q" ] || fail "print noeol"
carve set-staged --print eol o2,n2 | od -c | grep -q '\\n' &&
	fail "noeol region got a newline"
printf 'R' | carve set-staged eol o2,n2 - || fail "set noeol"
assert_index_text eol 'p\nR'

# executable bit is kept
printf 'echo 1\n' >x.sh
chmod +x x.sh
commit_all x
printf 'echo 2\n' >x.sh
printf 'echo 3\n' | carve set-staged x.sh o1,n1 - || fail "exec"
git ls-files -s x.sh | grep -q '^100755 ' || fail "mode lost"

must_fail carve set-staged f n99 "$tmp/new"
must_fail carve set-staged f
