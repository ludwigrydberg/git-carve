#!/bin/sh
# SPDX-License-Identifier: MIT
# Highlighting within lines. In a changed line paired with one on the other
# side, the changed words are shown in reverse video, or bold and
# underlined without colours. Lines with little in common, and unpaired
# lines, are not marked.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

printf 'return foo(bar);\nunrelated\nkeep\n' >f.c
commit_all base
printf 'return foo(baz);\ncompletely other text\nkeep\nadded\n' >f.c

esc=$(printf '\033')
tui_start
wait_screen "+return foo(baz);"
# the attribute is switched on right before the changed word
row_sgr "baz" | grep -q "foo($esc\[[0-9;]*7[0-9;]*mbaz" ||
	fail "+ side: $(row_sgr baz | od -c | head -5)"
row_sgr "-return" | grep -q "foo($esc\[[0-9;]*7[0-9;]*mbar" ||
	fail "- side"
row_sgr "completely other" | grep -q "$esc\[[0-9;]*7[0-9;]*m" &&
	fail "unrelated pair marked"
# (the Files cursor in reverse video may share the row)
row_sgr "added" | grep -q "$esc\[[0-9;]*7[0-9;]*m[^$esc]*added" &&
	fail "unpaired marked"
tui_quit

TUI_TERM=vt100 tui_start
wait_screen "+return foo(baz);"
row_sgr "baz" | grep -q "foo($esc\[[0-9;]*4[0-9;]*mbaz" ||
	fail "mono: $(row_sgr baz | od -c | head -5)"
tui_quit
