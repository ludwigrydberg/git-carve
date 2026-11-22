#!/bin/sh
# SPDX-License-Identifier: MIT
# gD with files changed just now. The diff cache does not trust a file
# changed within two seconds of reading it, so it reads them again and the
# files after the first one show.
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
wait_screen "b line 10X"

tui_quit
