#!/bin/sh
# SPDX-License-Identifier: MIT
# A git that fails in the middle of an operation. Each git process an
# operation starts fails in turn. Afterwards the repository is as it was
# (HEAD, index, work tree, no state left), or the operation is in progress
# and abort brings it back exactly.
. "$(dirname "$0")/../lib.sh"
new_repo tmpl

i=1
while [ $i -le 40 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
git branch -q up
git branch -q --set-upstream-to=up
edit "l 10" "l 10 C1"
commit_all C1
edit "l 20" "l 20 C2"
edit "l 25" "l 25 C2"
commit_all C2
edit "l 30" "l 30 C3"
commit_all C3
tmpl=$(pwd)

# the n-th git of a command run with FAIL_AT=n fails
git_shim '
if [ -n "${FAIL_AT:-}" ]; then
	n=$(($(cat "$FAIL_COUNT" 2>/dev/null || echo 0) + 1))
	echo "$n" >"$FAIL_COUNT"
	if [ "$n" = "$FAIL_AT" ]; then
		echo "$*" >"$FAIL_COUNT.git"
		echo "fatal: injected failure" >&2
		exit 1
	fi
fi'

# the state without the index, which a failing unstage leaves as it was
not_index() {
	sed '1s/ [^ ]* / /'
}

# check <name> <prepare> <command...>: every git of the command failing
# in turn leaves the repository as it was, or abort brings it back (or,
# when the command is an abort, finishes it), or the operation is done
# (perhaps after the recovery its error names), or it did nothing and
# said what failed (absorb leaves a hunk it cannot blame where it is).
# When the failing git is the unstaging of what the command staged, the
# index may stay staged. Each check works in its own directory $_w.
check() {
	_name=$1 _prep=$2
	shift 2
	_w=$(mktemp -d "$tmp/check.XXXXXX") || fail "mktemp"
	fresh "$_prep"
	"$@" >/dev/null 2>&1 || fail "$_name: fails without a failing git"
	_done=$(state)
	_n=1
	while :; do
		fresh "$_prep"
		_before=$(state)
		rm -f "$_w/count" "$_w/count.git"
		FAIL_AT=$_n FAIL_COUNT=$_w/count "$@" >"$_w/err" 2>&1
		_rc=$?
		_ran=$(cat "$_w/count" 2>/dev/null || echo 0)
		# the command ran out of git before the n-th, all points
		# are done
		[ "$_ran" -lt "$_n" ] && break
		_now=$(state)
		if [ $_rc = 0 ]; then
			[ "$_now" = "$_done" ] ||
				{ [ "$_now" = "$_before" ] &&
				  grep -q failed "$_w/err"; } ||
				fail "$_name, git $_n failing: succeeded, but not done
$(cat "$_w/err")"
		elif grep -q 'restore it with git restore' "$_w/err"; then
			git restore --source=refs/worktree/carve/worktree \
				--worktree -- . || fail "$_name: recovery failed"
			[ "$(state)" = "$_done" ] ||
				fail "$_name, git $_n failing: not done after the recovery
$(cat "$_w/err")"
		elif [ "$(cat "$_w/count.git")" = "reset -q" ] &&
			[ "$(echo "$_now" | not_index)" = \
			  "$(echo "$_before" | not_index)" ]; then
			:
		elif [ "$_now" != "$_before" ]; then
			carve abort >/dev/null 2>&1 ||
				carve split --abort >/dev/null 2>&1
			_now=$(state)
			[ "$_now" = "$_before" ] || [ "$_now" = "$_done" ] ||
				fail "$_name, git $_n failing: not as before, not after abort either
before: $_before
after:  $(state)
$(cat "$_w/err")"
		fi
		_n=$((_n + 1))
		[ $_n -le 80 ] || fail "$_name: more than 80 git processes"
	done
	echo "$_name: $((_n - 1)) points of failure"
}

# the checks run side by side, each its output in $tmp/out.<n>
_k=0
_pids=
bg() {
	_k=$((_k + 1))
	(check "$@") >"$tmp/out.$_k" 2>&1 &
	_pids="$_pids $!"
}

bg "split HEAD" ":" carve split
bg "split an older commit" ":" carve split HEAD~1
bg "fixup-to" 'edit "l 5" "l 5 fix"' carve fixup-to HEAD~2 a.c n5
bg "defer" ":" carve defer HEAD~1 a.c o20,n20 -m deferred
bg "move" ":" carve move HEAD~1 a.c o25,n25 --into HEAD~2
bg "absorb" 'edit "l 20 C2" "l 20 C2 fix"' carve absorb --apply
bg "split abort" 'carve split >/dev/null && carve stage a.c n30 &&
	carve commit -m part >/dev/null' carve split --abort

_bad=0
_k=0
for _p in $_pids; do
	_k=$((_k + 1))
	wait "$_p" || _bad=1
	cat "$tmp/out.$_k"
done
[ $_bad = 0 ] || fail "a git failing left the repository broken"
exit 0
