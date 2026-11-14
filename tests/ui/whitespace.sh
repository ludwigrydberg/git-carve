#!/bin/sh
# SPDX-License-Identifier: MIT
# Whitespace errors on added lines are highlighted as git diff does,
# trailing whitespace and spaces before a tab in the indent, following
# core.whitespace. With colours they get a red background, without them
# reverse video.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'a\n' >f.c
commit_all base
printf 'a\ntrail   \n \tindent\nclean\n' >f.c

esc=$(printf '\033')
# the raw screen row (with SGR codes) holding $1
# does the row with $1 switch on attribute $2 (SGR number) after the text?
marked_after() {
	row_sgr "$1" | sed "s/.*$1//" | grep -q "$esc\[\([0-9]*;\)*$2\(;[0-9]*\)*m"
}

tui_start
wait_screen "+trail"
marked_after trail 41 || fail "trailing spaces: $(row_sgr trail | od -c | head)"
marked_after "clean" 41 && fail "clean line marked"
# the spaces before the tab come before the text
row_sgr "indent" | grep -q "$esc\[\([0-9]*;\)*41\(;[0-9]*\)*m.*indent" ||
	fail "space before tab"
tui_quit

# monochrome, reverse video
TUI_TERM=vt100 tui_start
wait_screen "+trail"
marked_after trail 7 || fail "mono trailing: $(row_sgr trail | od -c | head)"
tui_quit

# core.whitespace turns them off
git config core.whitespace -trailing-space,-space-before-tab
tui_start
wait_screen "+trail"
marked_after trail 41 && fail "core.whitespace ignored"
row_sgr "indent" | grep -q "$esc\[\([0-9]*;\)*41" && fail "sbt not off"
tui_quit
