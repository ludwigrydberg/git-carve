#!/bin/sh
# SPDX-License-Identifier: MIT
# Whole sections and ranges of files. Enter or Space on a section's row
# stages (Unstaged, Untracked) or unstages (Staged) all its files, only
# those the filter shows when one is on. v in Files takes a range. Above
# 100 files it asks first. u takes it back in one step. After a single
# file, the cursor stays in its section on the next file.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

i=0
while [ $i -lt 120 ]; do printf 'a\n' >"f$i.c"; i=$((i + 1)); done
mkdir net
printf 'a\n' >net/one.c
printf 'a\n' >net/two.c
commit_all base
i=0
while [ $i -lt 120 ]; do printf 'b\n' >"f$i.c"; i=$((i + 1)); done
printf 'b\n' >net/one.c
printf 'b\n' >net/two.c
printf 'n\n' >new1.c
printf 'n\n' >new2.c

# the Files pane's cursor row
fcur() {
	screen | cut -c1-31 | grep '^>'
}

tui_start
wait_screen "Unstaged (122)"
wait_gone "scanning untracked"

# one file staged, and the cursor on the next one of Unstaged
keys Enter
wait_screen "staged f0.c"
wait_screen "Staged (1)"
wait_fcur 'f1\.c' || fail "cursor after Enter: $(fcur)"
keys u
wait_gone "Staged (1)"

# with the filter, Enter on the Unstaged row takes the two net/ files only
keys / n e t / Enter
wait_screen "Unstaged (2)"
keys k
wait_fcur 'Unstaged (2)' || fail "k to the section row: $(fcur)"
keys Enter
wait_screen "staged 2 files"
[ "$(git diff --cached --name-only | tr '\n' ' ')" = "net/one.c net/two.c " ] ||
	fail "filtered: $(git diff --cached --name-only)"
keys Escape
wait_gone "/net/"
keys u
wait_screen "undid stage-file"
[ -z "$(git diff --cached --name-only)" ] || fail "u: $(git diff --cached --name-only)"

# all 122, it asks first, and n leaves them
keys g g k
wait_screen "> Files"
wait_fcur 'Unstaged (122)' || fail "on the row: $(fcur)"
keys Enter
wait_screen "Stage 122 files? [y/N]"
keys n
wait_screen "nothing staged"
[ -z "$(git diff --cached --name-only)" ] || fail "n staged"
keys Enter
wait_screen "Stage 122 files? [y/N]"
keys y
wait_screen "staged 122 files"
[ "$(git diff --cached --name-only | wc -l | tr -d ' ')" = 122 ] ||
	fail "all: $(git diff --cached --name-only | wc -l)"
[ -z "$(git diff --name-only)" ] || fail "left: $(git diff --name-only)"
# the untracked files stay untracked
[ -z "$(git diff --cached --name-only -- new1.c)" ] || fail "new1.c staged"
# u goes back in one step
keys u
wait_screen "undid stage-file"
[ -z "$(git diff --cached --name-only)" ] || fail "u after all"

# Space on the Untracked row takes both new files
keys G
wait_fcur 'Untracked (2)' || fail "G: $(fcur)"
keys Space
wait_screen "staged 2 files"
[ "$(git diff --cached --name-only | tr '\n' ' ')" = "new1.c new2.c " ] ||
	fail "untracked: $(git diff --cached --name-only)"

# Enter on the Staged row unstages them again, and a file marked
# intent-to-add stays marked
printf 'i\n' >ita.c
git add -N ita.c
keys F5
wait_screen "ita.c"
keys g g k
wait_fcur 'Staged (2)' || fail "the Staged row: $(fcur)"
keys Enter
wait_screen "unstaged 2 files"
[ "$(git diff --cached --name-only)" = "" ] || fail "unstage all"
[ -n "$(git ls-files ita.c)" ] || fail "intent-to-add mark lost"

# v takes a range of three files and Enter stages them, while Esc ends a
# range
keys g g v j j
wait_match '^+M f1\.c' || fail "range mark: $(screen | grep 'f1\.c')"
keys Enter
wait_screen "staged 3 files"
[ "$(git diff --cached --name-only | tr '\n' ' ')" = "f0.c f1.c f10.c " ] ||
	fail "range: $(git diff --cached --name-only)"
keys v j Escape Enter
wait_screen "staged f"
[ "$(git diff --cached --name-only | wc -l | tr -d ' ')" = 4 ] ||
	fail "Esc kept the range: $(git diff --cached --name-only)"
tui_quit
