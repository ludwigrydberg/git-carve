#!/bin/sh
# SPDX-License-Identifier: MIT
# The first screen does not wait for the scan of untracked files. Tracked
# changes show at once and untracked files when the scan is done.
# carve.untrackedFiles = no leaves them out. A new file that is a copy of
# one in HEAD says so, untracked and staged. One that is not does not, and
# staging it does not make it look like one.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\n' >tracked.c
commit_all base
printf 'b\n' >tracked.c
printf 'n\n' >new.c
# the full scan waits while $tmp/hold exists, as a slow one would
# (git status -uall with an untracked cache, else git ls-files -o)
git_shim "case \" \$* \" in *\" --untracked-files=all \"*|*\" ls-files -z -o \"*)
	while [ -e '$tmp/hold' ]; do sleep 0.05; done ;;
esac"
# commits get a message
cat >"$tmp/editor" <<'X'
#!/bin/sh
printf 'add new\n' >"$1"
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

touch "$tmp/hold"
tui_start
wait_screen "M tracked.c"
screen | grep -q 'new.c' && fail "untracked file before the scan finished"
screen | grep -q 'scanning untracked' || fail "no scanning indicator"
rm "$tmp/hold"
# folded under Untracked, and l on its row, the last, unfolds it
wait_screen "Untracked (1) [+]"
wait_gone "scanning untracked"
keys G l
wait_screen "? new.c"
# staged and committed, the scan that started before the commit and ends
# after it must not bring it back as untracked
touch "$tmp/hold"
keys j Enter
wait_screen "A new.c"
keys c
wait_screen "committed"
rm "$tmp/hold"
wait_idle
screen | grep -q 'new\.c' && fail "committed file listed from the old scan"
[ "$(git log -1 --format=%s)" = "add new" ] || fail "commit"
# a new file staged and unstaged again is untracked at once, not only
# when the (slow) scan comes back
printf 'm\n' >more.c
git add more.c
keys C-l
wait_screen "A more.c"
# the scan that sees it staged is done before it is unstaged
wait_gone "scanning untracked"
keys g g Enter
wait_screen "unstaged more.c"
screen | grep -q '? more\.c' || fail "unstaged new file missing until the scan"
# the cursor stayed in the Staged section, and more.c is the last row
keys G Enter
wait_screen "A more.c"
screen | grep -q 'a copy of' && fail "more.c is no copy: $(screen)"
# a copy of new.c, in HEAD now
cp new.c copy.c
keys F5
wait_screen "? copy.c"
keys G
wait_screen "(new file, a copy of new.c)"
keys Enter
wait_screen "A copy.c"
# the cursor stays below, and copy.c is the first staged file
keys g g
wait_screen "(new file, a copy of new.c)"
tui_quit

git config carve.untrackedFiles no
printf 's\n' >stray.c
tui_start
wait_screen "M tracked.c"
wait_idle
screen | grep -q 'stray\.c\|Untracked' &&
	fail "untracked shown with untrackedFiles=no"
tui_quit

# the scan is git ls-files -o, which needs no refresh of the index. With
# git's untracked cache configured it is git status, set up to use the
# cache.
git config --unset carve.untrackedFiles
: >"$tmp/trace"
TUI_ENV="GIT_CARVE_TRACE=$tmp/trace" tui_start
wait_gone "scanning untracked"
tui_quit
grep -q '^bg .* ls-files -z -o --exclude-standard' "$tmp/trace" ||
	fail "no ls-files scan: $(grep '^bg' "$tmp/trace")"
git config core.untrackedCache true
: >"$tmp/trace"
TUI_ENV="GIT_CARVE_TRACE=$tmp/trace" tui_start
wait_gone "scanning untracked"
tui_quit
grep -q '^bg .*status.showUntrackedFiles=all.* status ' "$tmp/trace" ||
	fail "no cached status scan: $(grep '^bg' "$tmp/trace")"
