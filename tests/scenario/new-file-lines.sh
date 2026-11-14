#!/bin/sh
# SPDX-License-Identifier: MIT
# Line staging in a new untracked file. The first stage marks it
# intent-to-add, and unstaging everything returns it to that state.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'base\n' >base.c
commit_all base

printf 'l1\nl2\nl3\nl4\n' >new.c
carve diff new.c | grep -q '^          3 +l3$' || fail "diff of untracked"
carve stage new.c n3 || fail "stage n3"
assert_index_text new.c 'l3\n'
[ "$(short_status | grep new.c)" = "AM new.c" ] || fail "status after stage"

carve stage new.c n1 || fail "stage n1"
assert_index_text new.c 'l1\nl3\n'

carve unstage new.c n2 || fail "unstage n2 (cached numbering)"
assert_index_text new.c 'l1\n'
carve unstage new.c n1 || fail "unstage n1"
# back to intent-to-add, it shows up as an unstaged new file
[ "$(short_status | grep new.c)" = " A new.c" ] ||
	fail "not intent-to-add: $(short_status)"

# failing selection on an untracked file leaves it untracked
printf 'q\n' >other.c
must_fail carve stage other.c n5
[ "$(short_status | grep other.c)" = "?? other.c" ] || fail "other.c tracked"

# on an unborn branch, stage lines before the first commit
new_repo second
printf 'a\nb\n' >x.c
carve stage x.c n2 || fail "stage in unborn repo"
assert_index_text x.c 'b\n'
carve unstage x.c n1 || fail "unstage in unborn repo"
carve stage-file x.c || fail "stage-file unborn"
carve unstage-file x.c || fail "unstage-file unborn"
[ "$(short_status)" = "?? x.c" ] || fail "unborn unstage-file"
