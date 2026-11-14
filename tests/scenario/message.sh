#!/bin/sh
# SPDX-License-Identifier: MIT
# The commit message being written, the TUI's Message pane, is kept in
# $GIT_DIR/CARVE_MESSAGE and committed with commit --draft, leaving out
# comments and what follows a scissors line. Then it is emptied. An empty
# one starts from the editor template, the subject prefix and the sign-off.
. "$(dirname "$0")/../lib.sh"
new_repo

mkdir drv
printf 'a\n' >drv/a.c
commit_all base
printf 'b\n' >drv/a.c
commit_all "drv: change a"
printf 'c\n' >drv/a.c
git add drv/a.c

# nothing yet
[ -z "$(carve message)" ] || fail "a message before one was written"
# the template, the prefix of the history, then the sign-off
carve message --template >"$tmp/t"
head -n 1 "$tmp/t" | grep -q '^drv: $' || fail "template: $(cat "$tmp/t")"
git config carve.signoff true
carve message --template | grep -q '^Signed-off-by: T <t@t>$' ||
	fail "no sign-off: $(carve message --template)"
git config --unset carve.signoff

# set, read back, commit with it
printf 'drv: c\n\nBody.\n# a note\n# ------------------------ >8 ------------------------\nbelow\n' |
	carve message --set - || fail "set"
carve message | grep -q '^drv: c$' || fail "read back"
[ -f .git/CARVE_MESSAGE ] || fail "not in the git dir"
must_fail carve commit --draft -m x
carve commit --draft || fail "commit --draft"
[ "$(git log -1 --format=%B)" = "$(printf 'drv: c\n\nBody.')" ] ||
	fail "message: $(git log -1 --format=%B)"
[ -f .git/CARVE_MESSAGE ] && fail "not emptied"

# an empty one (comments only) commits nothing
printf 'd\n' >drv/a.c
git add drv/a.c
printf '# only a comment\n' | carve message --set -
must_fail carve commit --draft
grep -q 'the commit message is empty' "$tmp/must_fail.out" ||
	fail "empty: $(cat "$tmp/must_fail.out")"
[ "$(git log -1 --format=%s)" = "drv: c" ] || fail "committed anyway"
carve message --clear
[ -f .git/CARVE_MESSAGE ] && fail "not cleared"
exit 0
