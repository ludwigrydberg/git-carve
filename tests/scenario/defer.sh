#!/bin/sh
# SPDX-License-Identifier: MIT
# Pull part of an older commit out into a new commit right
# after it (or after a later commit). The new commit's tree equals the
# original commit's tree, and the commits above keep their trees (I7).
. "$(dirname "$0")/../lib.sh"
new_repo

printf 'int a;\n' >a.c
printf 'int b;\n' >b.c
commit_all C1
printf 'int a;\nint a2; /* belongs later */\n' >a.c
printf 'int b;\nint b2;\n' >b.c
printf 'keep\n' >k.c
commit_all "C2: add a2 and b2"
c2_tree=$(tree_of HEAD)
for i in 3 4; do
	printf 'c%s\n' "$i" >"c$i.c"
	commit_all "C$i"
done
tip=$(git rev-parse HEAD)
old_above=$(git log --format=%T HEAD~2..HEAD)

# the numbers come from the commit's own diff
carve show HEAD~2 a.c | grep -q '^          2 +int a2; /\* belongs later \*/$' ||
	fail "show: $(carve show HEAD~2 a.c)"
carve defer HEAD~2 a.c n2 -m "a: add a2" >"$tmp/out" || fail "defer"
[ "$(git rev-list --count HEAD)" = 5 ] || fail "count"
[ "$(git log --format=%s HEAD~4..HEAD~2 | tr '\n' '|')" = "a: add a2|C2: add a2 and b2|" ] ||
	fail "order: $(git log --oneline)"
git show HEAD~3:a.c | grep -q a2 && fail "a2 still in C2"
git show HEAD~3:b.c | grep -q b2 || fail "b2 lost from C2"
[ "$(tree_of HEAD~2)" = "$c2_tree" ] || fail "new commit tree != C2 tree"
[ "$(git log --format=%T HEAD~2..HEAD)" = "$old_above" ] || fail "I7"
[ "$(tree_of HEAD)" = "$(tree_of "$tip")" ] || fail "tip tree"
[ -z "$(short_status)" ] || fail "status"
carve status | grep -q 'nothing in progress' || fail "state left"

# defer past a later commit, after C4 (now HEAD~0 is C4 and HEAD~3 is C2)
tip=$(git rev-parse HEAD)
carve defer HEAD~3 b.c n2 --after HEAD -m "b: add b2" >/dev/null ||
	fail "defer --after"
[ "$(git log -1 --format=%s)" = "b: add b2" ] || fail "not after C4"
git show HEAD~4:b.c | grep -q b2 && fail "b2 still in C2"
git show HEAD~1:b.c | grep -q b2 && fail "b2 in C4"
[ "$(tree_of HEAD)" = "$(tree_of "$tip")" ] || fail "tip tree (--after)"
[ -z "$(short_status)" ] || fail "status (--after)"

# refusals
must_fail carve defer HEAD~1 nothere.c n1
printf 'x\n' >>a.c
must_fail carve defer HEAD~1 c4.c n1
grep -q 'commit or stash' "$tmp/must_fail.out" || fail "dirty message"
git checkout -q a.c
# taking the whole commit leaves nothing
printf 'int a;\nint a3;\n' >a.c
commit_all "C5: a3"
must_fail carve defer HEAD a.c o2,n2
grep -q 'nothing would be left' "$tmp/must_fail.out" ||
	fail "whole commit: $(cat "$tmp/must_fail.out")"
[ "$(git rev-parse HEAD)" = "$(git rev-parse main)" ] || fail "moved"
carve status | grep -q 'nothing in progress' || fail "state after refusal"

# the new commit's message editor gives up (EDITOR=true): resumable
printf 'int a;\nint a3;\nint a6;\n' >a.c
printf 'int b;\nint b2;\nint b6;\n' >b.c
commit_all "C6: a6 and b6"
printf 'z\n' >z.c
commit_all C7
tip=$(git rev-parse HEAD)
n=$(carve show HEAD~1 b.c | awk '/b6/ { print substr($0, 7, 5) + 0 }')
must_fail carve defer HEAD~1 b.c "n$n"
grep -q "'git-carve-cli continue'" "$tmp/must_fail.out" || fail "resume hint"
carve status | grep -q '^DEFER' || fail "status: $(carve status)"
must_fail carve continue
carve abort >/dev/null || fail "abort defer"
[ "$(git rev-parse HEAD)" = "$tip" ] || fail "abort HEAD"
[ "$(git symbolic-ref HEAD)" = refs/heads/main ] || fail "abort branch"
[ -z "$(short_status)" ] || fail "abort status: $(short_status)"

# and continue works once a message is possible
must_fail carve defer HEAD~1 b.c "n$n"
printf '#!/bin/sh\necho "b: resumed" >"$1"\n' >"$tmp/ed"
chmod +x "$tmp/ed"
env GIT_EDITOR="$tmp/ed" "$CARVE_CLI" continue >/dev/null || fail "continue defer"
[ "$(git log -1 --format=%s HEAD~1)" = "b: resumed" ] || fail "resumed commit"
[ "$(tree_of HEAD)" = "$(tree_of "$tip")" ] || fail "tip after resume"

# the target is HEAD and the message editor gives up, and abort restores
# HEAD
tip=$(git rev-parse HEAD)
printf 'z\nz2\n' >z.c
printf 'y\n' >y.c
commit_all "C8: z2 and y"
tip=$(git rev-parse HEAD)
must_fail carve defer HEAD z.c n2
carve status | grep -q '^DEFER' || fail "status (HEAD)"
carve abort >/dev/null || fail "abort (HEAD)"
[ "$(git rev-parse HEAD)" = "$tip" ] || fail "abort HEAD (HEAD)"
[ -z "$(short_status)" ] || fail "abort status (HEAD)"
carve defer HEAD z.c n2 -m "z: add z2" >/dev/null || fail "defer HEAD"
[ "$(git log -1 --format=%s)" = "z: add z2" ] || fail "defer HEAD subject"
[ "$(tree_of HEAD)" = "$(tree_of "$tip")" ] || fail "defer HEAD tree"
exit 0
