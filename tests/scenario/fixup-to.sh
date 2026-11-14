#!/bin/sh
# SPDX-License-Identifier: MIT
# Move a change into an older commit of the series, with and without a
# conflict in a later commit, then continue or abort.
. "$(dirname "$0")/../lib.sh"
new_repo

lines() {
	i=$1
	while [ "$i" -le "$2" ]; do echo "line $i"; i=$((i + 1)); done
}

build_series() {
	# C1 has 30 lines and C2 a function in the middle. C3 touches the
	# line after the function ($1 = near) or a line far away. C4 and C5
	# change other files.
	{ lines 1 30; } >f.c
	commit_all C1
	{ lines 1 10; echo "int helper(void)"; echo "{"; echo "	retrun 1;";
	  echo "}"; lines 11 30; } >f.c
	commit_all "C2: add helper()"
	if [ "$1" = near ]; then
		awk '{ if ($0 == "}") print "} /* helper */"; else print }' \
			f.c >f.tmp && mv f.tmp f.c
	else
		awk '{ if ($0 == "line 29") print "line 29 changed"; else print }' \
			f.c >f.tmp && mv f.tmp f.c
	fi
	commit_all "C3: touch f.c"
	printf 'four\n' >four.c
	commit_all C4
	printf 'five\n' >five.c
	commit_all C5
}

# --- no conflict ----------------------------------------------------
build_series far
tip=$(git rev-parse HEAD)
# the fix for C2's typo, plus unrelated work that must survive
awk '{ sub(/retrun/, "return"); print }' f.c >f.tmp && mv f.tmp f.c
printf 'five\nwork in progress\n' >five.c

n=$(new_no f.c 'return 1;')
o=$(old_no f.c 'retrun 1;')
carve fixup-to HEAD~3 f.c "o$o,n$n" >"$tmp/out" || fail "fixup-to"
[ "$(git rev-list --count HEAD)" = 5 ] || fail "count"
git show HEAD~3:f.c | grep -q '	return 1;' || fail "fix not in C2"
git show HEAD~3:f.c | grep -q 'retrun' && fail "typo left in C2"
[ "$(git log -1 --format=%s HEAD~3)" = "C2: add helper()" ] || fail "C2 msg"
git log --format=%s | grep -q fixup && fail "fixup commit left"
# unrelated work is still there, unstaged, and nothing is staged
[ "$(cat five.c)" = "$(printf 'five\nwork in progress')" ] ||
	fail "autostash lost work"
[ "$(short_status)" = " M five.c" ] || fail "status: $(short_status)"
# I7, every commit from C2 on equals its original plus exactly the fix
for i in 3 2 1 0; do
	d=$(git diff-tree -p "$tip~$i" "HEAD~$i")
	[ "$(echo "$d" | grep -c '^[-+][^-+]')" = 2 ] &&
		echo "$d" | grep -q '^-	retrun 1;$' &&
		echo "$d" | grep -q '^+	return 1;$' ||
		fail "I7: C$((5 - i)) differs by more than the fix"
done
carve status | grep -q 'nothing in progress' || fail "status after"
[ -d .git/carve ] && fail "state left"

# --- the target is HEAD, a plain amend -----------------------------------
printf 'five\nfinal\n' >five.c
carve fixup-to HEAD five.c n2 >/dev/null || fail "fixup-to HEAD"
[ "$(git rev-list --count HEAD)" = 5 ] || fail "amend count"
git show HEAD:five.c | grep -q final || fail "amend content"

# --- conflict, resolved and continued -------------------------------------
new_repo conflict
build_series near
tip=$(git rev-parse HEAD)
awk '{ sub(/retrun/, "return"); print }' f.c >f.tmp && mv f.tmp f.c
n=$(new_no f.c 'return 1;')
o=$(old_no f.c 'retrun 1;')
must_fail carve fixup-to HEAD~3 f.c "o$o,n$n"
grep -q 'conflicted file' "$tmp/must_fail.out" ||
	fail "conflict message: $(cat "$tmp/must_fail.out")"
carve status | grep -q '^FIXUP-TO .* "C2: add helper()" .* 1 conflicted file' ||
	fail "status: $(carve status)"
must_fail carve continue
grep -q 'resolve' "$tmp/must_fail.out" || fail "continue before resolving"
# resolve with C3's change on top of the fixed line
{ lines 1 10; echo "int helper(void)"; echo "{"; echo "	return 1;";
  echo "} /* helper */"; lines 11 30; } >f.c
