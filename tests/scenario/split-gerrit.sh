#!/bin/sh
# SPDX-License-Identifier: MIT
# Gerrit. A Change-Id trailer stays in only one commit of a split
# (carve.keepChangeId = first, last or none). The other templates leave it
# out, and the commit-msg hook can generate new ones.
. "$(dirname "$0")/../lib.sh"
new_repo

# a fake editor saves the template, then writes a subject above it
cat >"$tmp/editor" <<X
#!/bin/sh
n=\$(ls "$tmp"/tmpl.* 2>/dev/null | wc -l | tr -d ' ')
cp "\$1" "$tmp/tmpl.\$n"
if [ -n "\$SUBJECT" ]; then
	{ echo "\$SUBJECT"; cat "\$1"; } >"\$1.new" && mv "\$1.new" "\$1"
fi
X
chmod +x "$tmp/editor"
export GIT_EDITOR="$tmp/editor"

setup() {
	rm -f "$tmp"/tmpl.*
	git checkout -q --orphan "b$1" 2>/dev/null
	git rm -rq --cached . >/dev/null 2>&1
	rm -f ./*.c
	printf 'x\n' >base.c
	git add base.c
	git commit -q -m base
	printf 'a\n' >a.c
	printf 'b\n' >b.c
	git add a.c b.c
	git commit -q -F - <<M
gerrit: change two files

Body text.

Change-Id: I0123456789abcdef0123456789abcdef01234567
Signed-off-by: T <t@t>
M
	carve split >/dev/null || fail "split ($1)"
}

msg_of() {
	git log -1 --format=%B "$1"
}

# --- first (default)
setup first
carve stage-file a.c
env SUBJECT= "$CARVE_CLI" commit >/dev/null || fail "commit 1"
msg_of HEAD | grep -q '^Change-Id: I0123456789abcdef' || fail "first lost id"
msg_of HEAD | grep -q '^Signed-off-by: T' || fail "first lost sob"
carve stage-file b.c
env SUBJECT="gerrit: second part" "$CARVE_CLI" commit >/dev/null || fail "commit 2"
grep -q '^Change-Id' "$tmp/tmpl.1" && fail "id in second template"
grep -q '^# Original message of the commit being split:' "$tmp/tmpl.1" ||
	fail "reference missing"
grep -q '^# gerrit: change two files' "$tmp/tmpl.1" || fail "commented msg"
grep -q 'Change-Id' "$tmp/tmpl.1" && fail "id in commented reference"
msg_of HEAD | grep -q 'Change-Id' && fail "second commit has an id"
[ "$(git log -1 --format=%s)" = "gerrit: second part" ] || fail "subject 2"
carve split --finish >/dev/null || fail "finish (first)"

# --- -m on the first commit still gets the Change-Id
setup first-m
carve stage-file a.c
carve commit -m "gerrit: part one" >/dev/null || fail "commit -m"
msg_of HEAD | grep -q '^Change-Id: I0123456789abcdef' || fail "-m lost id"
carve stage-file b.c
carve commit -m "gerrit: part two" >/dev/null || fail "commit -m 2"
msg_of HEAD | grep -q 'Change-Id' && fail "-m second has id"
carve split --finish >/dev/null || fail "finish (first-m)"

# --- last
git config carve.keepChangeId last
setup last
carve stage-file a.c
env SUBJECT= "$CARVE_CLI" commit >/dev/null || fail "commit 1 (last)"
msg_of HEAD | grep -q 'Change-Id' && fail "last: first commit has id"
msg_of HEAD | grep -q '^Body text.' || fail "last: body kept"
carve stage-file b.c
env SUBJECT="gerrit: final part" "$CARVE_CLI" commit >/dev/null || fail "commit 2 (last)"
msg_of HEAD | grep -q '^Change-Id: I0123456789abcdef' || fail "last lost id"
carve split --finish >/dev/null || fail "finish (last)"

# --- none
git config carve.keepChangeId none
setup none
carve stage-file a.c
env SUBJECT= "$CARVE_CLI" commit >/dev/null || fail "commit 1 (none)"
msg_of HEAD | grep -q 'Change-Id' && fail "none: id kept"
msg_of HEAD | grep -q '^Signed-off-by: T' || fail "none: sob lost"
carve stage-file b.c
carve commit -m "gerrit: rest" >/dev/null || fail "commit 2 (none)"
carve split --finish >/dev/null || fail "finish (none)"
exit 0
