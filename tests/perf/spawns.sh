#!/bin/sh
# SPDX-License-Identifier: MIT
# git processes per operation. Each command and key below has a budget, and
# more than 5 needs a reason here. A count above its budget fails, and so
# does one that depends on the size of what is involved. Two fixtures
# differ in tracked files, commits in the series and change blocks.
. "$(dirname "$0")/../lib-tmux.sh"

failed=

# budget <key> <name> <max> [reason]: c_<key> against max
budget() {
	eval "_c=\$c_$1"
	if [ "$3" -gt 5 ] && [ -z "$4" ]; then
		echo "$2: budget $3 is above 5 without a reason"
		failed="$failed '$2'"
	fi
	if [ "$_c" -gt "$3" ]; then
		echo "$2: $_c git processes, budget $3; the calls:"
		sed 's/^/	/' "$tmp/log_$1"
		failed="$failed '$2'"
	else
		echo "$2: $_c (budget $3)"
	fi
}

# count <key>: the processes logged since the last : >gitlog
count() {
	cp "$tmp/gitlog" "$tmp/log_$1"
	eval "c_$1=$(wc -l <"$tmp/gitlog" | tr -d ' ')"
}

# one line per git process, whatever newlines its arguments hold
log_shim() {
	git_shim "{ printf '%s' \"\$*\" | tr '\n' ' '; echo; } >>\"$tmp/gitlog\""
}

# cli <key> <args...>: the processes of one command
cli() {
	_k=$1
	shift
	: >"$tmp/gitlog"
	carve "$@" >/dev/null 2>&1
	count "$_k"
}

# staged <n>: wait until n changed lines of a.c are staged
staged() {
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ]; do
		[ "$(git diff --cached a.c | grep -c '^+LINE')" = "$1" ] &&
			return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	fail "$1 staged lines expected"
}

# a.c has no unstaged changes
all_staged() {
	git diff --quiet a.c || fail "a.c not staged"
}

# new.c is staged
new_staged() {
	[ -n "$(git diff --cached --name-only -- new.c)" ] ||
		fail "new.c not staged"
}

# tui <key> <keys...>: the processes of keys sent in one write, from the
# TUI idle before them until it is idle again (lib-tmux.sh). That includes
# the undo entries kept back until the keys pause.
tui() {
	_k=$1
	shift
	wait_idle
	_ic=$(idle_count)
	: >"$tmp/gitlog"
	keys "$@"
	idle_after "$_ic"
	count "$_k"
}

# fixture <extra files> <commits> <blocks>: a series of commits on an
# upstream, a.c with <blocks> changed lines (every 10th), new.c untracked
fixture() {
	new_repo "r$1"
	i=0
	while [ $i -lt "$1" ]; do
		mkdir -p "d$((i / 100))"
		echo "x $i" >"d$((i / 100))/f$i.c"
		i=$((i + 1))
	done
	awk -v n="$3" 'BEGIN { for (i = 1; i <= n * 10; i++) print "line " i }' >a.c
	commit_all base
	git branch -q up
	git branch -q --set-upstream-to=up
	i=1
	while [ $i -le "$2" ]; do
		echo "c$i" >"s$i.c"
		commit_all "s: commit $i"
		i=$((i + 1))
	done
	awk -v n="$3" 'BEGIN { for (i = 1; i <= n * 10; i++)
		print (i % 10 == 5 ? "LINE " : "line ") i }' >a.c
	awk 'BEGIN { for (i = 1; i <= 20; i++) print "new " i }' >new.c
	# dated in the past, or the diff cache would not trust them for 2 s
	# (kpi.sh), and the counts would depend on the machine's speed
	touch -t 202001010000 a.c new.c
	log_shim
	carve status >/dev/null	# the version check, remembered
	# the commit editor writes a message
	printf '#!/bin/sh\nprintf "s: tui commit\\n" >"$1"\n' >"$tmp/editor"
	chmod +x "$tmp/editor"
	GIT_EDITOR="$tmp/editor"
	export GIT_EDITOR
}

