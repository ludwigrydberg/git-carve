#!/bin/sh
# SPDX-License-Identifier: MIT
# fixup makes a fixup! commit of what is staged, for a later squash. fixup
# --amend makes an amend! commit carrying a new message, and what is staged
# if anything, written the way git 2.32's --fixup=amend: does. Nothing else
# in the series changes. A commit not below HEAD, an empty index for fixup!
# and bad options are refused.
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'a\n' >a.c
commit_all "drv: add a"
git branch -q up
git branch -q --set-upstream-to=up
printf 'b\n' >b.c
commit_all "drv: add b"
printf 'c\n' >c.c
commit_all "drv: add c"
c2=$(git rev-parse HEAD~1)

# with nothing staged there is no fixup! commit
carve fixup "$c2" 2>"$tmp/err" && fail "fixup with nothing staged"
grep -q "nothing staged" "$tmp/err" || fail "message: $(cat "$tmp/err")"

# fixup! takes the staged change and the subject of the target, and the
# unstaged change stays where it is
printf 'b fixed\n' >b.c
printf 'c work\n' >c.c
git add b.c
carve fixup "$c2" >"$tmp/out" || fail "fixup"
grep -q "made a fixup! commit" "$tmp/out" || fail "output: $(cat "$tmp/out")"
[ "$(git log -1 --format=%s)" = "fixup! drv: add b" ] ||
	fail "subject: $(git log -1 --format=%s)"
[ "$(git show HEAD:b.c)" = "b fixed" ] || fail "content"
[ "$(git log -1 --format=%s HEAD~1)" = "drv: add c" ] || fail "series changed"
[ "$(git rev-parse HEAD~2)" = "$c2" ] || fail "series rewritten"
[ "$(git diff --name-only)" = c.c ] || fail "unstaged change lost"

# amend!: a new message, nothing staged
carve fixup --amend -m "drv: add b, the better way

Because." "$c2" || fail "fixup --amend -m"
git log -1 --format=%B >"$tmp/msg"
printf 'amend! drv: add b\n\ndrv: add b, the better way\n\nBecause.\n\n' \
	>"$tmp/want"
cmp -s "$tmp/msg" "$tmp/want" || fail "amend! message: $(cat "$tmp/msg")"
[ -z "$(git diff HEAD~1 HEAD --name-only)" ] || fail "amend! not empty"

# amend! in the editor starts from the target's message (EDITOR=true keeps
# it), and a line starting with # (#define) stays
git commit -q --allow-empty -m "drv: add d

#define D 1"
d=$(git rev-parse HEAD)
carve fixup --amend "$d" || fail "fixup --amend (editor)"
[ "$(git log -1 --format=%s)" = "amend! drv: add d" ] ||
	fail "editor subject: $(git log -1 --format=%s)"
git log -1 --format=%B | grep -q '^#define D 1$' || fail "#define lost"

# from a file, with a staged change
printf 'c fixed\n' >c.c
git add c.c
printf 'drv: add c properly\n' >"$tmp/m"
carve fixup --amend -F "$tmp/m" HEAD~4 || fail "fixup --amend -F"
[ "$(git log -1 --format=%s)" = "amend! drv: add c" ] || fail "-F subject"
[ "$(git show HEAD:c.c)" = "c fixed" ] || fail "-F content"

# undo takes the last one back, and its change is staged again
before=$(git rev-parse HEAD~1)
carve undo >/dev/null || fail "undo"
[ "$(git rev-parse HEAD)" = "$before" ] || fail "undo: HEAD"
[ "$(git diff --cached --name-only)" = c.c ] || fail "undo: index"

# refused when not below HEAD, for -m without --amend, and without a commit
other=$(git commit-tree -p up -m elsewhere "$(git rev-parse up^{tree})")
carve fixup --amend -m x "$other" 2>/dev/null && fail "fixup of another branch"
carve fixup -m x HEAD 2>/dev/null && fail "-m without --amend"
carve fixup 2>/dev/null && fail "no commit"
exit 0
