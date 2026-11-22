#!/bin/sh
# SPDX-License-Identifier: MIT
# The KPI suite. The TUI on a huge change set, as after resetting a
# kernel tree across a release, where every changed index entry has zeroed
# stat data and every git that refreshes the index hashes them all again.
# Each KPI is the time from the key to the screen showing the result, and
# the git processes it ran (GIT_CARVE_TRACE).
#
#   make kpi                a full run on a generated tree, timings and
#                           counts compared with tests/perf/kpi-full.txt
#   make kpi-save           the same, writing the results as the baseline
#   KPI_KERNEL=<linux repo> KPI_FROM=v7.1 KPI_TO=v7.2 make kpi
#                           a real kernel increment instead, in a temporary
#                           worktree of that repository, which is not
#                           touched otherwise, against
#                           tests/perf/kpi-kernel.txt
#   make check              a small tree, process counts only, against
#                           tests/perf/kpi-quick.txt
#
# KPI_FILES, KPI_CHANGED and KPI_NEW size the generated tree. A count above
# the baseline fails, and so does a time above twice the baseline plus
# 0.3 s. Times are compared only in a full run on the machine the baseline
# came from, and KPI_TIMES=0 skips them elsewhere.
. "$(dirname "$0")/../lib-tmux.sh"

if [ "${KPI_FULL:-0}" != 1 ]; then
	files=${KPI_FILES:-600} changed=${KPI_CHANGED:-450} new=${KPI_NEW:-250}
	base_file=$TESTS_DIR/perf/kpi-quick.txt
	times=0
else
	files=${KPI_FILES:-20000} changed=${KPI_CHANGED:-15000}
	new=${KPI_NEW:-1500}
	base_file=${KPI_BASELINE:-$TESTS_DIR/perf/kpi-full.txt}
	times=${KPI_TIMES:-1}
fi
[ -n "$KPI_KERNEL" ] && base_file=${KPI_BASELINE:-$TESTS_DIR/perf/kpi-kernel.txt}

# --- the tree -------------------------------------------------------------

# kpi_tree makes KPI_FILES files of 20 lines in 100 directories, with 60
# commits of history before them (the series, up being the first), and a
# "next" commit that changes line 10 of the first KPI_CHANGED files and
# adds KPI_NEW files. Then it resets to the base, as git reset across a
# release leaves it.
kpi_tree() {
	{
		awk -v n="$files" -v c="$changed" -v k="$new" 'BEGIN {
			t = 1577836800
			for (i = 1; i <= 60; i++) {
				printf "commit refs/heads/main\ncommitter T <t@t> %d +0000\n", t + i
				printf "data 12\nhist: c%03d\n\n", i
				printf "M 100644 inline hist/h%02d.c\ndata 3\n%02d\n\n", i % 7, i
			}
			printf "commit refs/heads/main\ncommitter T <t@t> %d +0000\n", t + 100
			printf "data 5\nbase\n\n"
			for (i = 0; i < n; i++) {
				printf "M 100644 inline drivers/d%02d/f%05d.c\n", i % 100, i
				s = ""
				for (l = 1; l <= 20; l++)
					s = s sprintf("int f%05d_%02d = %d;\n", i, l, l)
				printf "data %d\n%s\n", length(s), s
			}
			printf "commit refs/heads/next\ncommitter T <t@t> %d +0000\n", t + 200
			printf "data 5\nnext\nfrom refs/heads/main\n"
			for (i = 0; i < c; i++) {
				printf "M 100644 inline drivers/d%02d/f%05d.c\n", i % 100, i
				s = ""
				for (l = 1; l <= 20; l++)
					s = s sprintf("int f%05d_%02d = %d;\n", i, l,
						      l == 10 ? 1000 : l)
				if (i == c - 1)
					s = s "int kpi_needle;\n"
				printf "data %d\n%s\n", length(s), s
			}
			for (i = 0; i < k; i++) {
				s = sprintf("int n%05d;\n", i)
				printf "M 100644 inline drivers/new/n%05d.c\ndata %d\n%s\n", i, length(s), s
			}
			printf "\n"
		}'
	} | git fast-import --quiet || fail "fast-import"
	git checkout -q -f next || fail "checkout"
	# the series, the 60 commits of history and the base
	git branch -q up main~60
	git branch -q --set-upstream-to=up
	git reset -q main
}

