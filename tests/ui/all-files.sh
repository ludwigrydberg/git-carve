#!/bin/sh
# SPDX-License-Identifier: MIT
# gD shows the diffs of all files of a section one after another. Each file
# has a bar with its path, the files after the one shown come from the diff
# cache, and moving past the end of a file goes on into the next, with no
# git. The Files cursor follows, and Space stages in the file the cursor is
# in. A range stays within one file.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

for f in a b c; do
	i=1
	while [ $i -le 12 ]; do echo "$f line $i"; i=$((i + 1)); done >$f.c
done
commit_all base
for f in a b c; do
	sed "s/^$f line 6\$/$f line SIX/" $f.c >t && mv t $f.c
done
# older than the diff cache's two seconds, which it trusts at once
touch -t 202001010000 a.c b.c c.c
# one diff pane, as high as the screen
git config carve.layout one

: >"$tmp/trace"
TUI_ENV="$TUI_ENV GIT_CARVE_TRACE=$tmp/trace" tui_start
wait_screen "M a.c"
keys g D Tab
wait_active "Diff (unstaged): a.c  [all files]"
# a.c, then b.c and c.c below it from the cache
wait_screen "b line SIX"
wait_screen "c line SIX"
screen | grep -q ' M b\.c  *+1 -1' || fail "no bar of b.c: $(screen)"

# down past the end of a.c into b.c, with no git
: >"$tmp/trace"
step_keys G j
wait_active "Diff (unstaged): b.c  [all files]"
wait_fcur 'b\.c'
[ -s "$tmp/trace" ] && fail "git on the way into b.c: $(cat "$tmp/trace")"
# the end of a.c stays above b.c
screen | grep -q 'a line 9' || fail "the end of a.c went: $(screen)"

# Space stages the changed line of b.c (both halves, gm), the file the
# cursor is in
step_keys : n 6 Enter g m Space
wait_index b.c 'b line 1\nb line 2\nb line 3\nb line 4\nb line 5\nb line SIX\nb line 7\nb line 8\nb line 9\nb line 10\nb line 11\nb line 12\n'
[ -z "$(git diff --cached --name-only -- a.c c.c)" ] || fail "staged elsewhere"

# b.c has no unstaged changes left, and c.c, the next file, follows
wait_active "Diff (unstaged): c.c  [all files]"
wait_fcur 'c\.c'
# up past the start of c.c into the end of a.c
step_keys g g k
wait_active "Diff (unstaged): a.c  [all files]"

# a range stops at the end of a file
step_keys G v j
wait_screen "a range stays within one file"
wait_active "Diff (unstaged): a.c  [all files]"
step_keys Escape

# gD again shows one file
step_keys g D
wait_active "Diff (unstaged): a.c"
screen | grep -q 'all files' && fail "still all files"
screen | grep -q 'c line SIX' && fail "c.c still shown"
tui_quit
