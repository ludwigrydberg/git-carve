#!/bin/sh
# SPDX-License-Identifier: MIT
# u undoes the last change to the index or HEAD, such as a staged line or a
# commit. When the index changed behind its back it asks before forcing.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\nb\nc\n' >f.c
commit_all base
base=$(git rev-parse HEAD)
printf 'a\nB\nc\nd\n' >f.c
wt=$(worktree_state)

cat >"$tmp/editor" <<X
#!/bin/sh
printf 'ui: change f\n' >"\$1"
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

tui_start
wait_screen "M f.c"
keys Tab : n4 Enter Space
wait_index f.c 'a\nb\nc\nd\n'
keys u
wait_screen "undid stage f.c"
wait_index f.c 'a\nb\nc\n'
keys u
wait_screen "nothing to undo"
close_box

# a commit undone, HEAD back and the changes staged again
keys BTab Enter c
wait_screen "committed"
[ "$(git rev-list --count HEAD)" = 2 ] || fail "commit"
keys u
wait_screen "undid commit"
[ "$(git rev-parse HEAD)" = "$base" ] || fail "HEAD after u"
assert_index_text f.c 'a\nB\nc\nd\n'
[ "$(worktree_state)" = "$wt" ] || fail "work tree touched"

# the index changed outside, it asks, n keeps it and y forces
git reset -q
keys u
wait_screen "Undo anyway? [y/N]"
keys n
wait_screen "not undone"
assert_index_text f.c 'a\nb\nc\n'
keys u
wait_screen "Undo anyway? [y/N]"
keys y
wait_screen "undid stage-file f.c"
assert_index_text f.c 'a\nb\nc\n'
tui_quit
