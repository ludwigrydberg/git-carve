#!/bin/sh
# SPDX-License-Identifier: MIT
# The code style, kernel comments and reverse xmas tree declarations
# (tools/style.pl), and clang-format of the version the tree is formatted
# with, which .clang-format names. Another version formats some lines
# differently and is then left out.
. "$(dirname "$0")/../lib.sh"
new_repo
top=$(cd "$TESTS_DIR/.." && pwd)
cd "$top" || fail "cd $top"
files=$(ls src/*.c src/*.h src/ui/*.c src/ui/*.h src/cli/*.c src/cli/*.h \
	tests/unit/*.c tests/unit/*.h tests/fuzz/*.c)

# one argument per file
# shellcheck disable=SC2086
perl tools/style.pl $files >"$tmp/style" ||
	fail "style (CONTRIBUTING.md):
$(cat "$tmp/style")"

want=$(sed -n 's/.*formatted with clang-format \([0-9]*\).*/\1/p' .clang-format)
[ -n "$want" ] || fail ".clang-format names no clang-format version"
if command -v clang-format >/dev/null 2>&1; then
	have=$(clang-format --version | sed -n 's/.*version \([0-9]*\).*/\1/p')
	if [ "$have" = "$want" ]; then
		for f in $files; do
			clang-format --dry-run --Werror "$f" >/dev/null 2>&1 ||
				echo "$f" >>"$tmp/unformatted"
		done
		[ -s "$tmp/unformatted" ] &&
			fail "not formatted (make format): $(cat "$tmp/unformatted")"
	else
		echo "style: clang-format $have, not $want: formatting not checked"
	fi
fi
exit 0