# kernel_tree makes a detached worktree of KPI_KERNEL at KPI_TO, reset to
# KPI_FROM, and removes it at the end.
kernel_tree() {
	wt="$tmp/kernel"
	git -C "$KPI_KERNEL" worktree add -q --detach "$wt" "${KPI_TO:-v7.2}" ||
		fail "worktree add"
	# It is removed however the run ends, and tui_start's trap calls
	# this too. This is a command line for that trap to eval, and its
	# quotes are meant.
	# shellcheck disable=SC2089
	TUI_CLEANUP='git -C "$KPI_KERNEL" worktree remove --force "$wt" 2>/dev/null'
	# shellcheck disable=SC2090
	trap 'eval "$TUI_CLEANUP"; cleanup_tmp' EXIT
	cd "$wt" || fail "cd"
	git reset -q "${KPI_FROM:-v7.1}"
	# Paths that differ only in case (xt_CONNMARK.h, xt_connmark.h)
	# always look changed on a file system that ignores case. They are
	# put out of the way, since a split needs a clean tree.
	git ls-files | awk '{ k = tolower($0); n[k]++; p[NR] = $0 }
		END { for (i = 1; i <= NR; i++) if (n[tolower(p[i])] > 1)
			print p[i] }' >"$tmp/case"
	[ -s "$tmp/case" ] &&
		tr '\n' '\0' <"$tmp/case" |
		xargs -0 git update-index --skip-worktree --
	# the line the search looks for, in the last changed file
	_last=$(git diff --name-only | tail -1)
	echo "int kpi_needle;" >>"$_last"
}

new_repo
if [ -n "$KPI_KERNEL" ]; then
	kernel_tree
	# The commits get today's date, as yours would. The tests' fixed
	# date of 2020 is older than the kernel's history, and git, which
	# walks history by date, would then walk years of it to count the
	# series (4 s instead of 10 ms).
	unset GIT_AUTHOR_DATE GIT_COMMITTER_DATE
else
	t0=$(now)
	kpi_tree
	echo "generated $(git ls-files | wc -l | tr -d ' ') files, \
$(git diff --name-only | wc -l | tr -d ' ') changed, \
$(git ls-files -o --exclude-standard | wc -l | tr -d ' ') untracked \
in $(perl -e "printf '%.1f', $(now) - $t0") s"
fi
# The changed and untracked files written seconds ago. The diff cache does
# not trust a file younger than 2 s when its diff runs, as git does not a
# racy entry, and a fast machine would get to the steps within that time.
# Dated in the past, the cache counts the same everywhere.
{ git diff --name-only -z; git ls-files -z -o --exclude-standard; } |
	xargs -0 touch -t 202001010000
nchanged=$(git diff --name-only | wc -l | tr -d ' ')
[ -n "$KPI_KERNEL" ] && times=${KPI_TIMES:-1}

# KPI_TRACE_FILE keeps every git process of the run there
trace=${KPI_TRACE_FILE:-$tmp/trace}
: >"$trace"
cat >"$tmp/editor" <<'X'
#!/bin/sh
printf 'kpi: a commit\n' >"$1"
X
chmod +x "$tmp/editor"
TUI_ENV="GIT_CARVE_TRACE=$trace GIT_EDITOR=$tmp/editor"
export GIT_EDITOR="$tmp/editor"

# --- measuring ------------------------------------------------------------

results="$tmp/results"
: >"$results"

# start <name> [cold|watch]: once the TUI is idle (cold: before it
# starts), the clock and the trace from now on. A step's processes are
# those its keys cause, until the TUI is idle again (lib-tmux.sh), on any
# machine. A watch step sends no keys and only times what the screen shows.
start() {
	kpi=$1
	mode=${2:-keys}
	[ "$mode" = cold ] || wait_idle
	ic=$(idle_count)
	mark=$(wc -l <"$trace" | tr -d ' ')
	t0=$(now)
}

# until <text> [gone|re] [seconds] waits until the screen shows text (with
# gone, until it no longer does, and with re text is an extended regex).
# Then the KPI is written, the seconds and then the processes until the TUI
# is idle again. A timeout records -.
until_screen() {
	_lim=${3:-120}
	_end=$(perl -e "print $(now) + $_lim")
	while :; do
		if [ "$2" = gone ]; then
			screen 2>/dev/null | grep -F -q -- "$1" || break
		elif [ "$2" = re ]; then
			screen 2>/dev/null | grep -E -q -- "$1" && break
		else
			screen 2>/dev/null | grep -F -q -- "$1" && break
		fi
		if [ "$(perl -e "print $(now) > $_end ? 1 : 0")" = 1 ]; then
			echo "$kpi - -" >>"$results"
			printf '  %-22s timed out waiting for %s\n' "$kpi" "$1"
			return 1
		fi
		sleep 0.05
	done
	_t=$(perl -e "printf '%.2f', $(now) - $t0")
	[ "$mode" = watch ] || idle_after "$ic"
	_p=$(($(wc -l <"$trace" | tr -d ' ') - mark))
	# KPI_SHOW=<name> prints the processes of that step
	[ "${KPI_SHOW:-}" = "$kpi" ] && sed -n "$((mark + 1)),\$p" "$trace"
	echo "$kpi $_p $_t" >>"$results"
	printf '  %-22s %6s s  %3s git\n' "$kpi" "$_t" "$_p"
}

# kpi_count <name>: only processes, from start until the TUI is idle
kpi_count() {
	idle_after "$ic"
	_p=$(($(wc -l <"$trace" | tr -d ' ') - mark))
	echo "$1 $_p -" >>"$results"
	printf '  %-22s %8s  %3s git\n' "$1" "" "$_p"
}

