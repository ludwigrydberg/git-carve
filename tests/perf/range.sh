#!/bin/sh
# SPDX-License-Identifier: MIT
# git carve <range> opens at once, whatever the range's size, without a
# checkout, with one git log for what fits and the rest loaded as the
# cursor gets there. It is checked on ranges of 10 and 5000 commits, for
# the git processes at startup (the same for both), and moving over 30
# commits at once, which loads only the commit it settles on. The times it
# prints are compared by make kpi, on the machine of its baseline.
. "$(dirname "$0")/../lib-tmux.sh"

# gen_series <n>: a base commit and n commits on top, each changing f.c
gen_series() {
	awk -v n="$1" 'BEGIN {
		printf "blob\nmark :1\ndata 2\n0\n\n"
		printf "commit refs/heads/main\nmark :2\n"
		printf "committer T <t@t> 1577836800 +0000\ndata 5\nbase\n"
		printf "M 100644 :1 f.c\n\n"
		for (i = 1; i <= n; i++) {
			printf "blob\nmark :%d\ndata %d\n%d\n\n", 2 * i + 1,
				length(i "") + 1, i
			printf "commit refs/heads/main\nmark :%d\n", 2 * i + 2
			printf "committer T <t@t> %d +0000\n", 1577836800 + i
			printf "data %d\nC%d\nfrom :%d\n", length("C" i), i, 2 * i
			printf "M 100644 :%d f.c\n\n", 2 * i + 1
		}
	}' | git fast-import --quiet || fail "fast-import"
	git checkout -q -f main || fail "checkout"
	git tag base "HEAD~$1"
}

# open <n>: time to the first screen and git processes at startup
open_range() {
	: >"$tmp/gitlog"
	t0=$(now)
	saved=$CARVE
	CARVE="$CARVE base..HEAD"
	tui_start
	CARVE=$saved
	wait_screen "C$1" 100
	wait_screen "Commit HEAD (1)" 100
	t=$(elapsed "$t0" "$(now)")
	wait_gone "scanning untracked" 100
	wait_idle
	spawns=$(wc -l <"$tmp/gitlog" | tr -d ' ')
}

new_repo small
gen_series 10
git_shim "printf '%s\n' \"\$*\" >>\"$tmp/gitlog\""
open_range 10
small=$spawns
echo "10 commits: first screen after ${t}s, $small git processes"
tui_quit

new_repo big
gen_series 5000
git_shim "printf '%s\n' \"\$*\" >>\"$tmp/gitlog\""
open_range 5000
big=$spawns
echo "5000 commits: first screen after ${t}s, $big git processes"
[ "$big" = "$small" ] ||
	fail "startup spawns depend on the range: $big vs $small"
grep -q -E '^(checkout|rebase|reset|commit|update-ref) ' "$tmp/gitlog" &&
	fail "opening a range changed something: $(cat "$tmp/gitlog")"

# J over 30 commits loads one commit at the end, not 30
: >"$tmp/gitlog"
keys J J J J J J J J J J J J J J J J J J J J J J J J J J J J J J
wait_screen "Commit ~30 (1)"
wait_idle
n=$(grep -c 'diff-tree\|^diff ' "$tmp/gitlog")
echo "git runs for the commits' files while moving over 30: $n"
[ "$n" -le 3 ] || fail "$n commit loads for one move: $(cat "$tmp/gitlog")"
tui_quit
