#!/bin/sh
# SPDX-License-Identifier: MIT
# Shell scripts use only what stock macOS and Debian have. GNU-only tools
# and flags such as sed -i and printf "\x.." fail on macOS, and this
# catches them on Linux too.
. "$(dirname "$0")/../lib.sh"

top=$(cd "$TESTS_DIR/.." && pwd)
bad=
for f in "$top"/tests/*.sh "$top"/tests/*/*.sh "$top"/tools/*.sh; do
	[ -f "$f" ] || continue
	case $f in */portability.sh) continue ;; esac
	# code lines only, not comments
	hits=$(grep -n -E 'sed -i|readlink -f|sha256sum|md5sum|grep -P|printf [^|]*\\x[0-9a-fA-F]' "$f" |
		grep -v -E '^[0-9]+:[[:space:]]*#')
	[ -n "$hits" ] && bad="$bad
${f#"$top"/}:
$hits"
done
[ -z "$bad" ] || fail "not portable (sed -i, readlink -f, sha256sum, md5sum, grep -P, printf \\x):$bad"
exit 0
