#!/bin/sh
# SPDX-License-Identifier: MIT
# Basic line staging and unstaging by line number, whole-file operations,
# and no-newline-at-end-of-file edge cases, checked against exact blobs.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n' >f.c
commit_all base
printf 'one\nTWO\nthree\nfour\nfive\nsix\nSEVEN\neight\nnine\n' >f.c
before=$(worktree_state)

# stage only the addition of line 2 (the removal stays unstaged)
carve stage f.c n2 || fail "stage n2"
assert_index_text f.c 'one\ntwo\nTWO\nthree\nfour\nfive\nsix\nseven\neight\n'

# stage the matching removal by its old-side number
carve stage f.c o2 || fail "stage o2"
assert_index_text f.c 'one\nTWO\nthree\nfour\nfive\nsix\nseven\neight\n'

# range across a change pair plus the trailing addition
carve stage f.c n7-9 || fail "stage n7-9"
assert_index f.c f.c
[ "$(worktree_state)" = "$before" ] || fail "worktree modified"

# unstage the SEVEN change using staged-diff numbers
carve unstage f.c o7,n7 || fail "unstage"
assert_index_text f.c 'one\nTWO\nthree\nfour\nfive\nsix\nseven\neight\nnine\n'
carve diff f.c | grep -q '^          7 +SEVEN$' || fail "unstaged diff"

# nothing to do / bad selections
must_fail carve stage f.c n6
grep -q 'context' "$tmp/must_fail.out" || fail "context message"
must_fail carve stage f.c n100
must_fail carve stage f.c bogus
must_fail carve stage f.c

# whole file
carve stage-file f.c || fail "stage-file"
assert_index f.c f.c
carve unstage-file f.c || fail "unstage-file"
[ "$(index_blob f.c)" = "$(git rev-parse HEAD:f.c)" ] || fail "unstage-file"
[ "$(worktree_state)" = "$before" ] || fail "worktree modified"

# no newline at end of file on the old side, and lines added after it
printf 'a\nb' >eol.c
commit_all eol
printf 'a\nb\nc\nd' >eol.c
carve stage eol.c n4 || fail "stage n4 (eol)"
assert_index_text eol.c 'a\nb\nd'
carve stage eol.c n3 || fail "stage n3 (eol)"
assert_index_text eol.c 'a\nb\nc\nd'
carve unstage eol.c n3 || fail "unstage n3 (eol)"
assert_index_text eol.c 'a\nb\nd'
carve unstage eol.c n3 || fail "unstage n3 again (eol)"
# the newline added to b is a separate change and stays staged
assert_index_text eol.c 'a\nb\n'
carve unstage eol.c o2,n2 || fail "unstage newline change"
assert_index_text eol.c 'a\nb'

# the last line loses its newline, stage only the newline change
printf 'x\ny\n' >eol2.c
commit_all eol2
printf 'x\ny\nz' >eol2.c
carve stage eol2.c n3 || fail "stage z"
assert_index_text eol2.c 'x\ny\nz'

# two adjacent one-line changes, which git prints as "-a -b +A +B". Staging
# the a to A pair must keep its place, A before b, not move it below b.
printf 'x\ntimeout = 100;\nprobed devcie\ny\n' >pair.c
commit_all pair
printf 'x\ntimeout = 500;\nprobed device\ny\n' >pair.c
carve stage pair.c o2,n2 || fail "stage pair"
assert_index_text pair.c 'x\ntimeout = 500;\nprobed devcie\ny\n'
carve stage pair.c o3,n3 || fail "stage second pair"
assert_index pair.c pair.c
carve unstage pair.c o2,n2 || fail "unstage pair"
assert_index_text pair.c 'x\ntimeout = 100;\nprobed device\ny\n'
