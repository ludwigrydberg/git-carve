#!/bin/sh
# SPDX-License-Identifier: MIT
# The layout as users start with it, one Diff pane with the diff of the
# cursor's section, and the Message pane on top of it. e in the Message
# pane writes the message in the editor, from the prefix and sign-off, and
# c commits with it without the editor and empties it. gc hides the pane.
# When narrow, the Message pane is a pane of its own. carve.layout = two
# shows both diffs. A commit shown in the Series pane shows its message.
CARVE_TEST_LAYOUT=default
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=1; while [ $i -le 30 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
edit "l 5" "l 5 staged"
git add a.c
edit "l 25" "l 25 unstaged"

# the editor writes what e gets, or nothing, and a mark when it runs
cat >"$tmp/editor" <<X
#!/bin/sh
echo run >>"$tmp/editor.runs"
[ -f "$tmp/msg" ] && cp "$tmp/msg" "\$1"
exit 0
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

tui_start
wait_screen "M a.c"
wait_screen "Message"
wait_screen "No message yet."
# the first file is the staged a.c, with its staged diff
wait_screen "Diff (staged): a.c"
wait_screen "+l 5 staged"
screen | grep -q "Diff (unstaged)" && fail "two diffs shown"
# the unstaged a.c (past the Unstaged row): its unstaged diff
keys j j
wait_screen "Diff (unstaged): a.c"
wait_screen "+l 25 unstaged"
# Tab goes through Files, Message, Diff, then the Series pane and Files
# again
keys Tab
wait_active "Message"
keys Tab
wait_active "Diff (unstaged): a.c"
keys Tab
wait_active "Series"
keys Tab
wait_active "Files"

# e in the Message pane, left as it was, gives no message
keys Tab
wait_active "Message"
keys e
wait_screen "no message written"
[ -f .git/CARVE_MESSAGE ] && fail "an untouched template kept"
# e writes it, and the pane shows it
printf 'drv: stage l 5\n\nWhy it changes.\n' >"$tmp/msg"
keys e
wait_screen "message kept"
wait_screen "drv: stage l 5"
[ -f .git/CARVE_MESSAGE ] || fail "message not kept"
# c commits with it, without the editor
rm -f "$tmp/editor.runs"
keys c
wait_screen "committed"
[ -f "$tmp/editor.runs" ] && fail "c opened the editor"
[ "$(git log -1 --format=%s)" = "drv: stage l 5" ] ||
	fail "subject: $(git log -1 --format=%s)"
[ "$(git log -1 --format=%b)" = "Why it changes." ] || fail "body"
[ -f .git/CARVE_MESSAGE ] && fail "message not emptied"
wait_screen "No message yet."
# a commit shown shows its message, J for HEAD, then the one below
keys J
wait_screen "Message of HEAD"
wait_screen "Why it changes."
keys J
wait_screen "Message of ~1"
wait_gone "Why it changes."
keys K K
wait_screen "No message yet."
# gc hides the pane and shows it again
keys g c
wait_gone "No message yet."
wait_screen "message pane hidden"
keys g c
wait_screen "No message yet."
tui_quit

# when narrow, the Message pane is a pane of its own, and Tab reaches it
TUI_W=80 TUI_H=30 tui_start
wait_screen "No message yet."
keys Tab
wait_active "Message"
tui_quit

# carve.layout = two shows both diffs
git config carve.layout two
tui_start
wait_screen "Unstaged: a.c"
wait_screen "Staged: a.c"
tui_quit
