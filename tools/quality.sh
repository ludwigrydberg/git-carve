#!/bin/sh
# SPDX-License-Identifier: MIT
# Code quality checks, run by make lint, analyze, coverage and fuzz-long,
# where make check runs lint and analyze. Each part needs its tool, and one
# that is missing prints SKIP while the others still run. POSIX sh.
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
cd "$top" || exit 1
CC=${CC:-cc}
SOURCES=$(ls src/*.c src/*.h src/ui/*.c src/ui/*.h src/cli/*.c src/cli/*.h \
	tests/unit/*.c tests/unit/*.h tests/fuzz/*.c)
SHELL_FILES=$(ls tests/*.sh tests/*/*.sh tools/*.sh)
# tools installed with pip install --user (lizard, clang-tidy)
user_bin=$(python3 -m site --user-base 2>/dev/null)/bin
PATH=$PATH:$user_bin
export PATH
# the limit of a function's cyclomatic complexity (lizard's CCN)
CCN_MAX=40

have() {
	command -v "$1" >/dev/null 2>&1
}

skip_part() {
	echo "SKIP $1: $2"
}

ncurses_cflags() {
	pkg-config --cflags ncursesw 2>/dev/null ||
		pkg-config --cflags ncurses 2>/dev/null
}

# The flags the sources build with, for the tools that parse them.
cflags() {
	f="-std=gnu11 $(ncurses_cflags) -DCARVE_VERSION=\"x\" -Isrc"
	case $(uname) in Darwin) f="$f -D_DARWIN_C_SOURCE" ;; esac
	if have xcrun; then
		f="$f -isysroot $(xcrun --show-sdk-path)"
	fi
	echo "$f"
}

lint_format() {
	want=$(sed -n 's/.*formatted with clang-format \([0-9]*\).*/\1/p' \
		.clang-format)
	have clang-format || {
		skip_part format "clang-format is not installed"
		return 0
	}
	ver=$(clang-format --version | sed -n 's/.*version \([0-9]*\).*/\1/p')
	if [ "$ver" != "$want" ]; then
		skip_part format "clang-format $ver formats differently than $want"
		return 0
	fi
	rc=0
	for f in $SOURCES; do
		if ! clang-format --dry-run --Werror "$f" >/dev/null 2>&1; then
			echo "lint: $f is not formatted (make format)"
			rc=1
		fi
	done
	return $rc
}

lint_shell() {
	have shellcheck || {
		skip_part shellcheck "shellcheck is not installed"
		return 0
	}
	# SC1091, the libraries are sourced by path. SC2154, their
	# variables. SC2034, variables the libraries read. SC2015, "a && b
	# || c" on purpose. SC2016, single quotes keep $ for the shell run
	# later. SC1083, a literal { in tmux key names.
	# shellcheck disable=SC2086
	shellcheck -x -P tests -f gcc \
		-e SC1091,SC2154,SC2034,SC2015,SC2016,SC1083 $SHELL_FILES
}

lint_spelling() {
	have codespell || {
		skip_part spelling "codespell is not installed"
		return 0
	}
	codespell src tests docs tools README.md CONTRIBUTING.md Makefile
}

# Functions above CCN_MAX, except those tools/complexity.allow knows. A
# known one under the limit again must leave the list.
lint_complexity() {
	have lizard || {
		skip_part complexity "lizard is not installed (pip install lizard)"
		return 0
	}
	lizard -C "$CCN_MAX" -L 100000 -a 100 -w src 2>/dev/null |
		sed -n 's/.*: warning: \([A-Za-z_0-9]*\) has .* \([0-9]*\) CCN.*/\1 \2/p' \
		>"$tmp/ccn"
	rc=0
	while read -r name ccn; do
		grep -qx "$name" tools/complexity.allow || {
			echo "lint: $name has complexity $ccn (at most $CCN_MAX)"
			rc=1
		}
	done <"$tmp/ccn"
	grep -v '^#' tools/complexity.allow >"$tmp/allow"
	while read -r name; do
		grep -q "^$name " "$tmp/ccn" || {
			echo "lint: $name is under the limit now: drop it from" \
				"tools/complexity.allow"
			echo bad >"$tmp/stale"
		}
	done <"$tmp/allow"
	[ -f "$tmp/stale" ] && rc=1
	return $rc
}

lint() {
	tmp=$(mktemp -d) || exit 1
	bad=0
	lint_format || bad=1
	# kernel comments, reverse xmas tree
	# shellcheck disable=SC2086
	perl tools/style.pl $SOURCES || bad=1
	lint_shell || bad=1
	lint_spelling || bad=1
	lint_complexity || bad=1
	rm -rf "$tmp"
	[ $bad = 0 ] && echo "lint: ok"
	return $bad
}

