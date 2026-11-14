#!/bin/sh
# SPDX-License-Identifier: MIT
# Split HEAD end to end in the TUI with s, stage, c twice, and s to finish.
# Then X aborts a second split, and q asks for confirmation. A third,
# finished, says how many commits it made. X asks again before it discards
# changes in the way.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'base\n' >README
commit_all base
printf 'a\n' >a.c
printf 'b\n' >b.c
git add a.c b.c
git commit -q -m "ui: add a and b" || fail commit
orig=$(git rev-parse HEAD)
orig_tree=$(tree_of HEAD)

# the first commit keeps the original message, and later ones get a subject
cat >"$tmp/editor" <<'X'
#!/bin/sh
if [ -z "$(head -n 1 "$1")" ]; then
	{ echo "ui: second part"; cat "$1"; } >"$1.new" && mv "$1.new" "$1"
fi
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

tui_start
wait_screen "Nothing to stage"
wait_screen "to split HEAD"
keys s
wait_screen "Split HEAD"
keys y
wait_screen "SPLIT  ZZ: finish  X: abort"
wait_screen "${orig%"${orig#???????}"} \"ui: add a and b\""
wait_screen "0 commits made, remaining diff +2/-0"
wait_screen "Unstaged (2)"

keys Enter
wait_screen "Staged (1)"
keys c
wait_screen "1 commit made"
[ "$(git log -1 --format=%s)" = "ui: add a and b" ] || fail "first message"

# b.c is untracked, listed once the background scan is back
wait_screen '? b.c'
keys Enter
wait_screen "Staged (1)"
keys c
wait_screen "2 commits made, nothing left"
[ "$(git log -1 --format=%s)" = "ui: second part" ] || fail "second message"
wait_screen "Split complete"

# s only starts splits, and ZZ finishes
keys s
wait_screen "a split is in progress"
wait_screen "ZZ finishes it, X gives it up"
close_box
keys Z Z
wait_screen "split finished: 2 commits"
wait_gone "SPLIT "
[ "$(tree_of HEAD)" = "$orig_tree" ] || fail "I6: tree"
[ "$(git rev-list --count HEAD)" = 3 ] || fail "count"
git rev-parse -q --verify refs/worktree/carve/orig >/dev/null && fail "ref left"

# a second split, aborted with X
new=$(git rev-parse HEAD)
keys s
wait_screen "Split HEAD"
keys y
wait_screen "SPLIT "
# q asks first, and answering n keeps the TUI running
keys q
wait_screen "quit anyway"
keys n
wait_screen "SPLIT "
keys X
wait_screen "Abort the split"
keys y
wait_screen "split aborted"
[ "$(git rev-parse HEAD)" = "$new" ] || fail "abort HEAD"
[ -z "$(short_status)" ] || fail "abort status: $(short_status)"

# a split into one commit says that
keys s
wait_screen "Split HEAD"
keys y
wait_screen "SPLIT "
wait_screen '? b.c'
keys Enter
wait_screen "Staged (1)"
keys c
wait_screen "1 commit made, nothing left"
keys Z Z
wait_screen "split finished: 1 commit"

# X with changes of yours in the way asks again before throwing them away,
# and no keeps the split
keys s
wait_screen "Split HEAD"
keys y
wait_screen "SPLIT "
echo mine >>README
keys C-l X
wait_screen "Abort the split"
keys y
wait_screen "discard them? [y/N]"
keys n
wait_screen "SPLIT "
grep -q '^mine$' README || fail "your change lost on no"
keys X
wait_screen "Abort the split"
keys y
wait_screen "discard them? [y/N]"
keys y
wait_screen "split aborted"
grep -q '^mine$' README && fail "the change in the way kept"
tui_quit
