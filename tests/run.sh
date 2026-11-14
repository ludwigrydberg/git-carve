#!/bin/sh
# SPDX-License-Identifier: MIT
# Run a suite of tests, as tests/run.sh <suite-name> <test>... A test is an
# executable or a *.sh script, and exit status 77 means SKIP. Tests run in
# parallel, JOBS at a time (by default the number of CPUs), and every test
# has its own repository, HOME and tmux server.
if [ "$1" = --one ]; then
	# --one <logdir> <suite> <test>: run one test, record how it went
	logdir=$2 suite=$3 t=$4
	name=$(basename "$t")
	log=$logdir/$name.log
	t0=$(date +%s)
	case $t in
	*.sh) sh "$t" >"$log" 2>&1 ;;
	*) "$t" >"$log" 2>&1 ;;
	esac
	rc=$?
	secs=$(($(date +%s) - t0))
	echo $rc >"$logdir/$name.rc"
	case $rc in
	0) printf '  PASS  %s/%s (%ds)\n' "$suite" "$name" $secs ;;
	77) printf '  SKIP  %s/%s: %s\n' "$suite" "$name" \
		"$(tail -n 1 "$log")" ;;
	*) printf '  FAIL  %s/%s (exit %d, %ds)\n' "$suite" "$name" $rc \
		$secs ;;
	esac
	exit 0
fi
suite=$1
shift
top=$(cd "$(dirname "$0")/.." && pwd)
CARVE=${CARVE:-$top/git-carve}
CARVE_CLI=${CARVE_CLI:-$top/git-carve-cli}
export CARVE CARVE_CLI
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
logdir=$(mktemp -d "${TMPDIR:-/tmp}/carve-log.XXXXXX")
for t in "$@"; do
	[ -e "$t" ] && printf '%s\n' "$t"
done | xargs -n 1 -P "$jobs" sh "$0" --one "$logdir" "$suite"
pass=0 fail=0 skip=0 failed=
for t in "$@"; do
	[ -e "$t" ] || continue
	name=$(basename "$t")
	rc=$(cat "$logdir/$name.rc" 2>/dev/null || echo 1)
	if [ "$rc" -eq 0 ]; then
		pass=$((pass + 1))
	elif [ "$rc" -eq 77 ]; then
		skip=$((skip + 1))
	else
		fail=$((fail + 1))
		failed="$failed $suite/$name"
		printf '  FAIL  %s/%s (exit %d)\n' "$suite" "$name" "$rc"
		sed 's/^/        | /' "$logdir/$name.log" | tail -n 40
	fi
done
rm -rf "$logdir"
printf '%s: %d passed, %d failed, %d skipped\n' "$suite" $pass $fail $skip
[ $fail -eq 0 ] || { echo "FAILED:$failed"; exit 1; }
