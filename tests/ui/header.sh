#!/bin/sh
# SPDX-License-Identifier: MIT
# The title line names the branch and HEAD, on a branch (also one with a
# comma, and with branch decorations hidden by the user's config),
# detached, and before the first commit. After a commit and an amend in the
# TUI, where title and Series pane are made without git log, it is the same
# as git says, with a subject over two lines on one.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

# before the first commit, the branch without a commit
echo a >f.c
tui_start
wait_screen "git-carve  main  (no commits)"
tui_quit

commit_all "ui: first"
id=$(git rev-parse --short HEAD)
git config log.excludeDecoration 'refs/heads/*'
git checkout -q -b 'a,b'
git branch -q up
git branch -q --set-upstream-to=up
cat >"$tmp/editor" <<X
#!/bin/sh
cat "$tmp/msg" >"\$1"
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"
tui_start
wait_screen "git-carve  a,b  $id ui: first"
echo b >>f.c
keys F5
wait_screen "M f.c"
keys Enter
wait_screen "Staged (1)"
printf 'ui: second\nover two lines\n\nbody\n' >"$tmp/msg"
keys c
wait_screen "committed"
id2=$(git rev-parse --short HEAD)
wait_screen "git-carve  a,b  $id2 ui: second over two lines"
# cut at the pane's edge, a mark says it goes on
wait_screen "HEAD  $id2 ui: second o>"
printf 'ui: amended\n' >"$tmp/msg"
keys A
wait_screen "amended HEAD"
id3=$(git rev-parse --short HEAD)
wait_screen "git-carve  a,b  $id3 ui: amended"
wait_screen "HEAD  $id3 ui: amended"
screen | grep -q "ui: second" && fail "amended commit still listed"
tui_quit
id=$id3

git checkout -q --detach
tui_start
wait_screen "git-carve  (detached)  $id ui: amended"
tui_quit

# a title longer than the screen stops before ?:help
git commit -q --allow-empty -m "ui: $(printf 'a long subject %.0s' 1 2 3 4 5 6 7 8)"
TUI_W=80 tui_start
wait_screen "?:help"
screen | head -n 1 | grep -q '?:help *$' ||
	fail "title over ?:help: $(screen | head -n 1)"
tui_quit
exit 0
