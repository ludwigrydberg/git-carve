#!/bin/sh
# SPDX-License-Identifier: MIT
# With more than 200 changed files the commit editor gets no git status,
# which on a kernel tree is seconds of git and a buffer of thousands of
# lines, and the Files pane shows it all. With fewer it gets it, as git
# gives it.
. "$(dirname "$0")/../lib-tmux.sh"

cat_editor() {
	cat >"$tmp/editor" <<X
#!/bin/sh
cp "\$1" "$tmp/seen"
printf 'big: one\n' >"\$1"
X
	chmod +x "$tmp/editor"
	TUI_ENV="GIT_EDITOR=$tmp/editor"
}

# with few files, git's status is in the editor
new_repo few
printf 'a\n' >a.c
commit_all base
printf 'b\n' >a.c
cat_editor
tui_start
wait_screen "M a.c"
keys Enter
wait_screen "Staged (1)"
keys c
wait_screen "committed"
grep -q 'Changes to be committed' "$tmp/seen" || fail "few: no status: $(cat "$tmp/seen")"
tui_quit

# with 210 files, none
new_repo many
i=0
while [ $i -lt 210 ]; do printf 'a\n' >"f$i.c"; i=$((i + 1)); done
commit_all base
i=0
while [ $i -lt 210 ]; do printf 'b\n' >"f$i.c"; i=$((i + 1)); done
cat_editor
tui_start
wait_screen "Unstaged (210)"
keys Enter
wait_screen "Staged (1)"
keys c
wait_screen "committed"
grep -q 'Changes to be committed\|Changes not staged' "$tmp/seen" &&
	fail "many: status in the editor: $(head -20 "$tmp/seen")"
[ "$(git log -1 --format=%s)" = "big: one" ] || fail "commit"
tui_quit

# with more than 3,000 changed files the list after the commit follows from
# the one before, without a second git status, and is right. So does the
# list after a split of HEAD, which follows from the commit's files.
new_repo huge
i=0
while [ $i -lt 3100 ]; do printf 'a\n' >"g$i.c"; i=$((i + 1)); done
printf 'original\n' >late.c
commit_all base
i=0
while [ $i -lt 3100 ]; do printf 'b\n' >"g$i.c"; i=$((i + 1)); done
printf 'n\n' >new.c
cat_editor
: >"$tmp/trace"
TUI_ENV="$TUI_ENV GIT_CARVE_TRACE=$tmp/trace" tui_start
wait_screen "Unstaged (3100)" 300
wait_gone "scanning untracked" 300
keys Enter
wait_screen "Staged (1)"
: >"$tmp/trace"
keys c
wait_screen "committed"
wait_screen "Unstaged (3099)"
screen | grep -q 'Staged (' && fail "staged side left: $(screen)"
grep -q ' status ' "$tmp/trace" && fail "a status after the commit: $(cat "$tmp/trace")"
[ "$(git diff --name-only | wc -l | tr -d ' ')" = 3099 ] || fail "git"
# the untracked file is still listed
keys G
wait_screen "Untracked (1)"
# A file edited while the confirmation is open stays unstaged.
keys g g k Enter
wait_screen "Stage 3099 files"
printf 'changed during confirmation\n' >late.c
keys y
wait_screen "staged 3099 files" 300
wait_screen "Unstaged (1)"
keys g g
wait_screen "Staged (3099)"
[ -z "$(git diff --cached --name-only -- late.c)" ] ||
	fail "staged an unconfirmed file"
[ "$(git diff --cached --name-only | wc -l | tr -d ' ')" = 3099 ] ||
	fail "stage all: git"
keys G
wait_screen "Unstaged (1)"
git restore -- late.c
keys F5
wait_gone "Unstaged (1)"
keys c
wait_screen "committed"
screen | grep -q 'Unstaged (' && fail "unstaged left: $(screen)"
[ -z "$(git diff --name-only)" ] || fail "git: $(git diff --name-only | head -3)"
# splitting that commit lists its 3099 files without a git status, and
# the abort writes no file
: >"$tmp/trace"
keys s y
wait_screen "3099 files left" 300
# the clean check before the split is one, but none after its restore
sed -n '/restore -q --staged/,$p' "$tmp/trace" | grep -q ' status ' &&
	fail "a status after the split: $(cat "$tmp/trace")"
[ "$(git diff --name-only | wc -l | tr -d ' ')" = 3099 ] || fail "split: git"
: >"$tmp/trace"
keys X y
wait_screen "aborted" 300
grep -q 'reset -q --hard' "$tmp/trace" && fail "the abort wrote files"
[ -z "$(git status --porcelain --untracked-files=no)" ] || fail "abort: git"
tui_quit
