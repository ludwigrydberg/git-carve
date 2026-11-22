#!/bin/sh
# SPDX-License-Identifier: MIT
# The man page documents every config option (the README too) and every
# key, the CLI's help every command, and the man page renders cleanly
# where a formatter is available.
. "$(dirname "$0")/../lib.sh"
new_repo
man="$TESTS_DIR/../docs/git-carve.1"
readme="$TESTS_DIR/../README.md"
[ -f "$man" ] || fail "no man page"

# git-carve-cli help lists every command of its table in src/cli/cli.c. The
# CLI is internal, and neither the man page nor the README describe it.
src="$TESTS_DIR/../src"
cmds=$(sed -n '/cmds\[\] = {/,/^};/p' "$src/cli/cli.c" |
	sed -n 's/^[[:space:]]*{ "\([a-z-]*\)",.*/\1/p')
[ "$(echo "$cmds" | wc -l)" -gt 30 ] || fail "too few commands found: $cmds"
for cmd in $cmds; do
	carve help | grep -q -- "^   $cmd\( \|$\)\|| $cmd " ||
		fail "'git-carve-cli help' lacks $cmd"
done
for doc in "$man" "$readme"; do
	grep -n 'git-carve-cli\|git carve \(diff\|stage\|unstage\|commit\|split\|show\|fixup\|squash\|defer\|continue\|abort\|status\|undo\|discard\|resolve\|absorb\|gc\|move\|drop\|edit\|reword\|blame\)\([^a-z-]\|$\)' \
		"$doc" && fail "$(basename "$doc") describes the CLI"
done
# every config option the code reads (carve.keys.<action>: once)
opts=$(cat "$src"/*.c "$src"/ui/*.c | grep -o '"carve\.[A-Za-z]*\.\{0,1\}' |
	tr -d '"' | sed 's/^carve\.keys$/carve.keys./' | sort -u)
[ "$(echo "$opts" | wc -l)" -gt 10 ] || fail "too few options found: $opts"
for opt in $opts; do
	grep -q "^\.BI\{0,1\} $opt\( \|$\)" "$man" || fail "man page lacks $opt"
	grep -q "| \`$opt" "$readme" || fail "README lacks $opt"
done

# every default key of the TUI is set in bold somewhere in the man page
{ grep '^\.\(B\|BR\|RB\|BI\|IB\) ' "$man" | sed 's/^\.[A-Z]* //'
  grep -o '\\fB[^\\]*\\f[RP]' "$man"; } |
	sed 's/\\f[BRIP]//g; s/\\-/-/g; s/"/ /g' | tr ' ' '\n' >"$tmp/bold"
tui_cmd keys | awk 'NR > 1 && !NF { exit } NR > 1 { sub(/^[^ ]+ +/, "");
	sub(/  .*/, ""); print }' | tr ',' '\n' | sed 's/^ *//' >"$tmp/keys"
[ "$(wc -l <"$tmp/keys")" -gt 50 ] || fail "too few keys in 'git carve keys'"
awk 'NR == FNR { bold[$0] = 1; next } !($0 in bold) {
	print "man page lacks key " $0; bad = 1 } END { exit bad }' \
	"$tmp/bold" "$tmp/keys" || fail "keys missing from the man page"

if command -v mandoc >/dev/null 2>&1; then
	mandoc -T lint -W warning "$man" >"$tmp/lint" 2>&1 ||
		{ cat "$tmp/lint"; fail "mandoc lint"; }
elif command -v groff >/dev/null 2>&1; then
	groff -man -Tutf8 -ww -z "$man" 2>"$tmp/lint"
	[ -s "$tmp/lint" ] && { cat "$tmp/lint"; fail "groff warnings"; }
fi
exit 0
