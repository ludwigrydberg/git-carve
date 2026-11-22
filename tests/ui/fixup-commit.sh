#!/bin/sh
# SPDX-License-Identifier: MIT
# f makes a fixup! commit of the selected lines for a commit chosen from a
# list, which starts on the commit shown in the Series pane. Nothing is
# rewritten and the other lines stay unstaged. Esc in the list stages
# nothing. F makes an amend! commit with the target's message in the
# editor. S squashes them, after asking.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\n' >a.c
commit_all base
git branch -q up
git branch -q --set-upstream-to=up
printf 'one\ntwo\n' >b.c
commit_all "drv: add b"
printf 'c\n' >c.c
commit_all "drv: add c"
tip=$(git rev-parse HEAD)
printf 'ONE\ntwo\nthree\n' >b.c
# the editor appends a line to the message (amend!)
cat >"$tmp/editor" <<'X'
#!/bin/sh
awk '/^amend! / { print; getline; print; getline; print "drv: add b, better"; next }
	{ print }' "$1" >"$1.new" && mv "$1.new" "$1"
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

tui_start
wait_screen "M b.c"
# Esc in the list stages and commits nothing
keys Tab : n1 Enter f
wait_screen "A fixup! commit for which commit?"
step_keys Escape
[ -z "$(git diff --cached --name-only)" ] || fail "Esc staged something"
[ "$(git rev-parse HEAD)" = "$tip" ] || fail "Esc committed"
# after J J the commit shown is drv: add b, its file first, then the work
# tree's b.c under Unstaged
keys J J
wait_screen "Commit ~1"
_n=4
until active_title | grep -q '^Files'; do
	_n=$((_n - 1))
	[ "$_n" -gt 0 ] || fail "no Files pane: $(screen)"
	step_keys Tab
done
# (past the Unstaged section's row)
keys j j Tab
wait_active "Unstaged: b.c"
# one -> ONE, both halves
keys : o1 Enter v j f
wait_screen "A fixup! commit for which commit?"
# the list starts on the commit shown (drv: add b), and Enter takes it
keys Enter
wait_screen 'fixup! commit for "drv: add b" made'
[ "$(git log -1 --format=%s)" = "fixup! drv: add b" ] ||
	fail "subject: $(git log -1 --format=%s)"
[ "$(git show HEAD:b.c)" = "$(printf 'ONE\ntwo')" ] ||
	fail "fixup content: $(git show HEAD:b.c)"
[ "$(git rev-parse HEAD~1)" = "$tip" ] || fail "series rewritten"
git diff | grep -q '^+three$' || fail "the other line not unstaged"

# F makes an amend! with the target's message in the editor
keys F
wait_screen "An amend! commit (a new message) for which commit?"
# from the top, fixup! drv: add b, drv: add c, drv: add b
keys k k k k j j Enter
wait_screen 'amend! commit for "drv: add b" made'
[ "$(git log -1 --format=%s)" = "amend! drv: add b" ] ||
	fail "amend! subject: $(git log -1 --format=%s)"
git log -1 --format=%B | grep -q '^drv: add b, better$' ||
	fail "amend! message: $(git log -1 --format=%B)"
git diff | grep -q '^+three$' || fail "F took unstaged lines"

# S asks, and n leaves all as it is. y squashes both into drv: add b, and
# the unstaged line stays.
tree=$(git rev-parse HEAD^{tree})
keys S
wait_screen "Squash 2 fixups into 1 commit, from"
keys n
wait_screen "not squashed"
[ "$(git log -1 --format=%s)" = "amend! drv: add b" ] || fail "n squashed"
keys S
wait_screen "Squash 2 fixups into 1 commit, from"
keys y
wait_screen "squashed 2 fixups into 1 commit"
[ "$(git rev-list --count HEAD)" = 3 ] || fail "count: $(git log --oneline)"
[ "$(git log -1 --format=%s HEAD~1)" = "drv: add b, better" ] ||
	fail "squashed subject: $(git log -1 --format=%s HEAD~1)"
[ "$(git show HEAD~1:b.c)" = "$(printf 'ONE\ntwo')" ] || fail "squashed content"
[ "$(git rev-parse HEAD^{tree})" = "$tree" ] || fail "tree changed"
git diff | grep -q '^+three$' || fail "squash lost the unstaged line"
tui_quit