carve stage-file f.c || fail "stage resolution"
carve continue >/dev/null || fail "continue: $(carve status)"
[ "$(git rev-list --count HEAD)" = 5 ] || fail "count after continue"
git show HEAD~3:f.c | grep -q '	return 1;' || fail "fix not in C2 (conflict)"
git show HEAD~2:f.c | grep -q '} /\* helper \*/' || fail "C3 change lost"
[ -z "$(short_status)" ] || fail "status after continue: $(short_status)"
carve status | grep -q 'nothing in progress' || fail "state after continue"

# --- conflict, aborted -----------------------------------------------------
git reset -q --hard "$tip"
awk '{ sub(/retrun/, "return"); print }' f.c >f.tmp && mv f.tmp f.c
cp f.c "$tmp/fixed"
must_fail carve fixup-to HEAD~3 f.c "o$o,n$n"
carve abort >/dev/null || fail "abort"
[ "$(git rev-parse HEAD)" = "$tip" ] || fail "abort HEAD"
[ "$(git symbolic-ref HEAD)" = refs/heads/main ] || fail "abort branch"
[ "$(short_status)" = " M f.c" ] || fail "abort status: $(short_status)"
cmp -s f.c "$tmp/fixed" || fail "abort lost the change"
[ -d .git/carve ] && fail "state left after abort"

# --- a line a later commit added cannot go into an older commit ---------
git reset -q --hard "$tip"
awk '$0 != "line 5"' f.c >f.tmp && mv f.tmp f.c
printf 'four\nadded by C4\n' >four.c
commit_all "C6: extend four"
tip=$(git rev-parse HEAD)
# in the work tree, drop the line C6 added and keep an unrelated edit
printf 'four\n' >four.c
printf 'line 1\n' >>f.c
cp four.c "$tmp/four"; cp f.c "$tmp/f"
must_fail carve fixup-to HEAD~2 four.c o2
grep -q 'cannot move the change' "$tmp/must_fail.out" ||
	fail "refusal: $(cat "$tmp/must_fail.out")"
[ "$(git rev-parse HEAD)" = "$tip" ] || fail "refusal moved HEAD"
cmp -s four.c "$tmp/four" && cmp -s f.c "$tmp/f" || fail "refusal: work tree"
[ -z "$(git diff --cached --name-only)" ] || fail "refusal: index"
carve status | grep -q 'nothing in progress' || fail "refusal: state"
git checkout -q -- .

# --- refusals ----------------------------------------------------------------
printf 'line 1\n' >>f.c
carve stage-file f.c
printf 'line 2\n' >>f.c
must_fail carve fixup-to HEAD~3 f.c n1
grep -q 'already has staged changes' "$tmp/must_fail.out" || fail "staged"

# --- the file was renamed after the target -----------------
new_repo renamed
lines 1 30 >old.c
commit_all C1
git mv old.c new.c
awk '{ sub(/^line 20$/, "line 20 in C2"); print }' new.c >f.tmp &&
	mv f.tmp new.c
commit_all "C2: rename"
printf 'x\n' >x.c
commit_all C3
tip=$(git rev-parse HEAD)
awk '{ sub(/^line 10$/, "line 10 fixed"); print }' new.c >f.tmp &&
	mv f.tmp new.c
carve fixup-to HEAD~2 new.c o10,n10 >/dev/null || fail "fixup across rename"
git show HEAD~2:old.c | grep -q '^line 10 fixed$' || fail "fix not in C1's old.c"
git show --stat -M HEAD~1 | grep -q 'old.c => new.c' || fail "C2 no rename"
[ "$(git diff "$tip" HEAD --stat | tail -1)" = \
  " 1 file changed, 1 insertion(+), 1 deletion(-)" ] || fail "I7 tip"
[ -z "$(short_status)" ] || fail "left: $(short_status)"

# a rename moved into an older commit takes both halves, since C2 then
# renames old.c straight to final.c
mv new.c final.c
carve stage-move new.c final.c && carve fixup-to --cached HEAD~1 >/dev/null ||
	fail "fixup-to of a rename"
git show --stat -M HEAD~1 | grep -q 'old.c => final.c' ||
	fail "C2: $(git show --stat -M HEAD~1 | tail -2)"
[ -z "$(git ls-tree HEAD new.c)" ] || fail "new.c half left"
git show HEAD:final.c | grep -q '^line 10 fixed$' || fail "content"
[ -z "$(short_status)" ] || fail "left: $(short_status)"
exit 0
