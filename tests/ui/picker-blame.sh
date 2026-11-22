#!/bin/sh
# SPDX-License-Identifier: MIT
# b in the commit pickers of f and t moves the cursor to the commit that
# last touched the lines of the change (for an added line, the lines it
# replaces), and the footer says what it found. Enter takes it.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\n' >a.c
commit_all base
printf 'one\ntwo\n' >b.c
commit_all "drv: add b"
printf 'c\n' >c.c
commit_all "drv: add c"
printf 'd\n' >d.c
commit_all "drv: add d"
printf 'ONE\ntwo\n' >b.c
printf 'C\n' >c.c

# the picker's cursor row, ">id subject"
picked() {
	_n=30
	until screen | grep -q ">[0-9a-f]\{7,\} $1"; do
		_n=$((_n - 1))
		[ "$_n" -gt 0 ] || fail "cursor not on $1: $(screen)"
		sleep 0.1
	done
}

tui_start
wait_screen "M b.c"
# f on one -> ONE
keys Tab : o1 Enter v j f
wait_screen "A fixup! commit for which commit?"
wait_screen "b: blame"
picked "drv: add d"
keys b
picked "drv: add b"
wait_screen "b: the line comes from this commit"
keys Enter
wait_screen 'fixup! commit for "drv: add b" made'
[ "$(git log -1 --format=%s)" = "fixup! drv: add b" ] ||
	fail "subject: $(git log -1 --format=%s)"
[ "$(git show HEAD:b.c)" = "$(printf 'ONE\ntwo')" ] || fail "fixup content"

# t from the Files pane takes the whole change of c.c, the only file left
_n=4
until active_title | grep -q '^Files'; do
	_n=$((_n - 1))
	[ "$_n" -gt 0 ] || fail "no Files pane: $(screen)"
	step_keys Tab
done
wait_screen "M c.c"
keys t
wait_screen "Move the change into which commit?"
picked "fixup! drv: add b"
keys b
picked "drv: add c"
keys Enter
wait_screen 'moved into "drv: add c"'
[ "$(git show HEAD~2:c.c)" = C ] || fail "t content: $(git log --stat)"
[ "$(git log -1 --format=%s HEAD~2)" = "drv: add c" ] || fail "t target"
[ -z "$(git status --porcelain)" ] || fail "left: $(git status --porcelain)"
tui_quit
