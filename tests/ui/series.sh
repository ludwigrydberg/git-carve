#!/bin/sh
# SPDX-License-Identifier: MIT
# S splits an older commit chosen from a list, and f moves the selected
# lines into an older commit. A conflict stops with a status line, and s
# and X continue or abort.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'x\n' >base.c
commit_all C1
printf 'a\nretrun;\n' >a.c
printf 'b\n' >b.c
commit_all "C2: add a and b"
printf 'c\n' >c.c
commit_all "C3: add c"
printf 'd\n' >d.c
commit_all "C4: add d"
tip_tree=$(tree_of HEAD)

cat >"$tmp/editor" <<'X'
#!/bin/sh
if [ -z "$(head -n 1 "$1")" ]; then
	{ echo "C2b: add b"; cat "$1"; } >"$1.new" && mv "$1.new" "$1"
fi
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

tui_start
wait_screen "Nothing to stage"
# the commit shown in the Series pane, C2 after J J, is the one s splits
keys J J
wait_screen "Commit ~2"
keys s
wait_screen "the commits above it are replayed"
keys y
wait_screen "older commit"
wait_screen "SPLIT "
wait_screen '"C2: add'
wait_screen "Unstaged (2)"
# the first commit is a.c with the original message, then b.c
keys Enter
wait_screen "Staged (1)"
keys c
wait_screen "1 commit made"
keys Enter
wait_screen "Staged (1)"
keys c
wait_screen "nothing left"
keys Z Z
wait_screen "split finished: 2 commits"
[ "$(git rev-list --count HEAD)" = 5 ] || fail "count after split"
[ "$(tree_of HEAD)" = "$tip_tree" ] || fail "tip tree after split"
[ "$(git log -1 --format=%s HEAD~2)" = "C2b: add b" ] || fail "C2b"
[ "$(git log -1 --format=%s HEAD~3)" = "C2: add a and b" ] || fail "C2a"

# f fixes the typo in a.c, added by C2a and now HEAD~3, from the Unstaged
# pane
printf 'a\nreturn;\n' >a.c
keys C-l
wait_screen "Unstaged (1)"
# the work tree's a.c, below the commit shown
keys G Tab
wait_active "Unstaged: a.c"
keys : o 2 Enter v j t
wait_screen "Move the change into which commit?"
keys j j j Enter
wait_screen 'moved into "C2: add a and b"'
git show HEAD~3:a.c | grep -q '^return;$' || fail "fix not in C2a"
[ -z "$(short_status)" ] || fail "status after f: $(short_status)"
[ "$(git rev-list --count HEAD)" = 5 ] || fail "count after f"
tui_quit

# a conflict shows on the status line, then resolve, stage with Enter, and
# ZZ
new_repo conflict
printf 'l1\nl2\nl3\n' >f.c
commit_all C1
printf 'l1\nL2 typo\nl3\n' >f.c
commit_all C2
printf 'l1\nL2 typo\nL3 changed\n' >f.c
commit_all C3
printf 'l1\nL2 fixed\nL3 changed\n' >f.c
must_fail carve fixup-to HEAD~1 f.c o2,n2
tui_start
wait_screen "FIXUP-TO "
wait_screen "1 conflicted file"
wait_screen "ZZ: continue  X: abort"
wait_screen "U f.c"
printf 'l1\nL2 fixed\nL3 changed\n' >f.c
keys Enter
wait_screen "Staged"
keys Z Z
wait_screen "done"
wait_gone "FIXUP-TO"
git show HEAD~1:f.c | grep -q 'L2 fixed' || fail "fix not in C2"
[ "$(git show HEAD:f.c)" = "$(printf 'l1\nL2 fixed\nL3 changed')" ] ||
	fail "C3 content"
tui_quit
