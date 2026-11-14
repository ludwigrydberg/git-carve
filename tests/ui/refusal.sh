#!/bin/sh
# SPDX-License-Identifier: MIT
# A refused or failed operation shows its reason in a box mid-screen,
# wrapped to the terminal, with what to do apart below it. The next key
# only closes it. A long one, such as a hook's output, scrolls. A search
# that finds nothing stays on the bottom line.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\n' >f.c
commit_all base
printf 'b\n' >f.c

# split with changes in the tree is refused, in an 80x24 terminal
TUI_W=80 TUI_H=24 tui_start
wait_screen "M f.c"
keys s
# refused at once, not after asking
wait_screen "Can't split"
screen | grep -q 'Split HEAD' && fail "asked first"
wait_screen "the index or work tree has changes"
wait_screen "commit or stash them first"
wait_screen "any key: close"
# what to do on a line of its own, below the reason
screen | grep -q 'changes; commit' && fail "reason and fix on one line"
# Enter closes it and does not stage
keys Enter
wait_gone "Can't split"
[ -z "$(git diff --cached --name-only)" ] || fail "the key staged too"
# a search that finds nothing uses the bottom line, no box
keys Tab / nosuchtext Enter
wait_screen "not found in any unstaged diff: nosuchtext"
screen | grep -q "key: close" && fail "a box for a search"
tui_quit

# narrow, wrapped, nothing cut off
TUI_W=44 TUI_H=30 tui_start
wait_screen "M f.c"
keys s
wait_screen "the index or work tree has"
screen | grep -q 'stash them first' || fail "cut: $(screen)"
tui_quit

# a failing hook, its output in the box, scrolled with the arrow keys
mkdir -p .git/hooks
cat >.git/hooks/pre-commit <<'X'
#!/bin/sh
i=1; while [ $i -le 30 ]; do echo "hook line $i" >&2; i=$((i + 1)); done
exit 1
X
chmod +x .git/hooks/pre-commit
git add f.c
TUI_W=80 TUI_H=24 tui_start
wait_screen "M f.c"
keys c
wait_screen "Can't commit"
wait_screen "any other key: close"
screen | grep -q 'hook line 1$\|hook line 1 ' || fail "first line: $(screen)"
keys Down
wait_screen "(lines 2-"
keys End
wait_gone "Can't commit"
[ "$(git rev-list --count HEAD)" = 1 ] || fail "committed"
tui_quit
