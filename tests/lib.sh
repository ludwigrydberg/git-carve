# shellcheck shell=sh
# SPDX-License-Identifier: MIT
# Shared helpers for the shell tests. They are POSIX sh and must work with
# the stock tools of macOS and Debian.

TESTS_DIR=$(cd "$(dirname "$0")/.." && pwd)
# CARVE is the TUI (git-carve), and CARVE_CLI runs the commands the tests
# drive.
CARVE=${CARVE:-$TESTS_DIR/../git-carve}
case $CARVE in /*) ;; *) CARVE=$(pwd)/$CARVE ;; esac
CARVE_CLI=${CARVE_CLI:-$TESTS_DIR/../git-carve-cli}
case $CARVE_CLI in /*) ;; *) CARVE_CLI=$(pwd)/$CARVE_CLI ;; esac
export CARVE CARVE_CLI

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

skip() {
	echo "$*"
	exit 77
}

cleanup_tmp() {
	if [ -n "$KEEP_TMP" ]; then
		echo "kept $tmp" >&2
	else
		rm -rf "$tmp"
	fi
}

write_hostile_config() {
	cat >"$1" <<'CFG'
[diff]
	noprefix = true
	mnemonicPrefix = true
	external = false
[color]
	ui = always
[apply]
	whitespace = fix
[core]
	autocrlf = input
	quotePath = true
CFG
}

# new_repo [name]: create an isolated repository and cd into it.
new_repo() {
	tmp=$(mktemp -d "${TMPDIR:-/tmp}/carve-test.XXXXXX") ||
		fail "mktemp"
	tmp=$(cd "$tmp" && pwd -P)
	trap cleanup_tmp EXIT
	mkdir -p "$tmp/home" "$tmp/bin"
	export HOME="$tmp/home" GIT_CONFIG_NOSYSTEM=1
	export GIT_CONFIG_GLOBAL="$tmp/home/.gitconfig"
	export GIT_AUTHOR_NAME=T GIT_AUTHOR_EMAIL=t@t
	export GIT_COMMITTER_NAME=T GIT_COMMITTER_EMAIL=t@t
	export GIT_AUTHOR_DATE='2020-01-01T00:00:00Z'
	export GIT_COMMITTER_DATE=$GIT_AUTHOR_DATE
	export EDITOR=true GIT_EDITOR=true
	# nothing from the caller's git environment (git -c, hooks, rebase -x)
	unset VISUAL GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_CONFIG_PARAMETERS \
		GIT_CONFIG_COUNT GIT_OBJECT_DIRECTORY GIT_PREFIX \
		GIT_ALTERNATE_OBJECT_DIRECTORIES
	: >"$GIT_CONFIG_GLOBAL"
	# The TUI tests were written for two diff panes, no message pane
	# and one file at a time. CARVE_TEST_LAYOUT=default tests the TUI
	# as it starts for users.
	if [ "${CARVE_TEST_LAYOUT:-two}" = two ]; then
		cat >"$GIT_CONFIG_GLOBAL" <<-EOF
		[carve]
			layout = two
			messagePane = false
			allFiles = false
		EOF
	fi
	if [ -n "$CARVE_HOSTILE" ]; then
		write_hostile_config "$GIT_CONFIG_GLOBAL"
	fi
	ln -s "$CARVE" "$tmp/bin/git-carve"
	PATH="$tmp/bin:$PATH"
	export PATH
	git init -q "$tmp/${1:-repo}" || fail "git init"
	cd "$tmp/${1:-repo}" || fail "cd"
	git symbolic-ref HEAD refs/heads/main
}

# "VAR=x carve ..." keeps VAR set afterwards in bash (/bin/sh on macOS),
# because carve is a function. Use env VAR=x "$CARVE_CLI" ... instead.
carve() {
	"$CARVE_CLI" "$@"
}

# The TUI binary's own commands (keys, --version, --help).
tui_cmd() {
	"$CARVE" "$@"
}

# Run a command that must fail.
must_fail() {
	if "$@" >"$tmp/must_fail.out" 2>&1; then
		cat "$tmp/must_fail.out" >&2
		fail "expected failure: $*"
	fi
}

assert_eq() {
	[ "$1" = "$2" ] || fail "${3:-assert_eq}: got '$1', expected '$2'"
}

# Content hash of a file, no filters applied.
file_hash() {
	git hash-object --no-filters -- "$1"
}

# Blob id of the index entry for a path.
index_blob() {
	git rev-parse ":$1" 2>/dev/null || echo none
}

# assert_index <path> <file-with-expected-content>
assert_index() {
	exp=$(file_hash "$2")
	got=$(index_blob "$1")
	if [ "$exp" != "$got" ]; then
		echo "--- index content of $1:" >&2
		git cat-file blob ":$1" >&2
		echo "--- expected:" >&2
		cat "$2" >&2
		fail "index content of $1 differs"
	fi
}

# assert_index_text <path> <expected-text> (\n and the like expanded)
assert_index_text() {
	printf '%b' "$2" >"$tmp/expected"
	assert_index "$1" "$tmp/expected"
}

# Hash of every tracked (and i-t-a) file in the worktree plus names.
worktree_state() {
	git ls-files -z | xargs -0 git hash-object --no-filters -- | \
		git hash-object --stdin
	git ls-files | git hash-object --stdin
}

# Porcelain status without colors, independent of user config.
short_status() {
	git -c color.status=never status --porcelain --untracked-files=all
}

commit_all() {
	git add -A && git -c commit.gpgsign=false commit -q -m "$1" ||
		fail "commit $1"
}

tree_of() {
	git rev-parse "$1^{tree}"
}

# edit <line> <new line> [file]: replace a whole line (default a.c)
edit() {
	_f=${3:-a.c}
	awk -v a="$1" -v b="$2" '{ if ($0 == a) print b; else print }' "$_f" \
		>"$_f.tmp" && mv "$_f.tmp" "$_f"
}

# changes <commit>: its added and removed "l..." lines, on one line
changes() { git show "$1" --format= | grep '^[-+]l' | tr '\n' ' '; }

# now prints the seconds since the epoch, to the millisecond
now() {
	perl -MTime::HiRes=time -e 'printf "%.3f\n", time'
}

# elapsed <start> <end>: seconds between two now values
elapsed() {
	perl -e "printf \"%.2f\", $2 - $1"
}

# new_no [--cached] <path> <text>: new-side number of the first added line
# containing text in the annotated diff.
new_no() {
	_c=
	[ "$1" = --cached ] && { _c=--cached; shift; }
	carve diff $_c "$1" | awk -v pat="$2" '
		substr($0, 13, 1) == "+" && index(substr($0, 14), pat) {
			print substr($0, 7, 5) + 0; exit
		}'
}

# old_no [--cached] <path> <text>: old-side number of the first removed line
old_no() {
	_c=
	[ "$1" = --cached ] && { _c=--cached; shift; }
	carve diff $_c "$1" | awk -v pat="$2" '
		substr($0, 13, 1) == "-" && index(substr($0, 14), pat) {
			print substr($0, 1, 5) + 0; exit
		}'
}

# git_shim <sh-snippet>: put a git wrapper first in PATH that runs the
# snippet (with "$@" = git's arguments) and then the real git.
git_shim() {
	_real=$(command -v git)
	cat >"$tmp/bin/git" <<SHIM
#!/bin/sh
$1
exec "$_real" "\$@"
SHIM
	chmod +x "$tmp/bin/git"
}

# The state of a repository for the tests that interrupt operations
# (rollback.sh, crash.sh), each check in its own directory $_w with a
# template repository $tmpl.

# the files of the operation in progress, but the undo history
state_files() {
	for f in "$(git rev-parse --git-path carve)"/*; do
		case ${f##*/} in
		undo* | "*") ;;
		*) printf '%s ' "${f##*/}" ;;
		esac
	done
}

# HEAD, index, work tree and the operation's state
state() {
	printf '%s %s %s %s\n' "$(git rev-parse HEAD)" \
		"$(git write-tree 2>/dev/null || echo conflict)" \
		"$(worktree_state)" "$(state_files)"
}

# fresh <prepare>: in $_w, a copy of the template repository, prepared
fresh() {
	cd "$_w" || fail "cd"
	rm -rf run
	cp -R "$tmpl" run
	cd run || fail "cd run"
	eval "$1"
}
