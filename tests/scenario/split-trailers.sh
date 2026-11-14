#!/bin/sh
# SPDX-License-Identifier: MIT
# Trailers when splitting. Every part is signed off as the original was,
# one keeps the Change-Id (carve.keepChangeId), and none keeps the review
# tags (Reviewed-by, Acked-by, Tested-by), which were given for the unsplit
# patch. The split says which tags it dropped.
. "$(dirname "$0")/../lib.sh"
new_repo

# a fake editor puts a subject above the template of later parts
cat >"$tmp/editor" <<X
#!/bin/sh
if [ -n "\$SUBJECT" ]; then
	{ echo "\$SUBJECT"; cat "\$1"; } >"\$1.new" && mv "\$1.new" "\$1"
fi
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

printf 'x\n' >base.c
commit_all base
printf 'a\n' >a.c
printf 'b\n' >b.c
printf 'c\n' >c.c
git add a.c b.c c.c
git commit -q -F - <<M
drv: three things

Body text.

Change-Id: I0123456789abcdef0123456789abcdef01234567
Signed-off-by: Jane Hacker <jane@example.org>
Reviewed-by: Rev Iewer <rev@example.org>
Acked-by: Sam Maintainer <sam@example.org>
Tested-by: Tess Ter <tess@example.org>
Signed-off-by: Sam Maintainer <sam@example.org>
M

carve split >/dev/null 2>"$tmp/err" || fail "split: $(cat "$tmp/err")"
grep -q 'note: Reviewed-by, Acked-by, Tested-by not carried to the new commits' \
	"$tmp/err" || fail "no note: $(cat "$tmp/err")"
[ "$(grep -c . "$tmp/err")" = 1 ] || fail "one line: $(cat "$tmp/err")"

# part one from the template, two from theirs, three with -m
carve stage-file a.c
env SUBJECT= "$CARVE_CLI" commit >/dev/null || fail "commit 1"
carve stage-file b.c
env SUBJECT="drv: second thing" "$CARVE_CLI" commit >/dev/null || fail "commit 2"
carve stage-file c.c
carve commit -m "drv: third thing" >/dev/null || fail "commit 3"
carve split --finish >/dev/null || fail "finish"

for c in HEAD~2 HEAD~1 HEAD; do
	m=$(git log -1 --format=%B "$c")
	echo "$m" | grep -q '^Signed-off-by: Jane Hacker <jane@example.org>$' ||
		fail "$c: first sign-off missing: $m"
	echo "$m" | grep -q '^Signed-off-by: Sam Maintainer <sam@example.org>$' ||
		fail "$c: second sign-off missing: $m"
	[ "$(echo "$m" | grep -c '^Signed-off-by:')" = 2 ] || fail "$c: twice: $m"
	echo "$m" | grep -q -i -E '^(Reviewed|Acked|Tested)-by:' &&
		fail "$c: a review tag: $m"
done
[ "$(git log -3 --format=%B | grep -c '^Change-Id:')" = 1 ] || fail "ids"
git log -1 --format=%B HEAD~2 | grep -q '^Change-Id: I0123' || fail "not first"
git log -1 --format=%B HEAD~2 | grep -q '^Body text.$' || fail "body"
[ "$(git log -1 --format=%s HEAD~1)" = "drv: second thing" ] || fail "subject 2"

# with carve.keepChangeId last the last part has it, and all are signed off
git reset -q --hard HEAD~3
printf 'a\n' >a.c
printf 'b\n' >b.c
git add a.c b.c
git commit -q -F - <<M
drv: two things

Change-Id: I0123456789abcdef0123456789abcdef01234567
Signed-off-by: Jane Hacker <jane@example.org>
M
git config carve.keepChangeId last
carve split >/dev/null 2>"$tmp/err" || fail "split 2"
[ -s "$tmp/err" ] && fail "a note without review tags: $(cat "$tmp/err")"
carve stage-file a.c
carve commit -m "drv: first" >/dev/null || fail "commit a"
carve stage-file b.c
carve commit -m "drv: last" >/dev/null || fail "commit b"
carve split --finish >/dev/null || fail "finish 2"
git log -1 --format=%B HEAD | grep -q '^Change-Id: I0123' || fail "last: id"
git log -1 --format=%B HEAD~1 | grep -q '^Change-Id' && fail "last: first has id"
[ "$(git log -2 --format=%B | grep -c '^Signed-off-by: Jane')" = 2 ] ||
	fail "last: sign-offs"
exit 0
