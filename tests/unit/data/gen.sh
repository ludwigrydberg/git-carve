#!/bin/sh
# SPDX-License-Identifier: MIT
# Regenerate the golden diff inputs (*.diff) from real git. The expected
# parse results (*.out) are reviewed by hand; regenerate them with
#   UNIT_UPDATE=1 tests/unit/test_diff
# and inspect the difference before committing.
set -e
out=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/carve-gen.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
export HOME=$tmp GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=$tmp/.gitconfig
export GIT_AUTHOR_NAME=T GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=T
export GIT_COMMITTER_EMAIL=t@t GIT_AUTHOR_DATE='2020-01-01T00:00:00Z'
export GIT_COMMITTER_DATE=$GIT_AUTHOR_DATE
git init -q "$tmp/r"
cd "$tmp/r"

d() {
	git -c core.quotePath=false -c diff.noprefix=false \
		-c diff.mnemonicPrefix=false diff --no-color --no-ext-diff \
		--no-textconv --src-prefix=a/ --dst-prefix=b/ "$@"
}
reset() {
	git rm -rq --cached . >/dev/null 2>&1 || true
	rm -rf ./*
}
base() {
	# base <file> <content...>: commit a starting state
	f=$1
	shift
	printf "$@" >"$f"
	git add -A
	git commit -qm base --allow-empty
}

seq_lines() {
	i=$1
	while [ "$i" -le "$2" ]; do
		echo "line $i"
		i=$((i + 1))
	done
}

reset; seq_lines 1 20 >f.c; git add -A; git commit -qm b --allow-empty
seq_lines 1 20 | awk '$0 == "line 3" { print "line three"; next }
	$0 == "line 15" { next }
	{ print }
	$0 == "line 18" { print "line 18b" }' >f.c
d -U3 -- f.c >"$out/modify.diff"
d -U0 -- f.c >"$out/u0.diff"

reset; git commit -qm e --allow-empty
printf 'a\nb\nc\n' >new.c; git add new.c
d --cached -- new.c >"$out/new-file.diff"

reset; base del.c 'x\ny\n'; git rm -q del.c
d --cached -- del.c >"$out/deleted.diff"

reset; seq_lines 1 30 >old.c; git add -A; git commit -qm b --allow-empty
git mv old.c renamed.c
d --cached -M >"$out/rename-pure.diff"
reset; seq_lines 1 30 >old.c; git add -A; git commit -qm b --allow-empty
git mv old.c renamed.c; seq_lines 1 31 >renamed.c; git add renamed.c
d --cached -M >"$out/rename-modified.diff"

reset; base m.sh 'echo hi\n'; chmod +x m.sh
d -- m.sh >"$out/mode-change.diff"
printf 'echo hi\necho there\n' >m.sh
d -- m.sh >"$out/mode-and-content.diff"

reset; base bin.dat 'a\000b'; printf 'a\000c' >bin.dat
d -- bin.dat >"$out/binary.diff"

reset; base eol.c 'a\nb'; printf 'a\nb\n' >eol.c
d -- eol.c >"$out/noeol-old.diff"
reset; base eol.c 'a\nb\n'; printf 'a\nb' >eol.c
d -- eol.c >"$out/noeol-new.diff"
reset; base eol.c 'a\nb\nc'; printf 'a\nb\nC' >eol.c
d -- eol.c >"$out/noeol-both.diff"
reset; base eol.c 'a\nb\nc'; printf 'A\nb\nc' >eol.c
d -- eol.c >"$out/noeol-context.diff"

reset; git commit -qm e --allow-empty
: >empty.c; git add empty.c
d --cached -- empty.c >"$out/empty-file.diff"

reset; base crlf.c 'one\r\ntwo\r\nthree\r\n'
printf 'one\r\nTWO\r\nthree\r\n' >crlf.c
d -- crlf.c >"$out/crlf.diff"

reset; mkdir -p 'dir with space'; base 'dir with space/fil é.c' 'x\n'
printf 'y\n' >'dir with space/fil é.c'
d -- 'dir with space' >"$out/space-utf8.diff"

reset; base "$(printf 'tab\there.c')" 'x\n'
printf 'y\n' >"$(printf 'tab\there.c')"
d -- "$(printf 'tab\there.c')" >"$out/quoted.diff"

reset; base add.c 'a\nb\n'; printf 'a\nX\nY\nb\n' >add.c
d -- add.c >"$out/only-add.diff"
reset; base rm.c 'a\nX\nY\nb\n'; printf 'a\nb\n' >rm.c
d -- rm.c >"$out/only-del.diff"

reset; base blank.c 'a\n\nb\nc\n'; printf 'a\n\nb\nC\n' >blank.c
git -c diff.suppressBlankEmpty=true -c core.quotePath=false diff \
	--no-color --no-ext-diff --src-prefix=a/ --dst-prefix=b/ -- blank.c \
	>"$out/suppress-blank.diff"

reset; base a.c 'a\n'; base b.c 'b\n'; printf 'A\n' >a.c; printf 'B\n' >b.c
d >"$out/multi-file.diff"
