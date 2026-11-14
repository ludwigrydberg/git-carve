#!/bin/sh
# SPDX-License-Identifier: MIT
# git carve diff, annotated output with old and new numbers, --cached,
# untracked files, subdirectories and paths with spaces.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'a\nb\nc\n' >f.c
mkdir -p 'sub dir'
printf 'x\n' >'sub dir/g h.c'
commit_all base

printf 'a\nB\nc\nd\n' >f.c
carve diff f.c >"$tmp/out" || fail "diff"
cat >"$tmp/exp" <<'X'
@@ -1,3 +1,4 @@
    1     1  a
    2       -b
          2 +B
    3     3  c
          4 +d
X
tail -n 6 "$tmp/out" | diff -u "$tmp/exp" - || fail "annotated diff"
head -n 1 "$tmp/out" | grep -q '^diff --git a/f.c b/f.c$' || fail "header"

# nothing staged yet
[ -z "$(carve diff --cached f.c)" ] || fail "cached should be empty"
git add f.c
carve diff --cached f.c | grep -q '^          4 +d$' || fail "cached diff"
[ -z "$(carve diff f.c)" ] || fail "unstaged should be empty now"

# untracked file shows as a new file
printf 'n1\nn2\n' >new.c
carve diff new.c | grep -q '^          2 +n2$' || fail "untracked"

# relative to a subdirectory, with spaces
printf 'y\n' >'sub dir/g h.c'
(cd 'sub dir' && carve diff 'g h.c') | grep -q '^          1 +y$' ||
	fail "subdir path"
(cd 'sub dir' && carve diff '../f.c') >/dev/null || fail "dotdot path"

# binary and mode-only notes
printf 'a\000b' >bin
commit_all bin
printf 'a\000c' >bin
carve diff bin | grep -q '^# binary file' || fail "binary note"

# absolute paths, also through a symlinked directory (/tmp on macOS)
carve diff "$tmp/repo/f.c" >/dev/null || fail "absolute path"
ln -s "$tmp/repo" "$tmp/link"
carve diff "$tmp/link/f.c" >/dev/null || fail "path through a symlink"

must_fail carve diff
must_fail carve diff /nonexistent/outside
must_fail carve diff no-such-file

# after git rm the deletion is staged, nothing is unstaged, and there is no
# error
printf 'gone\n' >gone.c
git add gone.c && git commit -q -m gone
git rm -q gone.c
carve diff gone.c >"$tmp/out" || fail "diff of a staged deletion"
[ -s "$tmp/out" ] && fail "unstaged diff of a staged deletion: $(cat "$tmp/out")"
carve diff --cached gone.c | grep -q '^deleted file' || fail "--cached"
