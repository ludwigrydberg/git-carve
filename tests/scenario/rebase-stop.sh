#!/bin/sh
# SPDX-License-Identifier: MIT
# Where a rebase of yours stops (edit, break), HEAD is carved and split as
# anywhere else. The rebase is left alone and goes on with git rebase
# --continue, and the tip tree is the same. What needs a rebase of its own,
# an older commit or fixup-to, says to wait for yours.
. "$(dirname "$0")/../lib.sh"
new_repo

i=1; while [ $i -le 40 ]; do echo "l $i"; i=$((i + 1)); done >a.c
commit_all base
edit "l 10" "l 10 P1"
commit_all P1
edit "l 20" "l 20 P2"
edit "l 25" "l 25 P2"
edit "l 30" "l 30 helper"
git commit -q -a -m P2 -m "Signed-off-by: A <a@a>"
edit "l 35" "l 35 P3"
commit_all P3
branch=$(git symbolic-ref --short HEAD)
tip=$(git rev-parse HEAD^{tree})
p1=$(git rev-parse HEAD~2)

# a sequence editor that stops (edit) at the commit with subject $STOP
cat >"$tmp/seq" <<'EOF'
#!/bin/sh
awk -v s="$STOP" '{ t = $0; sub(/^[^ ]+ [^ ]+ /, "", t); sub(/^# /, "", t) }
	$1 == "pick" && t == s { $1 = "edit" } { print }' \
	"$1" >"$1.tmp" && mv "$1.tmp" "$1"
EOF
chmod +x "$tmp/seq"
stop_at() {
	STOP=$1 GIT_SEQUENCE_EDITOR="$tmp/seq" git rebase -q -i HEAD~3 \
		>/dev/null 2>&1
	[ "$(git log -1 --format=%s)" = "$1" ] || fail "not stopped at $1"
}
export STOP

# carve before, the helper into a new commit below P2
stop_at P2
carve move HEAD a.c o30,n30 --new-before HEAD -m "helper first" >"$tmp/out" ||
	fail "carve: $(cat "$tmp/out")"
[ "$(git log -1 --format=%s HEAD~1)" = "helper first" ] || fail "new commit"
git show HEAD~1 | grep -q '^+l 30 helper$' || fail "new commit content"
git show HEAD~1 | grep -q "^-l 30$" || fail "new commit: the old line"
git show HEAD | grep -q '^+l 30 helper$' && fail "helper still in P2"
git log -1 --format=%B HEAD | grep -q '^Signed-off-by: A <a@a>$' ||
	fail "P2 lost its message"
carve undo --list | grep -q "move .*a.c o30,n30" ||
	fail "no undo entry: $(carve undo --list)"
# what needs a rebase of its own waits for yours
must_fail carve split "$p1"
grep -q 'a rebase of yours is in progress' "$tmp/must_fail.out" ||
	fail "split older: $(cat "$tmp/must_fail.out")"
git rebase --continue >/dev/null 2>&1 || fail "rebase --continue"
[ "$(git symbolic-ref --short HEAD)" = "$branch" ] || fail "rebase not done"
[ "$(git rev-parse HEAD^{tree})" = "$tip" ] || fail "tip changed"
[ "$(git log --format=%s | tr '\n' ' ')" = "P3 P2 helper first P1 base " ] ||
	fail "series: $(git log --format=%s)"
carved=$(git rev-parse HEAD)

# split HEAD at a stop, and finishing leaves the rebase to you
stop_at P2
carve split >"$tmp/out" || fail "split: $(cat "$tmp/out")"
grep -q 'In your rebase' "$tmp/out" || fail "start note"
carve split --status | grep -q '(in your rebase' || fail "status"
carve stage a.c o20,n20 && carve commit -m "P2 a" >/dev/null || fail "commit 1"
must_fail git rebase --continue		# unstaged changes: git refuses
carve stage-file a.c && carve commit -m "P2 b" >/dev/null || fail "commit 2"
carve split --finish >"$tmp/out" || fail "finish: $(cat "$tmp/out")"
grep -q 'your rebase is still in progress' "$tmp/out" || fail "finish note"
[ -d "$(git rev-parse --git-path rebase-merge)" ] || fail "rebase gone"
git rebase --continue >/dev/null 2>&1 || fail "continue after split"
[ "$(git rev-parse HEAD^{tree})" = "$tip" ] || fail "tip after split"
[ "$(git log --format=%s | tr "\n" " ")" = "P3 P2 b P2 a helper first P1 base " ] ||
	fail "split series: $(git log --format=%s)"

