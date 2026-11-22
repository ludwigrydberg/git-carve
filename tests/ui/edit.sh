#!/bin/sh
# SPDX-License-Identifier: MIT
# e opens the file in the editor at the cursor's line, from the Unstaged
# and the Staged pane, and leaves the index alone. A deleted file is
# refused. In the Series pane e rewrites a commit's message. After a
# prefix key the status line lists what can follow it.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

cat >drv.c <<'X'
int drv_probe(struct device *dev)
{
	return foo(dev);
}
X
commit_all base
cat >drv.c <<'X'
int drv_probe(struct device *dev)
{
	return bar(dev, FLAG_X);
}
X

# a fake editor records the arguments it was given
cat >"$tmp/editor" <<X
#!/bin/sh
for a in "\$@"; do printf '%s\n' "\$a"; done >"$tmp/args"
exit 0
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

wait_args() {
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ] && [ ! -s "$tmp/args" ]; do
		sleep 0.1
		_n=$((_n - 1))
	done
	grep -q '^+3$' "$tmp/args" || fail "$1 args: $(cat "$tmp/args")"
	grep -q 'drv.c$' "$tmp/args" || fail "$1 file: $(cat "$tmp/args")"
}

tui_start
keys Tab
wait_active "Unstaged: drv.c"
keys : 3 Enter e
wait_args e
wait_active "Unstaged: drv.c"
[ -z "$(git diff --cached --name-only)" ] || fail "e staged something"
tui_quit

# from the Staged pane too
git add drv.c
rm -f "$tmp/args"
tui_start
keys Tab Tab
wait_active "Staged: drv.c"
keys : 3 Enter e
wait_args e
tui_quit

# a file deleted in the work tree is refused, not opened (saving would
# bring it back)
git reset -q
git rm -q --cached drv.c >/dev/null
git commit -q -m gone
git checkout -q HEAD~1 -- drv.c
git commit -q -m back
rm drv.c
rm -f "$tmp/args"
tui_start
keys Tab
wait_screen "Unstaged: drv.c"
keys e
wait_screen "drv.c is not in the work tree"
[ ! -e drv.c ] || fail "e brought drv.c back"
[ ! -s "$tmp/args" ] || fail "editor started: $(cat "$tmp/args")"

# in the Series pane e gives the commit under the cursor a new message,
# as r does, and on the work tree row it writes the next commit's
keys BTab BTab
wait_active "Series"
keys j e
n=0
while [ "$n" -lt "$WAIT_TRIES" ] && [ ! -s "$tmp/args" ]; do
	sleep 0.1
	n=$((n + 1))
done
grep -q 'COMMIT_EDITMSG\|carve' "$tmp/args" ||
	fail "series e: $(cat "$tmp/args")"

# after a prefix key the status line lists what can follow it
keys g
wait_screen "g:  "
wait_screen "c message-pane"
keys Escape
wait_gone "c message-pane"
tui_quit
