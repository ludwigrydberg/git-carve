#!/bin/sh
# SPDX-License-Identifier: MIT
# The binaries run, report their version and refuse unknown commands. git
# carve is the TUI only, and the commands are git-carve-cli's.
. "$(dirname "$0")/../lib.sh"
new_repo

tui_cmd --version | grep -q '^git-carve version ' || fail "version"
tui_cmd -V | grep -q '^git-carve version ' || fail "-V"
tui_cmd -h | grep -q '^usage: git carve' || fail "-h"
tui_cmd --bogus 2>&1 | grep -q "unknown option '--bogus'" ||
	fail "unknown option of git carve"
tui_cmd HEAD~1.. extra 2>&1 | grep -q "unknown argument 'extra'" ||
	fail "a second argument"
git carve version | grep -q '^git-carve version ' || fail "git carve version"
carve --version | grep -q '^git-carve-cli version ' || fail "cli version"
must_fail carve no-such-command
grep -q "unknown command 'no-such-command'" "$tmp/must_fail.out" ||
	fail "message: $(cat "$tmp/must_fail.out")"

cd "$tmp" && must_fail carve diff x
grep -q "not inside a git work tree" "$tmp/must_fail.out" ||
	fail "outside repo message: $(cat "$tmp/must_fail.out")"

# commit with -m, -F and --amend
cd "$tmp/repo" || fail cd
printf 'x\n' >c.txt
carve stage-file c.txt
carve commit -m "cli: add c" >/dev/null || fail "commit -m"
[ "$(git log -1 --format=%s)" = "cli: add c" ] || fail "commit -m subject"
printf 'y\n' >>c.txt
carve stage-file c.txt
printf 'cli: amended\n\nbody\n' >"$tmp/msg"
carve commit --amend -F "$tmp/msg" >/dev/null || fail "commit --amend -F"
[ "$(git log -1 --format=%s)" = "cli: amended" ] || fail "amend subject"
[ "$(git rev-list --count HEAD)" = 1 ] || fail "amend count"
must_fail carve commit -m x -F "$tmp/msg"

# selections with n, o, new: and old:, never starting with "-". Options may
# come anywhere and "--" ends them.
printf 'p\nq\n' >sel.c
carve stage-file sel.c
carve commit -m sel >/dev/null
printf 'p\nQ\n' >sel.c
must_fail carve stage sel.c -L2,L2
grep -q "o<N>" "$tmp/must_fail.out" || fail "old syntax hint: $(cat "$tmp/must_fail.out")"
carve stage old:2,new:2 -- sel.c 2>/dev/null && fail "positional order ignored"
carve stage sel.c old:2,new:2 || fail "long forms"
[ "$(git diff --cached --name-only)" = sel.c ] || fail "long forms staged"
carve unstage -- sel.c o2,n2 || fail "-- before positionals"
[ -z "$(git diff --cached --name-only)" ] || fail "unstage with --"
carve diff --cached=x sel.c 2>/dev/null && fail "flag with value accepted"
must_fail carve stage --bogus sel.c n2
grep -q "unknown option '--bogus'" "$tmp/must_fail.out" || fail "unknown option"
carve set-staged sel.c --print o2,n2 | grep -qx Q || fail "option after positionals"
# a path starting with "-" needs "--"
printf 'x\n' >-dash.c
carve stage -- -dash.c n1 || fail "path starting with -"
[ "$(git diff --cached --name-only)" = -dash.c ] || fail "dash path staged"
carve diff sel.c | grep -q '^  old   new$' || fail "column header"

# git carve help shows the TUI and its keys, no command
tui_cmd help >"$tmp/help"
grep -q "git carve keys" "$tmp/help" || fail "help lacks keys"
grep -q '^   stage ' "$tmp/help" && fail "help lists stage"
grep -q 'cli' "$tmp/help" && fail "help names the CLI"
# no command reaches the TUI binary
git carve stage a.c n1 2>"$tmp/err" && fail "git carve stage ran"
grep -q "unknown argument 'stage'" "$tmp/err" || fail "message: $(cat "$tmp/err")"
# git-carve-cli help lists every command
carve help | grep -q '^   stage ' || fail "cli help lacks stage"
exit 0
