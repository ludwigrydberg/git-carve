#!/bin/sh
# SPDX-License-Identifier: MIT
# The diff cache. Once the keys pause, the diffs of every file come from
# one git diff of the tree in the background. What the panes show from it
# must be what git says, after moving, after a file changed outside (by its
# stat, also when the edit keeps size and mtime), after a line was staged
# (that file is read again), and after the index changed outside (all of it
# is read again). A file changed within the last seconds is not trusted,
# racy as in git, and the fixture's files are made older.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

for f in a b c; do
	awk -v f=$f 'BEGIN { for (i = 1; i <= 30; i++) print f " " i }' >$f.c
done
commit_all base
for f in a b c; do
	awk -v f=$f 'BEGIN { for (i = 1; i <= 30; i++)
		print (i % 10 == 3 ? "X" f " " : f " ") i }' >$f.c
	touch -t 202001010000 $f.c
done

TUI_ENV="GIT_CARVE_TRACE=$tmp/tr" tui_start
wait_screen ">M a.c"
wait_screen "+Xa 3"
wait_gone "scanning untracked"
wait_idle	# the cache is made
: >"$tmp/tr"
keys j
wait_screen "Unstaged: b.c"
wait_screen "+Xb 3"
keys j
wait_screen "Unstaged: c.c"
wait_screen "+Xc 3"
wait_idle
[ "$(grep -c ' diff ' "$tmp/tr")" = 0 ] ||
	fail "diffs while moving: $(cat "$tmp/tr")"

# b.c changed outside shows its new diff, not the cached one
sed 's/^b 1$/b 1 outside/' b.c >b.tmp && mv b.tmp b.c
keys k
wait_screen "Unstaged: b.c"
wait_screen "+b 1 outside"

# changed outside keeping size and mtime (cp -p, rsync -t, tar x), the
# ctime tells, and F5 does not show the old diff either
keys k
wait_screen "Unstaged: a.c"
wait_idle	# the cache is made again (b.c changed)
same_stat_edit() {	# <from> <to>: c.c edited, size and mtime kept
	cp -p c.c c.ref
	sed "s/^$1 3\$/$2 3/" c.ref >c.c
	touch -r c.ref c.c
	rm c.ref
}
same_stat_edit Xc Yc
keys j j
wait_screen "Unstaged: c.c"
wait_screen "+Yc 3"
screen | grep -q "+Xc 3" && fail "old diff of c.c shown"
wait_idle	# the cache is made again
same_stat_edit Yc Zc
keys F5
wait_screen "+Zc 3"
git checkout -q c.c
awk 'BEGIN { for (i = 1; i <= 30; i++)
	print (i % 10 == 3 ? "Xc " : "c ") i }' >c.c
touch -t 202001010000 c.c
keys F5
wait_screen "+Xc 3"
keys k

# a line of a.c staged, a.c is read again and c.c still comes from the
# cache
keys k Tab
wait_screen "> Unstaged: a.c"
keys : n 3 Enter Space
wait_screen "staged 1 line"
wait_screen "Staged: a.c"
screen | grep -q "^.*+Xa 3" || fail "staged line not shown"
keys BTab
wait_screen "> Files"
keys j j
wait_screen "Unstaged: c.c"
wait_screen "+Xc 3"

# the index changed outside, and the staged pane shows it without F5
git add b.c
keys k
wait_screen "Unstaged: b.c"
wait_screen "+b 1 outside"
keys j
wait_screen "Unstaged: c.c"
keys k
wait_screen "Unstaged: b.c"
wait_screen "(no unstaged changes)"
screen | grep -q "(no staged changes)" && fail "staged b.c not shown"
keys j

# after F5 the truth
git add c.c
keys C-l
wait_screen "Staged: c.c"
wait_screen "(no unstaged changes)"
git diff --cached c.c | grep -q '^+Xc 3' || fail "c.c not staged"
screen | grep -q "+Xc 3" || fail "staged diff of c.c not shown"
tui_quit
exit 0
