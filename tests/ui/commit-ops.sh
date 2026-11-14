#!/bin/sh
# SPDX-License-Identifier: MIT
# Changing the commits of the Series pane from their diffs. Space takes
# lines out into the work tree, f moves them (to a gap, as a new commit
# there), d drops them, r gives a commit a new message, and s splits an
# older commit.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1; while [ $i -le 40 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
edit "l 10" "l 10 P1"; commit_all P1
edit "l 20" "l 20 P2"; edit "l 30" "l 30 P2x"; commit_all P2
edit "l 35" "l 35 P3"; edit "l 38" "l 38 P3y"; commit_all P3
git branch -q up HEAD~3
git branch -q --set-upstream-to=up
orig=$(git rev-parse HEAD)
tip=$(tree_of HEAD)
# the editor writes $tmp/msg
cat >"$tmp/editor" <<X
#!/bin/sh
cp "\$1" "$tmp/template"
cp "$tmp/msg" "\$1"
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

tui_start
wait_screen 'Commit HEAD (1)'
# Space takes P2's l 30, both halves, out of it into the work tree
keys J
wait_screen 'Commit ~1 (1)'
keys Tab / 'l 30 P2x' Enter v k Space
wait_screen 'taken out of '
[ "$(changes HEAD~1)" = "-l 20 +l 20 P2 " ] || fail "out: $(changes HEAD~1)"
git diff | grep -q '^+l 30 P2x$' || fail "not in the work tree: $(git diff)"
git reset -q --hard "$orig"
keys C-l
wait_screen 'l 30 P2x'
# f moves P2's l 20 into a new commit in the gap below P2
printf 'P2a\n' >"$tmp/msg"
keys / 'l 20 P2' Enter v k t
wait_screen 'Move the lines into which commit?'
wait_screen '-- a new commit before '
keys j Enter
wait_screen 'into a new commit'
[ "$(git log --format=%s | tr '\n' ' ')" = "P3 P2 P2a P1 base " ] ||
	fail "f: $(git log --format=%s)"
# the new commit's editor says which lines and from where, and nothing of
# git's
grep -q "^# Lines moved out of [0-9a-f]* \"P2\"," "$tmp/template" ||
	fail "template: $(cat "$tmp/template")"
grep -q "^#     +l 20 P2$" "$tmp/template" || fail "template lines"
grep -q "rebas" "$tmp/template" && fail "git's status: $(cat "$tmp/template")"
[ "$(tree_of HEAD)" = "$tip" ] || fail "f: tip changed"
# d drops P3's l 38, from the work tree too
keys K
wait_screen 'Commit HEAD (1)'
keys Tab / 'l 38 P3y' Enter v k d
wait_screen 'Drop the selected lines'
keys y
wait_screen 'dropped from '
grep -q 'P3y' a.c && fail "dropped line in the work tree"
[ "$(changes HEAD)" = "-l 35 +l 35 P3 " ] || fail "d: $(changes HEAD)"
# r gives P1 (~3) a new message in the editor
printf 'P1 better\n' >"$tmp/msg"
keys J J J
wait_screen 'Commit ~3 (1)'
keys r
wait_screen 'has its new message'
[ "$(git log -1 --format=%s HEAD~3)" = "P1 better" ] ||
	fail "r: $(git log --format=%s)"
# s on an older commit splits that one, and X aborts
keys s
wait_screen 'the commits above it are replayed'
keys y
wait_screen 'SPLIT'
wait_screen '(older commit)'
keys X
wait_screen 'Abort the split'
keys y
wait_screen 'split aborted'
[ "$(git log -1 --format=%s HEAD~3)" = "P1 better" ] || fail "abort"
tui_quit

# e edits lines of an older commit as the commit has them
tui_start
keys J J
wait_screen 'Commit ~2 (1)'
printf 'l 10 EDITED\n' >"$tmp/msg"
keys Tab / 'l 10 P1' Enter e
wait_screen 'has the lines as edited'
git show HEAD~2 | grep -q '^+l 10 EDITED$' || fail "e: $(git show HEAD~2)"
grep -q '^l 10 EDITED$' a.c || fail "e: the work tree"
tui_quit