# The TUI's memory, of git-carve itself as a child of the pane's shell or
# as the pane's process, never the shell's.
rss() {
	_pid=$(tmux -L "$TMUX_SOCK" list-panes -t t -F '#{pane_pid}')
	_n=20
	while [ "$_n" -gt 0 ]; do
		_c=$(ps -A -o pid= -o ppid= -o comm= | awk -v p="$_pid" \
			'($1 == p || $2 == p) && $3 ~ /git-carve$/ { print $1; exit }')
		[ -n "$_c" ] && break
		sleep 0.1
		_n=$((_n - 1))
	done
	[ -n "$_c" ] || fail "rss: no git-carve process under the pane"
	ps -o rss= -p "$_c" | tr -d ' '
}

echo "KPIs ($nchanged changed files):"

# 1. the first screen, then the untracked files
start startup cold
tui_start
until_screen "Unstaged ("
start untracked-scan watch
until_screen "scanning untracked" gone
echo "rss-kb $(rss) -" >>"$results"

# 2. holding j in the Files pane, 30 keys in one burst as a held key
# sends them, and the diffs loaded
start hold-j-files
keys j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j
kpi_count hold-j-files
step_keys g g

# 3. the Series pane, 40 rows down
step_keys BTab
start hold-j-series
keys j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j j
kpi_count hold-j-series
[ -n "$KPI_TRACE" ] && sed -n "$((mark + 1)),\$p" "$trace" >"$KPI_TRACE"
# back to the work tree on top
step_keys g g
wait_gone "Commit ~"
step_keys Tab

# 4. search all diffs for a line in the last changed file
step_keys Tab
start search
keys / k p i _ n e e d l e Enter
# found, or too much to search, as a kernel release is over the limit
until_screen '\+int kpi_needle|not searched' re
step_keys Escape BTab

# 5. F5, then ? (shown once F5 is done)
start refresh
keys F5 '?'
until_screen "Keys in the"
step_keys q

# 6. stage one file, commit it
step_keys g g
start stage-file
keys Enter
until_screen "Staged ("
start commit-one
keys c
until_screen "committed"

# 7. everything, Enter on the Unstaged section row after asking, the first
# file, then up to its section's row
step_keys g g k
start stage-all
keys Enter y
until_screen '^staged [0-9]+ files' re 120
staged_all=$?
if [ $staged_all = 0 ]; then
	# and back, Enter on the Staged section row, then again
	step_keys g g k
	start unstage-all
	keys Enter y
	until_screen '^unstaged [0-9]+ files' re 120
	step_keys g g k
	keys Enter y
	wait_match '^staged [0-9]* files' || fail "staging again: $(screen)"
fi
tui_quit
if [ $staged_all != 0 ]; then
	# without a stage-all key, as git does it, to go on
	git add -u
fi

# 8. commit everything
tui_start
wait_screen "Staged (" 600
wait_gone "scanning untracked" 600
start commit-all
keys c
until_screen "committed"

# 9. split that commit, give it up again
start split
keys s y
until_screen "SPLIT"
start split-abort
keys X y
until_screen "split aborted"
tui_quit

# --- the results ----------------------------------------------------------

[ "${KPI_SAVE:-0}" = 1 ] && {
	cp "$results" "$base_file"
	echo "saved as $base_file"
	exit 0
}
[ -f "$base_file" ] || { echo "no baseline ($base_file): make kpi-save"; exit 0; }

bad=
while read -r name procs secs; do
	b=$(awk -v n="$name" '$1 == n { print $2, $3 }' "$base_file")
	[ -n "$b" ] || continue
	bp=${b% *} bt=${b#* }
	if [ "$name" = rss-kb ]; then
		# memory up to twice the baseline, in full runs only, since
		# the sanitizers take far more
		[ "$times" = 1 ] && [ "$procs" != - ] && [ "$bp" != - ] &&
			[ "$procs" -gt $((bp * 2)) ] &&
			bad="$bad $name(${procs}kB>2x${bp})"
		continue
	fi
	if [ "$procs" = - ]; then
		[ "$bp" != - ] && bad="$bad $name(timed-out)"
		continue
	fi
	# A step's processes do not depend on the machine (start), and a
	# count above the baseline is a regression. tests/perf/spawns.sh
	# keeps the budgets of the single keys.
	lim=$bp
	[ "$bp" != - ] && [ "$procs" -gt "$lim" ] &&
		bad="$bad $name(git:$procs>$lim)"
	[ "$times" = 1 ] && [ "$secs" != - ] && [ "$bt" != - ] &&
		[ "$(perl -e "print $secs > 2 * $bt + 0.3 ? 1 : 0")" = 1 ] &&
		bad="$bad $name(${secs}s>2x${bt}s)"
done <"$results"
[ -z "$bad" ] || fail "regressions:$bad"
echo "no regression against $(basename "$base_file")"
exit 0
