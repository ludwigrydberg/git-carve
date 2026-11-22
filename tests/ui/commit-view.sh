#!/bin/sh
# SPDX-License-Identifier: MIT
# A commit of the Series pane is a section of the Files pane with its diff
# in the upper pane. } and { move through its files, / searches them, E
# opens the work-tree line of a commit line, and J and K show the next.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1; while [ $i -le 30 ]; do echo "l $i"; i=$((i + 1)); done >a.c
printf 'b 1\nb 2\nb 3\n' >b.c
commit_all base
git branch -q up
git branch -q --set-upstream-to=up
sed 's/^l 10$/l 10 P1/' a.c >a.tmp && mv a.tmp a.c
sed 's/^b 3$/b 3 P1/' b.c >b.tmp && mv b.tmp b.c
commit_all P1
sed 's/^l 20$/l 20 P2/' a.c >a.tmp && mv a.tmp a.c
commit_all P2

cat >"$tmp/editor" <<X
#!/bin/sh
echo "\$@" >"$tmp/editor.args"
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

tui_start
wait_screen 'Commit HEAD (1)'
wait_screen '+l 20 P2'
keys J
# the commit's files, and its diff without a key
wait_screen 'Commit ~1 (2)'
wait_screen '>M a.c'
wait_screen 'Commit ~1: a.c'
wait_screen '+l 10 P1'
# } and { go through the commit's files
keys Tab
wait_active 'Commit ~1: a.c'
keys '}'
wait_screen 'Commit ~1: b.c'
wait_screen '+b 3 P1'
keys '{'
wait_screen 'Commit ~1: a.c'
# a search goes through the commit's files
keys / P1 Enter
wait_screen 'matches in commit changes'
keys g /
wait_screen '2 matches for "P1" in commit changes'
wait_screen 'b.c:n3: +b 3 P1'
keys Escape
wait_gone '2 matches for'

# e on a file of the commit in the Files pane opens the work-tree file
wait_screen 'Commit ~1: a.c'
keys BTab
wait_active 'Files'
keys e
# the editor starts once e is handled, wait for it rather than a fixed time
n=0
while [ "$n" -lt "$WAIT_TRIES" ] && [ ! -s "$tmp/editor.args" ]; do
	sleep 0.1
	n=$((n + 1))
done
[ "$(cat "$tmp/editor.args" 2>/dev/null)" = "a.c" ] ||
	fail "e: $(cat "$tmp/editor.args" 2>/dev/null)"

# K shows the newer commit again, its files only
keys C-l K
wait_screen 'Commit HEAD (1)'
wait_screen '+l 20 P2'
screen | grep -q -F ' b.c' && fail "b.c is not in P2: $(screen)"
tui_quit
exit 0