# abort at the stop restores the commit, and the rebase goes on unchanged
stop_at "P2 b"
stopped=$(git rev-parse HEAD)
carve split >/dev/null || fail "split 2"
carve split --abort >/dev/null || fail "abort"
[ "$(git rev-parse HEAD)" = "$stopped" ] || fail "abort: HEAD"
git rebase --continue >/dev/null 2>&1 || fail "continue after abort"
[ "$(git rev-parse HEAD^{tree})" = "$tip" ] || fail "tip after abort"

# with the rebase aborted under the split, abort refuses and --force only
# forgets
stop_at "P2 b"
carve split >/dev/null || fail "split 3"
git rebase --abort
before=$(git rev-parse HEAD)
must_fail carve split --abort
grep -q 'has moved on' "$tmp/must_fail.out" ||
	fail "moved on: $(cat "$tmp/must_fail.out")"
carve split --abort --force | grep -q 'split forgotten' || fail "forget"
[ "$(git rev-parse HEAD)" = "$before" ] || fail "forget moved HEAD"
[ "$(git rev-parse HEAD^{tree})" = "$tip" ] || fail "forget changed files"
carve split --status | grep -q 'no split in progress' || fail "still split"

# at a break stop, carve after into a new commit on top of the one before
cat >"$tmp/seq-break" <<'EOF'
#!/bin/sh
awk -v s="$STOP" '{ t = $0; sub(/^[^ ]+ [^ ]+ /, "", t); sub(/^# /, "", t); print }
	$1 == "pick" && t == s { print "break" }' "$1" >"$1.tmp" &&
	mv "$1.tmp" "$1"
EOF
chmod +x "$tmp/seq-break"
STOP="P2 a" GIT_SEQUENCE_EDITOR="$tmp/seq-break" git rebase -q -i HEAD~3 \
	>/dev/null 2>&1
[ "$(git log -1 --format=%s)" = "P2 a" ] || fail "no break stop"
carve move HEAD a.c n20 --new-after HEAD -m "P2 a part two" >/dev/null ||
	fail "carve after at a break"
[ "$(git log -1 --format=%s)" = "P2 a part two" ] || fail "after: subject"
git rebase --continue >/dev/null 2>&1 || fail "continue after break"
[ "$(git rev-parse HEAD^{tree})" = "$tip" ] || fail "tip after break"

# after a carve or a move rewrote HEAD at an edit stop, staged changes
# are still amended into it by git rebase --continue (rebase-merge/amend
# follows HEAD)
for how in new out; do
	git reset -q --hard "$carved"
	stop_at P2
	if [ $how = new ]; then
		carve move HEAD a.c o25,n25 --new-before HEAD -m "P2 first" \
			>/dev/null || fail "new commit at the stop"
		edit "l 26" "l 26 at the stop"
	else
		# Space on HEAD's lines takes them out of it into the work
		# tree
		carve move HEAD a.c o25,n25 --out >/dev/null || fail "out"
	fi
	git add a.c
	git rebase --continue >/dev/null 2>"$tmp/err" ||
		fail "$how: continue with staged changes: $(cat "$tmp/err")"
	[ "$(git log -1 --format=%s HEAD~1)" = P2 ] || fail "$how: P2 moved"
	git show HEAD~1 | grep -q '^+l 2[56] ' ||
		fail "$how: not amended into P2: $(git show HEAD~1)"
done
exit 0
