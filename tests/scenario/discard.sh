#!/bin/sh
# SPDX-License-Identifier: MIT
# Discard. Selected unstaged lines or a file's unstaged changes leave the
# work tree, and the index is untouched. Undo puts the exact bytes and the
# file mode back, and refuses to overwrite later edits unless forced.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'a\nb\nc\nd\ne\n' >f.c
printf '#!/bin/sh\necho hi\n' >run.sh
chmod +x run.sh
commit_all base

# --- lines ---------------------------------------------------------------
printf 'A\nb\nc\nD\ne\nf' >f.c		# two changes, and no newline at the end
carve stage f.c o1,n1 || fail "stage A"
idx=$(git write-tree)
before=$(git hash-object f.c)
carve discard f.c o4,n4 || fail "discard D"
[ "$(cat f.c)" = "$(printf 'A\nb\nc\nd\ne\nf')" ] || fail "content: $(cat f.c)"
[ "$(git write-tree)" = "$idx" ] || fail "index touched"
carve discard f.c n6 || fail "discard no-eol line"
[ "$(git hash-object f.c)" = "$(printf 'A\nb\nc\nd\ne\n' | git hash-object --stdin)" ] ||
	fail "no-eol: $(od -c f.c)"
carve undo >/dev/null && carve undo >/dev/null || fail "undo discards"
[ "$(git hash-object f.c)" = "$before" ] || fail "undo: $(cat f.c)"
[ "$(git write-tree)" = "$idx" ] || fail "undo touched the index"

# undo refuses to overwrite an edit made after the discard
carve discard f.c o4,n4 >/dev/null
echo more >>f.c
must_fail carve undo
grep -q 'f.c has changed since' "$tmp/must_fail.out" ||
	fail "refusal: $(cat "$tmp/must_fail.out")"
carve undo --force >/dev/null || fail "undo --force"
[ "$(git hash-object f.c)" = "$before" ] || fail "forced undo"

must_fail carve discard f.c n99
[ "$(git hash-object f.c)" = "$before" ] || fail "failed discard changed f.c"

# --- whole files -------------------------------------------------------------
carve discard-file f.c || fail "discard-file"
[ "$(git hash-object f.c)" = "$(git rev-parse :f.c)" ] ||
	fail "not the index version"
carve undo >/dev/null && [ "$(git hash-object f.c)" = "$before" ] ||
	fail "undo discard-file"

# an untracked file is deleted, and comes back executable
printf 'new\n' >new.sh
chmod +x new.sh
carve discard-file new.sh || fail "discard untracked"
[ -e new.sh ] && fail "untracked file not deleted"
carve undo >/dev/null || fail "undo untracked"
[ "$(cat new.sh)" = new ] && [ -x new.sh ] || fail "untracked not restored"
must_fail carve discard new.sh n1
grep -q 'untracked: discard the whole file' "$tmp/must_fail.out" ||
	fail "line discard of untracked"

# a mode change and a deletion
chmod -x run.sh
carve discard-file run.sh && [ -x run.sh ] || fail "mode not restored"
carve undo >/dev/null && [ ! -x run.sh ] || fail "undo mode"
chmod +x run.sh
rm run.sh
carve discard-file run.sh && [ -x run.sh ] || fail "deletion not undone"
carve undo >/dev/null
[ -e run.sh ] && fail "undo of the discarded deletion"

# the saved content survives git gc
git reset -q --hard
printf 'precious\n' >>f.c
carve discard-file f.c
git gc -q --prune=now
carve undo >/dev/null || fail "undo after gc"
tail -1 f.c | grep -q precious || fail "content lost to gc"
exit 0
