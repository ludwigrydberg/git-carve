#!/bin/sh
# SPDX-License-Identifier: MIT
# gD with files changed just now and diffs longer than the pane. The diff
# cache does not trust a file changed within two seconds of reading it, so
# it reads them again, and the files after the first one show as loading
# until then. Moving down from the last row of the pane into the next file
# scrolls, and the bar of that file and the cursor stay inside the frame.
# } goes to the next file.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

for f in a:12 b:60 c:12; do
	i=1
	while [ $i -le "${f#*:}" ]; do
		echo "${f%:*} line $i"
		i=$((i + 1))
	done >"${f%:*}.c"
done
commit_all base
sed 's/^a line 6$/a line 6X/' a.c >t && mv t a.c
sed 's/^\(b line [1-5]0\)$/\1X/' b.c >t && mv t b.c
sed 's/^c line 9$/c line 9X/' c.c >t && mv t c.c
git config carve.layout one

tui_start
wait_screen "M a.c"
keys g D Tab
wait_active "Diff (unstaged): a.c  [all files]"
# while the cache reads them again they are loading, not left for later
screen | grep -q "shown when the cursor" && fail "not loading: $(screen)"
wait_screen "b line 10X"

step_keys '}'
wait_active "Diff (unstaged): b.c  [all files]"
step_keys G
cursor_row | grep -q 'b line 53' || fail "not at the end of b.c: $(screen)"
step_keys j
wait_active "Diff (unstaged): c.c  [all files]"
cursor_row | grep -q '@@ -6,7 +6,7 @@' ||
	fail "the cursor is not on c.c's first row: $(screen)"
screen | cut -c33- | grep -q '^ M c\.c ' || fail "no bar of c.c: $(screen)"
tui_quit
