#!/bin/sh
# SPDX-License-Identifier: MIT
# The Series pane of git carve. The range (@{u}..HEAD) newest first under
# the work tree, a divider at the upstream, and carve.seriesStep older
# commits loaded as the cursor gets past the last one. Until a row is
# chosen the work tree is shown while it has changes, and HEAD when it is
# clean.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1
while [ $i -le 8 ]; do
	echo "c$i" >>a.c
	commit_all "C$i"
	i=$((i + 1))
done
git branch -q up HEAD~2
git branch -q --set-upstream-to=up
git config carve.seriesStep 2

tui_start
# clean, HEAD is shown, and the range is C8, C7, then the upstream
wait_screen 'Commit HEAD (1)'
inner | grep -q -E '^\* HEAD +[0-9a-f]+ C8' || fail "HEAD: $(screen | head -n 8)"
inner | grep -q -E '^  ~1 +[0-9a-f]+ C7' || fail "~1: $(screen | head -n 8)"
wait_screen '-- up --'
screen | grep -q ' C6' && fail "C6 loaded before it was reached"
# past the range, two older commits at a time
keys J J
wait_screen 'Commit ~2 (1)'
screen | grep -q -E ' ~3 +[0-9a-f]+ C5' || fail "C5 not loaded: $(screen | head -n 10)"
screen | grep -q ' C4' && fail "more than carve.seriesStep loaded"
keys J J
wait_screen 'Commit ~4 (1)'
wait_screen ' C3'
# a change in the work tree does not move a chosen row
echo x >>a.c
keys C-l
wait_screen 'Unstaged (1)'
screen | grep -q 'Commit ~4 (1)' || fail "chosen row moved: $(screen | head -n 12)"
tui_quit

# until a row is chosen, the work tree while it has changes
tui_start
wait_screen '*  work tree'
screen | grep -q 'Commit HEAD' && fail "HEAD shown with changes"
git checkout -q a.c
keys C-l
wait_screen 'Commit HEAD (1)'
tui_quit
