#!/bin/sh
# SPDX-License-Identifier: MIT
# Commit prefixes. The subsystem prefix the history of the staged files
# uses most is suggested, and the commit editor starts with it, also for
# the later commits of a split. carve.suggestPrefix=false turns it off.
. "$(dirname "$0")/../lib.sh"
new_repo

# a file whose only commit is from 2015, where assignments before a
# function call would stay set and are saved and put back
saved=$GIT_COMMITTER_DATE
GIT_AUTHOR_DATE=2015-01-01T00:00:00Z GIT_COMMITTER_DATE=2015-01-01T00:00:00Z
printf 'old\n' >old.c
commit_all "ancient: add old"
GIT_AUTHOR_DATE=$saved GIT_COMMITTER_DATE=$saved

mkdir -p drivers/net
printf '1\n' >drivers/net/phy.c
printf 'x\n' >other.c
commit_all "initial import"
for s in "net: phy: add a" "net: phy: fix b" 'Revert "net: phy: fix b"' \
	 "net: add c"; do
	echo "$s" >>drivers/net/phy.c
	commit_all "$s"
done
echo y >>other.c
commit_all "misc: other"

must_fail carve prefix
grep -q 'no prefix to suggest' "$tmp/must_fail.out" || fail "nothing staged"
echo 5 >>drivers/net/phy.c
carve stage-file drivers/net/phy.c
[ "$(carve prefix)" = "net: phy: " ] || fail "prefix: $(carve prefix)"

cat >"$tmp/editor" <<X
#!/bin/sh
cp "\$1" "$tmp/template"
awk 'NR == 1 { \$0 = \$0 "do it" } { print }' "\$1" >"\$1.new" &&
	mv "\$1.new" "\$1"
X
chmod +x "$tmp/editor"
GIT_EDITOR="$tmp/editor" carve commit -s >/dev/null || fail commit
[ "$(head -1 "$tmp/template")" = "net: phy: " ] || fail "template"
[ "$(git log -1 --format=%s)" = "net: phy: do it" ] || fail "subject"
git log -1 --format=%B | grep -q '^Signed-off-by: T <t@t>$' || fail "signoff"
[ -d .git/carve ] && fail "state left"

# a line of the message that starts with the comment character (a #define)
# stays, and only the scissors line and below go
cat >"$tmp/editor2" <<'X'
#!/bin/sh
awk 'NR == 1 { $0 = $0 "define it" } { print } NR == 1 { print ""; print "#define PHY_X 1 is new" }' \
	"$1" >"$1.new" && mv "$1.new" "$1"
X
chmod +x "$tmp/editor2"
for cc in "" auto ";"; do
	[ -n "$cc" ] && git config core.commentChar "$cc"
	echo "8$cc" >>drivers/net/phy.c && carve stage-file drivers/net/phy.c
	GIT_EDITOR="$tmp/editor2" carve commit >/dev/null || fail "commit $cc"
	git log -1 --format=%B | grep -q '^#define PHY_X 1 is new$' ||
		fail "#define gone ($cc): $(git log -1 --format=%B)"
	git log -1 --format=%B | grep -q 'above' && fail "scissors kept ($cc)"
done
git config --unset core.commentChar

# an unedited template aborts, as with git
echo 6 >>drivers/net/phy.c && carve stage-file drivers/net/phy.c
GIT_EDITOR=true carve commit >/dev/null 2>&1 && fail "unedited committed"

# the later commits of a split
echo z >>other.c && carve stage-file other.c
carve commit -m "net: phy: two things" >/dev/null
carve split >/dev/null
carve stage-file drivers/net/phy.c && carve commit >/dev/null 2>&1
carve stage-file other.c
GIT_EDITOR="$tmp/editor" carve commit >/dev/null || fail "split commit"
head -1 "$tmp/template" | grep -q '^misc: $' || fail "split: $(head -1 "$tmp/template")"
carve split --finish >/dev/null || fail finish

git config carve.suggestPrefix false
echo 7 >>drivers/net/phy.c && carve stage-file drivers/net/phy.c
must_fail carve prefix
git config --unset carve.suggestPrefix
git checkout -q drivers/net/phy.c
git reset -q
# only the two years before HEAD's date vote, and old.c's prefix from 2015,
# the start of the history, is not suggested
echo more >>old.c
carve stage-file old.c
must_fail carve prefix
grep -q 'no prefix to suggest' "$tmp/must_fail.out" || fail "old history voted"
carve unstage-file old.c
# a new file does not vote, the staged file with history still does
printf 'n\n' >brand-new.c
echo 9 >>drivers/net/phy.c
carve stage-file brand-new.c drivers/net/phy.c
[ "$(carve prefix)" = "net: phy: " ] || fail "with a new file: $(carve prefix)"
carve unstage-file brand-new.c drivers/net/phy.c
# above 500 staged files there is none
mkdir many
i=0
while [ $i -lt 501 ]; do echo x >"many/m$i.c"; i=$((i + 1)); done
commit_all "many: add"
i=0
while [ $i -lt 501 ]; do echo y >>"many/m$i.c"; i=$((i + 1)); done
git add many
must_fail carve prefix
grep -q 'no prefix to suggest' "$tmp/must_fail.out" || fail "501 files voted"
exit 0
