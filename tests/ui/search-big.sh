#!/bin/sh
# SPDX-License-Identifier: MIT
# Search in a big change set. Above 500 changed files the search reads the
# files first and diffs only those that can hold the pattern. It must find
# what the diffs show, added, removed and context lines and staged
# changes, and not a match outside the hunks. A deleted file allows only
# whole-file operations and is not searched, as with fewer files.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

mkdir many
i=0
while [ $i -lt 600 ]; do
	echo x >many/f$i.c
	i=$((i + 1))
done
seq 1 20 | sed 's/^/l /' >a.c
seq 1 20 | sed 's/^/l /;12s/.*/needle ctx/' >k.c
seq 1 40 | sed 's/^/l /;1s/.*/needle far/' >far.c
printf 'one\nneedle gone\ntwo\n' >r.c
printf 'needle del\n' >del.c
printf 'one\n' >s.c
commit_all base

for f in many/*.c; do echo y >"$f"; done
sed '10a\
needle add' a.c >t && mv t a.c
sed '11s/.*/changed/' k.c >t && mv t k.c
sed '40s/.*/changed/' far.c >t && mv t far.c
printf 'one\ntwo\n' >r.c
rm del.c
printf 'one\nneedle staged\n' >s.c
git add s.c

tui_start
wait_screen "M a.c"
keys j j
wait_screen "Unstaged: a.c"
keys Tab / needle Enter
# added in a.c, context in k.c and removed in r.c
wait_screen "3 matches in unstaged changes"
cursor_row | grep -q '+needle add$' || fail "a: $(cursor_row)"
keys Escape
tui_quit

# the staged side, from the Staged pane
tui_start
wait_screen "M s.c"
keys Tab Tab
wait_active "Staged: s.c"
keys / needle Enter
wait_screen "1 match in staged changes"
tui_quit
