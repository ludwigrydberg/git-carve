#!/bin/sh
# SPDX-License-Identifier: MIT
# c commits what is staged through git commit (editor), A amends.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\n' >f.c
commit_all base
printf 'a\nb\n' >f.c

cat >"$tmp/editor" <<X
#!/bin/sh
printf '%s\n' "\${CARVE_TEST_MSG:-ui: add b}" >"\$1"
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

tui_start
keys c
wait_screen "nothing staged to commit"
close_box
keys Enter
wait_screen "Staged (1)"
keys c
wait_screen "committed"
[ "$(git log -1 --format=%s)" = "ui: add b" ] || fail "commit subject"
wait_screen "ui: add b"
wait_screen "Nothing to stage"
tui_quit

# amend with a different message
export CARVE_TEST_MSG="ui: add b (amended)"
tui_start
keys A
wait_screen "amended HEAD"
[ "$(git log -1 --format=%s)" = "ui: add b (amended)" ] || fail "amend"
[ "$(git rev-list --count HEAD)" = 2 ] || fail "amend made a new commit"
tui_quit

# Ctrl-S turns sign-off on, shown in the header, and the next commit is
# signed off
cat >"$tmp/editor2" <<X
#!/bin/sh
{ echo "ui: signed"; cat "\$1"; } >"\$1.new" && mv "\$1.new" "\$1"
X
chmod +x "$tmp/editor2"
export GIT_EDITOR="$tmp/editor2"
printf 'a\nb\nc\n' >f.c
unset CARVE_TEST_MSG
tui_start
keys C-s
wait_screen "sign-off on"
screen | head -1 | grep -q 'sign-off' || fail "no sign-off in the header"
keys Enter c
wait_screen "committed"
git log -1 --format=%B | grep -q '^Signed-off-by: T <t@t>$' ||
	fail "not signed off: $(git log -1 --format=%B)"
keys C-s
wait_screen "sign-off off"
screen | head -1 | grep -q 'sign-off' && fail "header after off"
tui_quit