# clang-tidy with .clang-tidy (its clang-analyzer checks are clang's
# static analyzer), else clang --analyze alone.
analyze_clang() {
	fl=$(cflags)
	if have clang-tidy; then
		rc=0
		for f in src/*.c src/ui/*.c src/cli/*.c; do
			# shellcheck disable=SC2086
			clang-tidy --quiet "$f" -- $fl 2>/dev/null |
				grep -E ': (error|warning):' && rc=1
		done
		return $rc
	fi
	have clang || {
		skip_part clang "neither clang-tidy nor clang is installed"
		return 0
	}
	skip_part clang-tidy "not installed (pip install clang-tidy); clang --analyze only"
	out=$(mktemp) || exit 1
	for f in src/*.c src/ui/*.c src/cli/*.c; do
		# shellcheck disable=SC2086
		clang --analyze $fl -Xanalyzer \
			-analyzer-disable-checker=security.insecureAPI.DeprecatedOrUnsafeBufferHandling \
			-o /dev/null "$f" 2>&1 | grep 'warning:' >>"$out"
	done
	rc=0
	[ -s "$out" ] && { sed 's/^/analyze: /' "$out"; rc=1; }
	rm -f "$out"
	return $rc
}

analyze_gcc() {
	$CC -fanalyzer -x c -c /dev/null -o /dev/null 2>/dev/null || return 0
	nc=$(ncurses_cflags)
	out=$(mktemp) || exit 1
	for f in src/*.c src/ui/*.c src/cli/*.c; do
		# shellcheck disable=SC2086
		LC_ALL=C $CC -std=gnu11 -fanalyzer -O1 -Wall -Wextra \
			-Wno-unused-parameter $nc -DCARVE_VERSION='"x"' \
			-Isrc -c "$f" -o /dev/null 2>&1 |
			grep 'warning:' >>"$out"
	done
	# drop the checked findings of tools/analyzer.supp (LC_ALL=C above
	# quotes names as they are written there)
	awk 'BEGIN { n = 0 }
	NR == FNR {
		if ($0 ~ /^#/ || $0 == "") next
		file[n] = substr($0, 1, index($0, ": ") - 1)
		text[n] = substr($0, index($0, ": ") + 2)
		n++
		next
	}
	{
		for (i = 0; i < n; i++)
			if (index($0, file[i] ":") == 1 && index($0, text[i]))
				next
		print
	}' tools/analyzer.supp "$out" >"$out.left"
	rc=0
	[ -s "$out.left" ] && { sed 's/^/analyze: /' "$out.left"; rc=1; }
	rm -f "$out" "$out.left"
	return $rc
}

analyze() {
	bad=0
	analyze_clang || bad=1
	# varFuncNullUB: the git_* argument lists end in NULL, which the
	# sentinel attribute in git.h checks at compile time
	if have cppcheck; then
		cppcheck --quiet --error-exitcode=1 --inline-suppr \
			--enable=warning,portability,performance \
			--suppress=varFuncNullUB \
			--std=c11 -Isrc src src/ui || bad=1
	fi
	analyze_gcc || bad=1
	[ $bad = 0 ] && echo "analyze: ok"
	return $bad
}

# gcov, or llvm-cov's gcov for a clang build.
gcov_cmd() {
	case $($CC --version 2>/dev/null) in
	*clang*)
		if have llvm-cov; then
			echo "llvm-cov gcov"
		elif have xcrun && xcrun --find llvm-cov >/dev/null 2>&1; then
			echo "xcrun llvm-cov gcov"
		fi
		;;
	*) have gcov && echo gcov ;;
	esac
}

# Line coverage of all of make check, built apart in coverage/. Under
# COVERAGE_MIN percent, if that is set, it fails.
coverage() {
	gc=$(gcov_cmd)
	[ -n "$gc" ] || { skip_part coverage "no gcov or llvm-cov"; return 0; }
	dir=$top/coverage
	rm -rf "$dir.build" "$dir"
	mkdir -p "$dir.build" "$dir" || exit 1
	git ls-files -z | xargs -0 tar cf - | (cd "$dir.build" && tar xf -)
	# every stage, whatever an earlier one did
	for stage in unit scenario fuzz ui perf; do
		(cd "$dir.build" && make -s COVERAGE=1 CFLAGS='-O0 -g' \
			"check-$stage") ||
			echo "coverage: the $stage tests failed; counted anyway"
	done
	# shellcheck disable=SC2086
	(cd "$dir.build" &&
	 for f in src/*.c src/ui/*.c src/cli/*.c; do
		$gc -o "$(dirname "$f")" "$f" 2>/dev/null |
			awk -v f="$f" '/^Lines executed:/ {
				sub(/^Lines executed:/, ""); print f ": " $0; exit }'
	 done) >"$dir/summary.txt"
	mv "$dir.build"/*.gcov "$dir/" 2>/dev/null
	rm -rf "$dir.build"
	cat "$dir/summary.txt"
	awk '{ p = $2; sub(/%/, "", p); n = $4; covered += p * n / 100
		total += n }
	END { if (total) printf "coverage: %.1f%% of %d lines\n",
		100 * covered / total, total }' "$dir/summary.txt" |
		tee "$dir/total.txt"
	awk -v min="${COVERAGE_MIN:-0}" '{ p = $2; sub(/%/, "", p)
		if (p + 0 < min + 0) { print "coverage: under " min "%"; exit 1 } }' \
		"$dir/total.txt"
}

fuzz_long() {
	make -s git-carve tests/fuzz/fuzz || exit 1
	iters=${FUZZ_ITERS:-20000}
	for seed in ${FUZZ_SEEDS:-1 2 3 4 5}; do
		echo "fuzz-long: seed $seed, $iters iterations"
		FUZZ_SEED=$seed FUZZ_ITERS=$iters sh tests/run.sh fuzz \
			tests/fuzz/*.sh || exit 1
	done
}

case ${1:-} in
lint) lint ;;
analyze) analyze ;;
coverage) coverage ;;
fuzz-long) fuzz_long ;;
*) echo "usage: tools/quality.sh lint|analyze|coverage|fuzz-long" >&2
   exit 2 ;;
esac
