#!/bin/sh
# SPDX-License-Identifier: MIT
# Performance on a tree the size of a kernel, a generated repository with
# 50,000 tracked files and thousands of untracked ones. It checks a number
# of git processes at startup that does not depend on the tree size, lazy
# diffs while moving, that the first screen does not wait for the untracked
# scan, and that the user's fsmonitor hook is used. The times it prints are
# compared by make kpi, on the machine of its baseline.
. "$(dirname "$0")/../lib-tmux.sh"

# gen_tree <dirs>: <dirs> x 100 tracked files, 60 of them modified,
# <dirs> x 10 untracked files
gen_tree() {
	{
		printf 'blob\nmark :1\ndata 11\nint x = 1;\n\n'
		printf 'commit refs/heads/main\ncommitter T <t@t> 1577836800 +0000\n'
		printf 'data 5\nbase\n\n'
		awk -v n="$1" 'BEGIN { for (d = 0; d < n; d++) for (f = 0; f < 100; f++)
			printf "M 100644 :1 drivers/d%03d/f%03d.c\n", d, f }'
	} | git fast-import --quiet || fail "fast-import"
	git checkout -q -f main || fail "checkout"
	i=0
	while [ $i -lt 60 ]; do
		printf 'int x = 2;\n' >"drivers/d000/f$(printf %03d $i).c"
		i=$((i + 1))
	done
	awk -v n="$1" 'BEGIN { for (d = 0; d < n; d++) for (f = 0; f < 10; f++)
		print "x" > sprintf("drivers/d%03d/tmp%d.o", d, f) }'
}

# startup_spawns <untracked files> counts the git processes until the first
# screen is complete and the untracked scan merged, into $spawns. Diffs are
# counted apart in $diffs, since the first file's may come from the diff
# cache's diffs of the whole tree, which run whenever the keys pause.
startup_spawns() {
	: >"$tmp/gitlog"
	tui_start
	wait_screen "M   f000.c" 100
	wait_screen "+int x = 2;" 100
	wait_screen "Unstaged (60)" 100
	wait_gone "scanning untracked" 100
	wait_idle
	spawns=$(grep -vc ' diff ' "$tmp/gitlog")
	diffs=$(grep -c ' diff ' "$tmp/gitlog")
	[ "$diffs" -le 4 ] || fail "$diffs git diff at startup"
	# the untracked files merged in, shown by the folded row, the last
	keys G
	wait_screen "Untracked ($1) [+]" 100
	tui_quit
}

# --- a tiny tree for comparison
new_repo small
gen_tree 1
git_shim "printf '%s\n' \"\$*\" >>\"$tmp/gitlog\""
startup_spawns 10
small=$spawns

# --- the big tree
new_repo big
t0=$(now)
gen_tree 500
echo "generated $(git ls-files | wc -l | tr -d ' ') tracked files in $(elapsed "$t0" "$(now)")s"
git_shim "printf '%s\n' \"\$*\" >>\"$tmp/gitlog\""

startup_spawns 5000
big=$spawns
echo "git processes at startup: $big (tiny tree: $small)"
[ "$big" = "$small" ] ||
	fail "startup spawns depend on the tree size: $big vs $small"
[ "$big" -le 30 ] || fail "too many git processes at startup: $big"

# time to the first screen
t0=$(now)
tui_start
wait_screen "M   f000.c" 100
t=$(elapsed "$t0" "$(now)")
echo "first screen after ${t}s"

# moving through 40 files in one burst, as a held key sends them, loads
# diffs only at the end
wait_gone "scanning untracked" 100
wait_idle
: >"$tmp/gitlog"
keys j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j
wait_screen "Unstaged: drivers/d000/f040.c"
wait_screen "+int x = 2;"
wait_idle
n=$(grep -c ' diff ' "$tmp/gitlog")
echo "git diff runs while moving over 40 files: $n"
[ "$n" -le 4 ] || fail "$n git diff runs for one move"
tui_quit

# the first screen does not wait for the untracked scan: here the scan
# cannot end until the first screen is there
git_shim "printf '%s\n' \"\$*\" >>\"$tmp/gitlog\"
case \" \$* \" in *\" --untracked-files=all \"*|*\" ls-files -z -o \"*)
	while [ -e '$tmp/hold' ]; do sleep 0.05; done ;;
esac"
touch "$tmp/hold"
tui_start
wait_screen "M   f000.c" 100
screen | grep -q "scanning untracked" || fail "no scan running at the first screen"
rm "$tmp/hold"
wait_gone "scanning untracked" 100
keys G
wait_screen "Untracked (5000) [+]" 100
tui_quit

# the user's fsmonitor hook is used (never disabled by us)
rm -f "$tmp/bin/git"
cat >"$tmp/fsmonitor" <<X
#!/bin/sh
echo "\$*" >>"$tmp/fsmonitor.log"
exit 1
X
chmod +x "$tmp/fsmonitor"
git config core.fsmonitor "$tmp/fsmonitor"
tui_start
wait_screen "M   f000.c" 100
tui_quit
[ -s "$tmp/fsmonitor.log" ] || fail "the fsmonitor hook was never called"

# the CLI on the big tree
t0=$(now)
carve diff drivers/d000/f001.c >/dev/null || fail "diff"
carve stage drivers/d000/f001.c o1,n1 || fail "stage"
t=$(elapsed "$t0" "$(now)")
echo "CLI diff + stage: ${t}s"
exit 0
