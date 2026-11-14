#!/bin/sh
# SPDX-License-Identifier: MIT
# git carve killed in the middle of an operation, at every git it starts:
# just before that git runs, or while it runs to its end on its own. A
# fresh start then finds the repository as it was or as done, or the
# operation in progress, and abort brings it back. No work is lost.
# CRASH_ITERS more runs pick an operation, a point and a signal at random
# (CRASH_SEED).
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

# At the CRASH_AT-th git, git carve gets CRASH_SIG. With CRASH_MODE=after
# that git still runs to its end, and its pid is in $CRASH_COUNT.pid.
git_shim '
if [ -n "${CRASH_AT:-}" ]; then
	n=$(($(cat "$CRASH_COUNT" 2>/dev/null || echo 0) + 1))
	echo "$n" >"$CRASH_COUNT"
	if [ "$n" = "$CRASH_AT" ]; then
		echo "$*" >"$CRASH_COUNT.git"
		kill -"${CRASH_SIG:-KILL}" "$PPID"
		[ "${CRASH_MODE:-before}" = before ] && exit 1
		echo $$ >"$CRASH_COUNT.pid"
	fi
fi'

# crash_once <n> <mode> <signal> <command...>: run the command killed at
# its n-th git, and wait for that git when it runs on. Sets _ran to the
# number of gits it started.
crash_once() {
	_at=$1 _mode=$2 _sig=$3
	shift 3
	rm -f "$_w/count" "$_w/count.git" "$_w/count.pid"
	# a subshell, since "VAR=x function" keeps VAR set afterwards in sh
	(CRASH_AT=$_at CRASH_MODE=$_mode CRASH_SIG=$_sig \
		CRASH_COUNT=$_w/count "$@") >"$_w/err" 2>&1
	_ran=$(cat "$_w/count" 2>/dev/null || echo 0)
	if [ -s "$_w/count.pid" ]; then
		_pid=$(cat "$_w/count.pid")
		# gone, or a zombie where nothing reaps orphans (a container)
		while kill -0 "$_pid" 2>/dev/null; do
			case $(ps -o stat= -p "$_pid" 2>/dev/null) in Z*) break ;; esac
			sleep 0.05
		done
	fi
	return 0
}

# head_tree <state>: HEAD and the work tree of a state, without the index
head_tree() {
	echo "$1" | sed '1s/ [^ ]* / /'
}

# recover <name>: the start after the crash. git carve must say what it
# finds. An operation in progress is aborted, and what is left is cleaned
# up as a fresh start offers it (gc --force). Then the repository is as
# before or as done, or as before with only the index changed (lines of
# the operation staged), and no work is lost.
recover() {
	_now=$(state)
	[ "$_now" = "$_before" ] || [ "$_now" = "$_done" ] && return 0
	carve status >"$_w/status" 2>&1 ||
		fail "$1: status after the crash failed: $(cat "$_w/status")"
	if ! grep -q 'nothing in progress' "$_w/status"; then
		carve abort >"$_w/abort" 2>&1 ||
			carve split --abort >>"$_w/abort" 2>&1
	fi
	carve gc --force >"$_w/gc" 2>&1 ||
		fail "$1: gc after the crash failed: $(cat "$_w/gc")"
	_now=$(state)
	[ "$_now" = "$_before" ] || [ "$_now" = "$_done" ] && return 0
	[ "$(head_tree "$_now")" = "$(head_tree "$_before")" ] &&
		[ -z "$(state_files)" ] && return 0
	fail "$1: neither as before nor done after abort and gc
git:    $(cat "$_w/count.git" 2>/dev/null)
before: $_before
done:   $_done
now:    $_now
status: $(cat "$_w/status")
abort:  $(cat "$_w/abort" 2>/dev/null)
gc:     $(cat "$_w/gc")
run:    $(cat "$_w/err")"
}

# crash_check <name> <prepare> <command...>: the command killed at each of its
# gits, before it runs and while it runs on, then a fresh start
crash_check() {
	_name=$1 _prep=$2
	shift 2
	_w=$(mktemp -d "$tmp/check.XXXXXX") || fail "mktemp"
	fresh "$_prep"
	"$@" >/dev/null 2>&1 || fail "$_name: fails without a crash"
	_done=$(state)
	for _mode in before after; do
		_n=1
		while :; do
			fresh "$_prep"
			_before=$(state)
			crash_once "$_n" "$_mode" KILL "$@"
			# the command ran out of git before the n-th
			[ "$_ran" -lt "$_n" ] && break
			recover "$_name, killed at git $_n ($_mode)"
			_n=$((_n + 1))
			[ $_n -le 80 ] || fail "$_name: more than 80 gits"
		done
	done
	echo "$_name: killed at $((_n - 1)) points, before and while git ran"
}

