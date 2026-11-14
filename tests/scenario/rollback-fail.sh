#!/bin/sh
# SPDX-License-Identifier: MIT
# A rollback that fails. When a fixup-to, a move or a defer rolls back
# and git fails in the middle of it, the state stays, the error says that
# abort tries again, and abort then brings everything back as it was. A
# snapshot ref that cannot be removed after a fixup that worked only
# warns.
. "$(dirname "$0")/../lib.sh"
new_repo

# the first git whose arguments contain one of the |-separated parts of
# FAIL_ONCE fails, once per part
git_shim '
if [ -n "${FAIL_ONCE:-}" ]; then
	_args="$*" _ifs=$IFS
	IFS="|"
	for _p in $FAIL_ONCE; do
		case $_args in
		*"$_p"*)
			_m="$FAIL_DIR/$(printf "%s" "$_p" | cksum | cut -d" " -f1)"
			if [ ! -e "$_m" ]; then
				: >"$_m"
				echo "fatal: injected failure" >&2
				exit 1
			fi ;;
		esac
	done
	IFS=$_ifs
fi'
FAIL_DIR=$tmp/failed
export FAIL_DIR
mkdir "$FAIL_DIR"

# the repository as abort must bring it back
snapshot() {
	printf '%s %s %s\n' "$(git rev-parse HEAD)" "$(git write-tree)" \
		"$(worktree_state)"
}

# --- fixup-to whose result does not fit, and the rollback fails ---------
printf 'four\n' >four.c
printf 'x\n' >f.c
commit_all A
printf 'y\n' >f.c
commit_all B
printf 'four\nadded later\n' >four.c
commit_all C
tip=$(git rev-parse HEAD)
# the line C added goes, and an unrelated edit stays
printf 'four\n' >four.c
printf 'z\n' >>f.c
before=$(snapshot)

must_fail env FAIL_ONCE="reset -q --hard $tip" "$CARVE_CLI" fixup-to HEAD~1 \
	four.c o2
short=$(echo "$tip" | cut -c1-7)
grep -q "could not roll back to $short" "$tmp/must_fail.out" ||
	fail "fixup: error: $(cat "$tmp/must_fail.out")"
grep -q "'git-carve-cli abort' tries again" "$tmp/must_fail.out" ||
	fail "fixup: no abort: $(cat "$tmp/must_fail.out")"
carve status | grep -q 'nothing in progress' && fail "fixup: state removed"
carve abort >/dev/null || fail "fixup: abort"
[ "$(snapshot)" = "$before" ] || fail "fixup: abort did not bring it back"
carve status | grep -q 'nothing in progress' || fail "fixup: state left"

# --- a move that fails, and its rollback fails too -----------------------
git reset -q --hard "$tip"
printf 'one\n' >m.c
commit_all D
printf 'one\ntwo\n' >m.c
commit_all E
tip=$(git rev-parse HEAD)
printf 'z\n' >>f.c
before=$(snapshot)
rm -f "$FAIL_DIR"/*

must_fail env FAIL_ONCE="rebase -i|reset -q --hard $tip" "$CARVE_CLI" \
	move HEAD m.c n2 --into HEAD~1
grep -q "'git-carve-cli abort' tries again" "$tmp/must_fail.out" ||
	fail "move: no abort: $(cat "$tmp/must_fail.out")"
carve status | grep -q 'nothing in progress' && fail "move: state removed"
carve abort >/dev/null || fail "move: abort"
[ "$(snapshot)" = "$before" ] || fail "move: abort did not bring it back"
carve status | grep -q 'nothing in progress' || fail "move: state left"

# --- a defer that is refused half way, and its rollback fails ------------
git checkout -q -- .
printf 'one\ntwo\nthree\n' >m.c
commit_all F
tip=$(git rev-parse HEAD)
before=$(snapshot)
rm -f "$FAIL_DIR"/*

# the selection is all of F, nothing would be left of it
must_fail env FAIL_ONCE="reset -q --hard $tip" "$CARVE_CLI" \
	defer HEAD m.c n3
grep -q "'git-carve-cli abort' tries again" "$tmp/must_fail.out" ||
	fail "defer: no abort: $(cat "$tmp/must_fail.out")"
carve abort >/dev/null || fail "defer: abort"
[ "$(snapshot)" = "$before" ] || fail "defer: abort did not bring it back"
carve status | grep -q 'nothing in progress' || fail "defer: state left"

# --- the snapshot ref cannot be removed after a fixup that worked --------
# The files are back, the fixup is done, a warning names the ref, and the
# next fixup replaces it.
printf 'z\n' >>f.c
cp f.c "$tmp/f.c"
printf 'ONE\ntwo\nthree\n' >m.c
rm -f "$FAIL_DIR"/*
env FAIL_ONCE="update-ref -d refs/worktree/carve/worktree" "$CARVE_CLI" \
	fixup-to HEAD~1 m.c o1,n1 >"$tmp/out" 2>&1 ||
	fail "ref: $(cat "$tmp/out")"
grep -q "your files are back, but refs/worktree/carve/worktree" "$tmp/out" ||
	fail "ref: no warning: $(cat "$tmp/out")"
cmp -s f.c "$tmp/f.c" || fail "ref: your edit is gone"
git show HEAD~1:m.c | grep -q '^ONE$' || fail "ref: the fixup is not in"
carve status | grep -q 'nothing in progress' || fail "ref: state left"
printf 'ONE again\ntwo\nthree\n' >m.c
carve fixup-to HEAD~1 m.c o1,n1 >/dev/null || fail "ref: the next fixup"
cmp -s f.c "$tmp/f.c" || fail "ref: the next fixup lost your edit"
git rev-parse -q --verify refs/worktree/carve/worktree >/dev/null &&
	fail "ref: still there after the next fixup"

exit 0
