#!/bin/sh
# SPDX-License-Identifier: MIT
# Signed-off-by. commit -s adds the committer's sign-off once, also when
# the message already has it before other trailers, where git commit -s
# would add a second one. carve.signoff makes it the default, and amend and
# split templates get it too.
. "$(dirname "$0")/../lib.sh"
new_repo
sob='Signed-off-by: T <t@t>'

# the sign-off lines of HEAD's message
count_sob() {
	git log -1 --format=%B HEAD | grep -c "^$sob$"
}

printf 'a\n' >f.c
commit_all base

printf 'b\n' >f.c && carve stage-file f.c
carve commit -s -m "one" >/dev/null || fail "commit -s"
[ "$(count_sob)" = 1 ] || fail "no sign-off: $(git log -1 --format=%B)"

# already there, before a Change-Id, and not added again
printf 'c\n' >f.c && carve stage-file f.c
printf 'two\n\n%s\nChange-Id: I1234\n' "$sob" >"$tmp/msg"
carve commit -s -F "$tmp/msg" >/dev/null || fail "commit -s -F"
[ "$(count_sob)" = 1 ] || fail "duplicate: $(git log -1 --format=%B)"
git log -1 --format=%B | grep -q '^Change-Id: I1234$' || fail "Change-Id lost"

# someone else's sign-off stays, ours is added after it
printf 'd\n' >f.c && carve stage-file f.c
carve commit -s -m "$(printf 'three\n\nSigned-off-by: X <x@x>')" >/dev/null
[ "$(git log -1 --format=%B | grep -c '^Signed-off-by')" = 2 ] ||
	fail "other sign-off: $(git log -1 --format=%B)"
git log -1 --format=%B | tail -2 | grep -q "^$sob$" || fail "order"

# carve.signoff makes it the default, and --no-signoff overrides it
git config carve.signoff true
printf 'e\n' >f.c && carve stage-file f.c
carve commit -m four >/dev/null
[ "$(count_sob)" = 1 ] || fail "carve.signoff"
printf 'f\n' >f.c && carve stage-file f.c
carve commit --no-signoff -m five >/dev/null
[ "$(count_sob)" = 0 ] || fail "--no-signoff"
must_fail carve commit -s --no-signoff -m x

# amend with the editor gets HEAD's message, signed off once
carve commit --amend >/dev/null || fail "amend"
[ "$(count_sob)" = 1 ] || fail "amend: $(git log -1 --format=%B)"
[ "$(git log -1 --format=%s)" = five ] || fail "amend subject"
carve commit --amend >/dev/null
[ "$(count_sob)" = 1 ] || fail "amend twice"

# the editor with nothing filled in uses git commit -s
cat >"$tmp/editor" <<X
#!/bin/sh
{ echo "from the editor"; cat "\$1"; } >"\$1.new" && mv "\$1.new" "\$1"
X
chmod +x "$tmp/editor"
printf 'g\n' >f.c && carve stage-file f.c
GIT_EDITOR="$tmp/editor" carve commit >/dev/null || fail "editor commit"
[ "$(git log -1 --format=%s)" = "from the editor" ] || fail "editor subject"
[ "$(count_sob)" = 1 ] || fail "editor: $(git log -1 --format=%B)"

# in a split the first commit's message, already signed off, and the later
# template both end up signed off exactly once
printf 'h\nH\n' >f.c && carve stage-file f.c
carve commit -m "$(printf 'big\n\nBody.\n\n%s' "$sob")" >/dev/null
carve split >/dev/null || fail split
carve stage f.c n1,o1 >/dev/null
carve commit >/dev/null || fail "first split commit"
[ "$(count_sob)" = 1 ] || fail "split first: $(git log -1 --format=%B)"
carve stage-file f.c
GIT_EDITOR="$tmp/editor" carve commit >/dev/null || fail "second"
[ "$(count_sob)" = 1 ] || fail "split later: $(git log -1 --format=%B)"
git log -1 --format=%B | grep -q '^#' && fail "comments committed"
carve split --finish >/dev/null || fail finish
exit 0