# the operations, as names, preparations and commands
op() {
	case $1 in
	0) _o="split HEAD" _p=":" _c="carve split" ;;
	1) _o="split an older commit" _p=":" _c="carve split HEAD~1" ;;
	2) _o="fixup-to" _p='edit "l 5" "l 5 fix"'
	   _c="carve fixup-to HEAD~2 a.c n5" ;;
	3) _o="defer" _p=":" _c="carve defer HEAD~1 a.c o20,n20 -m deferred" ;;
	4) _o="move" _p=":" _c="carve move HEAD~1 a.c o25,n25 --into HEAD~2" ;;
	5) _o="absorb" _p='edit "l 20 C2" "l 20 C2 fix"'
	   _c="carve absorb --apply" ;;
	6) _o="split abort" _p='carve split >/dev/null && carve stage a.c n30 &&
		carve commit -m part >/dev/null' _c="carve split --abort" ;;
	7) _o="split finish" _p='carve split >/dev/null && carve stage a.c n30 &&
		carve commit -m part >/dev/null && carve stage-file a.c &&
		carve commit -m rest >/dev/null' _c="carve split --finish" ;;
	8) _o="stage" _p='edit "l 5" "l 5 x"; edit "l 35" "l 35 x"'
	   _c="carve stage a.c n5" ;;
	9) _o="commit" _p='edit "l 5" "l 5 x"; carve stage-file a.c'
	   _c="carve commit -m new" ;;
	10) _o="undo" _p='edit "l 5" "l 5 x"; carve stage-file a.c &&
		carve commit -m new >/dev/null' _c="carve undo" ;;
	11) _o="squash" _p='edit "l 5" "l 5 x"; carve stage-file a.c &&
		git commit -q --fixup HEAD~2' _c="carve squash" ;;
	esac
}
OPS=12

# the checks run side by side, each its output in $tmp/out.<n>
_k=0
_pids=
crash_bg() {
	_k=$((_k + 1))
	(crash_check "$@") >"$tmp/out.$_k" 2>&1 &
	_pids="$_pids $!"
}

i=0
while [ $i -lt $OPS ]; do
	op $i
	# shellcheck disable=SC2086
	crash_bg "$_o" "$_p" $_c
	i=$((i + 1))
done

# random runs: an operation, a git, a mode and a signal, from the seed
random_runs() {
	_w=$(mktemp -d "$tmp/random.XXXXXX") || fail "mktemp"
	awk -v seed="${CRASH_SEED:-1}" -v n="${CRASH_ITERS:-24}" \
		-v ops=$OPS 'BEGIN {
		srand(seed)
		split("KILL TERM INT HUP", sig, " ")
		for (i = 0; i < n; i++)
			printf "%d %d %s %s\n", int(rand() * ops),
				1 + int(rand() * 20),
				(rand() < 0.5 ? "before" : "after"),
				sig[1 + int(rand() * 4)]
	}' >"$_w/plan"
	while read -r _op _at _mode _sig; do
		op "$_op"
		fresh "$_p"
		# shellcheck disable=SC2086
		$_c >/dev/null 2>&1 || fail "$_o: fails without a crash"
		_done=$(state)
		fresh "$_p"
		_before=$(state)
		# shellcheck disable=SC2086
		crash_once "$_at" "$_mode" "$_sig" $_c
		[ "$_ran" -lt "$_at" ] && continue
		recover "$_o, $_sig at git $_at ($_mode, seed ${CRASH_SEED:-1})"
	done <"$_w/plan"
	echo "random: ${CRASH_ITERS:-24} runs, seed ${CRASH_SEED:-1}"
}
_k=$((_k + 1))
(random_runs) >"$tmp/out.$_k" 2>&1 &
_pids="$_pids $!"

_bad=0
_k=0
for _p in $_pids; do
	_k=$((_k + 1))
	wait "$_p" || _bad=1
	cat "$tmp/out.$_k"
done
[ $_bad = 0 ] || fail "a crash left the repository broken"
exit 0
