#!/bin/sh
# SPDX-License-Identifier: MIT
# Files whose diff would hang the screen are not loaded, tag files by name
# (carve.skipDiff) and whole files over carve.maxFileSize by size only. gd
# loads one anyway. Any key stops a slow load, and the screen answers
# meanwhile. gi puts an untracked file into .git/info/exclude. When set,
# carve.skipDiff replaces the tag files, and an empty value clears it.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

echo a >f.c
commit_all base
echo b >>f.c
# a tag file of some 20 MB, text, which git would diff line by line
yes 'carve_line	src/drv.c	/^int carve_line(void)$/;"	f' |
	head -n 400000 >tags
awk 'BEGIN { for (i = 0; i < 20000; i++) print "line " i }' >big.txt
echo slow >slow.txt
git config carve.maxFileSize 100k
# the diff of slow.txt takes its time
git_shim 'case "$*" in *--no-index*slow.txt*) sleep 30 ;; esac'

tui_start
# the scan is done, and / finds a file of the folded Untracked section
wait_screen 'Untracked (3) [+]'
keys / tags Enter
wait_screen 'not loaded: carve.skipDiff names it'
# over carve.maxFileSize, a note, found without reading the file
keys / big.txt Enter
wait_screen 'a whole file of 204 KB, over carve.maxFileSize'
keys g d
wait_screen '1 +line 0'
# a slow load tells, and a key stops it
keys / slow.txt Enter
wait_screen 'loading the diff of slow.txt (any key stops it)'
keys Escape
wait_screen 'loading stopped by a key: gd loads it'
# the screen answers, move away and back
keys k j
wait_screen 'loading the diff of slow.txt'
# away again (the Untracked row, all the filter leaves): it stops
keys k
wait_gone 'loading the diff of slow.txt'
# after gi it is never listed again
keys / slow.txt Enter
wait_screen 'loading the diff of slow.txt'
keys Escape
wait_screen 'loading stopped by a key'
keys g i
wait_screen 'Add /slow.txt to .git/info/exclude?'
keys y
wait_screen 'slow.txt is ignored now'
grep -q '^/slow\.txt$' "$(git rev-parse --git-path info/exclude)" ||
	fail "not in info/exclude"
wait_gone '? slow.txt'
tui_quit

# with carve.skipDiff set, its patterns replace the tag files, and an empty
# value clears the ones before it
printf 'gen\n' >a.gen
printf 'kept out\n' >b.out
git config carve.skipDiff '*.out'
git config --add carve.skipDiff ''
git config --add carve.skipDiff '*.gen'
tui_start
wait_screen 'Untracked (4) [+]'
keys / a.gen Enter
wait_screen 'not loaded: carve.skipDiff names it'
keys / b.out Enter
wait_screen '+kept out'
tui_quit

# staged, and in a commit, the size comes from the index entry and the
# commit through the cat-file worker, still without reading the file
git add big.txt
git commit -q -m "add big"
{ cat big.txt; echo more; } >big2.txt
git add big2.txt
tui_start
keys / big2.txt Enter
wait_screen 'Staged: big2.txt'
wait_screen 'a whole file of 204 KB, over carve.maxFileSize'
# the filter (/big2.txt) would leave HEAD's big.txt out, and Esc clears it
keys Escape
wait_screen 'filter cleared'
keys J
wait_screen 'Commit HEAD'
wait_screen 'Commit HEAD: big.txt'
wait_screen 'a whole file of 204 KB, over carve.maxFileSize'
tui_quit
exit 0