# measure <extra files> <commits> <blocks>: c_<op> of every operation
measure() {
	fixture "$@"

	cli cli_diff diff a.c
	# the first records the index before it too, and the second is the
	# rule
	cli cli_first_stage stage a.c o5,n5
	cli cli_unstage unstage a.c o5,n5
	cli cli_stage stage a.c o5,n5
	cli cli_unstage unstage a.c o5,n5
	cli cli_stage_file stage-file a.c
	cli cli_unstage_file unstage-file a.c
	cli cli_status split --status
	cli cli_absorb absorb
	cli cli_blame blame a.c

	: >"$tmp/gitlog"
	tui_start
	wait_idle
	wait_screen ">M a.c"
	count startup
	step_keys Tab
	wait_screen "> Unstaged: a.c"
	# the first stage reads the undo history, then comes the steady
	# state
	tui first a Space; staged 1
	tui space a Space; staged 2
	tui burst a Space a Space; staged 4
	# typing: the second key once the first is done, within the pause,
	# and still one write of the undo entries after both
	wait_idle
	_ic=$(idle_count)
	: >"$tmp/gitlog"
	keys a Space
	staged 5
	keys a Space
	idle_after "$_ic"
	count typing
	staged 6
	step_keys Tab
	wait_screen "> Staged: a.c"
	tui space_unstage a Space; staged 5
	tui undo u; staged 6
	step_keys BTab BTab
	wait_screen "> Files"
	# a.c in the Unstaged section, below its Staged entry (the first)
	# and the section's row, and Enter stages it all
	step_keys g g j j
	tui enter Enter; all_staged
	tui commit c; wait_screen "committed"
	tui series_move J; wait_screen "Commit HEAD"
	tui refresh C-l
	# a new file after a commit is staged without listing HEAD's tree.
	# It is under the folded Untracked row, the last, which l unfolds.
	step_keys G l j
	wait_screen ">? new.c"
	tui enter_new Enter; new_staged
	tui_quit

	cli cli_commit_undo undo
	cli cli_commit commit -m "s: again" --no-signoff
	# a fixup! commit of HEAD~1, then the squash of it
	echo more >>a.c
	git add a.c
	cli cli_fixup fixup HEAD~1
	cli cli_squash squash
}

all() {
	for k in cli_diff cli_first_stage cli_stage cli_unstage cli_stage_file \
		cli_unstage_file cli_status cli_absorb cli_blame startup space burst \
		typing space_unstage undo refresh enter enter_new commit \
		series_move \
		cli_commit_undo cli_commit cli_fixup cli_squash; do
		eval "printf '%s=%s ' $k \$c_$k"
	done
}

# the undo bracket of a single command, the list of entries, write-tree,
# commit-tree, update-ref and the configuration (carve.undoLimit)
undo_cli="the undo bracket (for-each-ref, config, write-tree, commit-tree, update-ref)"

measure 0 2 8
small=$(all)
budget cli_diff "cli diff" 3
budget cli_stage "cli stage" 10 "apply, diff, ls-files; $undo_cli; open, cat-file"
budget cli_first_stage "cli stage (first)" 10 "as cli stage (no entry yet: the index before it from write-tree)"
budget cli_unstage "cli unstage" 10 "as cli stage"
budget cli_stage_file "cli stage-file" 9 "git add; $undo_cli; open, both cat-file workers"
budget cli_unstage_file "cli unstage-file" 9 "as cli stage-file"
budget cli_status "cli split --status" 3
budget cli_absorb "cli absorb (plan)" 12 "diff, blame per file, rev-list, log of the targets"
budget cli_blame "cli blame" 5
budget startup "tui startup" 20 "see tests/perf/big-tree.sh"
# A line key runs apply and the two diffs, and its undo entry is the patch
# it applied. When the keys pause, the entries kept back are written in one
# go with for-each-ref, hash-object and update-ref.
budget space "tui Space" 6 "3 per key + 3 per pause, see above"
budget burst "tui Space x2" 9 "3 per key + 3 per pause"
budget typing "tui Space, then Space again" 9 "3 per key + 3 per pause"
budget space_unstage "tui Space unstage" 6 "as tui Space"
budget enter "tui Enter" 9 "git add, two diffs; its undo entry keeps the index tree: after line keys (patches) the history and write-tree, write-tree after; 3 per pause"
budget undo "tui u" 9 "for-each-ref, diff-tree + update-index, update-ref; the file reloaded"
budget enter_new "tui Enter (new file)" 8 "as tui Enter; not git ls-tree -r of HEAD"
budget refresh "tui F5" 10 "everything again, as asked: status, config, upstream, scan, diffs"
budget commit "tui c" 9 "prefix (staged names, their log), git commit, status, the undo entry (list, commit-tree, update-ref; the index tree is HEAD's), the untracked file's diff, the unstaged side of the diff cache; nothing staged, title and series from the status and the cat-file worker"
budget series_move "tui J" 3
budget cli_commit_undo "cli undo" 8 "list, cat-file, diff-tree + update-index, update-ref"
budget cli_commit "cli commit" 10 "git commit; $undo_cli"
budget cli_fixup "cli fixup" 11 "rev-list (the commit in the series), git commit; $undo_cli; open, cat-file workers; nothing staged: diff-index only after git commit failed"
budget cli_squash "cli squash" 12 "upstream, one log of the series, stash create (clean: no reset, no update-ref), the rebase; $undo_cli; open, cat-file workers"

measure 1000 8 12
big=$(all)
if [ "$small" != "$big" ]; then
	echo "counts depend on the size:"
	echo "  small: $small"
	echo "  big:   $big"
	failed="$failed size"
fi
[ -z "$failed" ] || fail "over budget:$failed"
exit 0
