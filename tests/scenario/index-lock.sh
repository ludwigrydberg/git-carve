#!/bin/sh
# SPDX-License-Identifier: MIT
# The index lock (found on the kernel tree, where the TUI's background
# git status held index.lock long enough for u to fail with a bogus "the
# index has conflicts"): the background scan runs without optional
# locks, the foreground status may write the refreshed index, and undo
# waits a little for a lock and otherwise says what is wrong.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'a\n' >f.c
commit_all base
printf 'b\n' >f.c

# the scan of untracked files takes no optional lock, and the tracked
# status refreshes the index for the gits after it
git_shim 'case " $* " in *" status "*) echo "$*" >>"'"$tmp"'/status";; esac'
PATH="$tmp/bin:$PATH" carve status >/dev/null
PATH="$tmp/bin:$PATH" carve renames >/dev/null
grep -- '--untracked-files=all' "$tmp/status" | grep -qv -- '--no-optional-locks' &&
	fail "the scan with the lock: $(cat "$tmp/status")"
rm "$tmp/bin/git"


carve stage-file f.c
# undo waits for a lock that goes away
: >.git/index.lock
(sleep 0.3; rm -f .git/index.lock) &
carve undo >/dev/null || fail "undo did not wait for the lock"
wait
assert_index_text f.c 'a\n'

# a lock that stays gives git's reason, not a made-up conflict
carve stage-file f.c
: >.git/index.lock
must_fail carve undo
grep -q 'index.lock' "$tmp/must_fail.out" ||
	fail "message: $(cat "$tmp/must_fail.out")"
grep -q 'conflicts' "$tmp/must_fail.out" && fail "bogus conflict message"
rm -f .git/index.lock
carve undo >/dev/null || fail "undo after the lock went"

# No diff takes the lock. One may be stopped at any moment, by a key in the
# TUI or too many lines, and a git killed while it holds index.lock leaves
# it behind. absorb diffs every file as the TUI's search does.
new_repo r2
printf 'a\nb\n' >g.c
commit_all base
git branch -q up
git branch -q --set-upstream-to=up
printf 'a\nb P1\n' >g.c
commit_all P1
printf 'a\nb P1 fixed\n' >g.c
git_shim 'case " $* " in *" diff "*) echo "$*" >>"'"$tmp"'/diffs";; esac'
PATH="$tmp/bin:$PATH" carve absorb >/dev/null || fail "absorb plan"
PATH="$tmp/bin:$PATH" carve diff g.c >/dev/null
rm "$tmp/bin/git"
grep -q -- ' -U0' "$tmp/diffs" || fail "no diff of every file: $(cat "$tmp/diffs")"
grep -v -- '--no-optional-locks' "$tmp/diffs" | grep -v -- ' diff-tree ' |
	grep -q . && fail "a diff with the lock: $(grep -v -- '--no-optional-locks' "$tmp/diffs")"
exit 0
