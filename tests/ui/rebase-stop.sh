#!/bin/sh
# SPDX-License-Identifier: MIT
# A rebase of yours stopped at a commit. The Files pane shows that commit
# as for one of the Series pane, f moves lines of it into a new commit in a
# gap, and s splits it. Line operations on it say they do not apply. The
# rebase is left to you.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1; while [ $i -le 40 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
sed 's/^l 10$/l 10 P1/' a.c >a.tmp && mv a.tmp a.c
commit_all P1
sed -e 's/^l 20$/l 20 P2/' -e 's/^l 30$/l 30 helper/' a.c >a.tmp &&
	mv a.tmp a.c
commit_all P2
tip=$(git rev-parse HEAD^{tree})
cat >"$tmp/seq" <<'X'
#!/bin/sh
awk '$1 == "pick" && ($3 == "P2" || ($3 == "#" && $4 == "P2")) {
	$1 = "edit" } { print }' "$1" \
	>"$1.tmp" && mv "$1.tmp" "$1"
X
chmod +x "$tmp/seq"
GIT_SEQUENCE_EDITOR="$tmp/seq" git rebase -q -i HEAD~2 >/dev/null 2>&1
[ "$(git log -1 --format=%s)" = P2 ] || fail "not stopped at P2"
cat >"$tmp/editor" <<X
#!/bin/sh
cp "\$1" "$tmp/template"
printf 'helper first\\n' >"\$1"
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

tui_start
wait_screen 'REBASE 2/2'
wait_screen 'git rebase --continue goes on'
wait_screen 'Commit HEAD (1)'
keys Tab
wait_screen '> Commit HEAD: a.c'
# its lines move with f, into a gap as a new commit, not by staging
keys O
wait_screen "on a commit's lines: Space takes them out"
close_box
keys / 'l 30 helper' Enter t
wait_screen 'Move the lines into which commit?'
# the whole rebased range, P1 below HEAD too
wait_screen "-- a new commit before $(git rev-parse --short HEAD) --"
wait_screen " $(git rev-parse --short HEAD~1) P1"
wait_screen '-- rebase onto '
# the gap below HEAD, for a new commit right before it
keys j Enter
wait_screen 'carved 1 changed line of a.c into a new commit before it'
[ "$(git log -1 --format=%s HEAD~1)" = "helper first" ] || fail "new commit"
# its editor says what was carved and from where, not git's rebase status
grep -q "^# Lines carved out of [0-9a-f]* \"P2\"," "$tmp/template" ||
	fail "template: $(cat "$tmp/template")"
grep -q "^#     +l 30 helper$" "$tmp/template" || fail "template lines"
grep -q "rebas" "$tmp/template" && fail "git's status: $(cat "$tmp/template")"
[ "$(git log -1 --format=%s HEAD)" = P2 ] || fail "P2 moved"
git show HEAD | grep -q '^+l 30 helper$' && fail "helper still in P2"
[ -d "$(git rev-parse --git-path rebase-merge)" ] || fail "rebase gone"
carve undo --list | grep -q 'move a.c' || fail "no undo entry"

# f into P1, below HEAD, is done at once in a worktree of its own, and the
# rebase goes on from the same tree
at=$(git rev-parse HEAD^{tree})
keys C-l
wait_screen 'Commit HEAD (1)'
keys / 'l 20 P2' Enter v k t
wait_screen 'Move the lines into which commit?'
keys j j j j Enter
wait_screen 'moved from '
[ "$(git log -1 --format=%s HEAD~2)" = P1 ] || fail "P1 subject"
git show HEAD~2 | grep -q '^+l 20 P2$' || fail "not in P1"
git show HEAD | grep -q '^+l 20 P2$' && fail "still in P2"
[ "$(git rev-parse HEAD^{tree})" = "$at" ] || fail "tree at the stop"
[ -d "$(git rev-parse --git-path rebase-merge)" ] || fail "rebase gone 2"
[ "$(git worktree list | wc -l | tr -d ' ')" = 1 ] || fail "worktree left"

# s splits the commit, and abort brings it back, in the rebase
keys s
wait_screen 'Split HEAD'
keys y
wait_screen '(in your rebase)'
keys X
wait_screen 'Abort the split'
keys y
wait_screen 'split aborted'
wait_screen 'REBASE'
[ "$(git log -1 --format=%s HEAD)" = P2 ] || fail "abort: HEAD"
keys q
_n=$WAIT_TRIES
while tmux -L "$TMUX_SOCK" has-session -t t 2>/dev/null; do
	_n=$((_n - 1))
	[ "$_n" -gt 0 ] || fail "did not quit"
	sleep 0.1
done
git rebase --continue >/dev/null 2>&1 || fail "rebase --continue"
[ "$(git rev-parse HEAD^{tree})" = "$tip" ] || fail "tip changed"
check_sanitizers
