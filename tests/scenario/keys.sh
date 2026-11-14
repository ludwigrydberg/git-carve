#!/bin/sh
# SPDX-License-Identifier: MIT
# 'git carve keys' lists the TUI bindings, including the user's own from
# carve.keys.<action>, and reports configuration problems.
. "$(dirname "$0")/../lib.sh"
new_repo
printf 'a\n' >f.c
commit_all base

tui_cmd keys >"$tmp/out" 2>"$tmp/err" || fail "keys failed"
grep -q '^stage  *Space  ' "$tmp/out" || fail "default stage: $(cat "$tmp/out")"
grep -q '^down  *j, Down  ' "$tmp/out" || fail "default down"
grep -q '^repaint  *Ctrl-L  ' "$tmp/out" || fail "default repaint"
grep -q '^top  *Home, gg  ' "$tmp/out" || fail "default top"
grep -q '^page-down  *Ctrl-F, PgDn  ' "$tmp/out" || fail "default page-down"
grep -q '^search-list  *g/  ' "$tmp/out" || fail "default search-list"
grep -q '^scroll-center  *zz  ' "$tmp/out" || fail "default zz"
[ -s "$tmp/err" ] && fail "warnings without config: $(cat "$tmp/err")"

git config carve.keys.stage "y Space"
git config carve.keys.zoom none
git config carve.keys.quit Ctrl-Q
tui_cmd keys >"$tmp/out" 2>"$tmp/err" || fail "keys failed"
grep -q '^stage  *Space, y  ' "$tmp/out" || fail "remapped stage: $(cat "$tmp/out")"
grep -q '^zoom  *(none)  ' "$tmp/out" || fail "unbound zoom"
grep -q '^quit  *Ctrl-Q  ' "$tmp/out" || fail "remapped quit"
[ -s "$tmp/err" ] && fail "warnings for a good config: $(cat "$tmp/err")"

# problems are reported on stderr, and the defaults stay
git config carve.keys.nosuch y
git config carve.keys.commit Bogus
tui_cmd keys >"$tmp/out" 2>"$tmp/err" || fail "keys failed on a bad config"
grep -q "unknown action 'nosuch'" "$tmp/err" || fail "unknown action: $(cat "$tmp/err")"
grep -q "carve.keys.commit: unknown key 'Bogus'" "$tmp/err" ||
	fail "unknown key: $(cat "$tmp/err")"
grep -q '^commit  *c  ' "$tmp/out" || fail "commit lost its default"

# a plain key over a default prefix ends the sequences under it, and says
# so
git config --unset carve.keys.nosuch
git config --unset carve.keys.commit
git config carve.keys.top g
tui_cmd keys >"$tmp/out" 2>"$tmp/err" || fail "keys with g"
grep -q '^top  *g  ' "$tmp/out" || fail "g for top: $(cat "$tmp/out")"
grep -q 'action search-list has no key any more' "$tmp/err" ||
	fail "g/ lost silently: $(cat "$tmp/err")"
git config --unset carve.keys.top

# your own sequence, which a plain key may not take away
git config carve.keys.show-commit gw
tui_cmd keys >"$tmp/out" 2>"$tmp/err" || fail "keys with a sequence"
grep -q '^show-commit  *gw  ' "$tmp/out" || fail "own sequence: $(cat "$tmp/out")"
[ -s "$tmp/err" ] && fail "warnings for a sequence: $(cat "$tmp/err")"
git config carve.keys.top g
tui_cmd keys >"$tmp/out" 2>"$tmp/err" || fail "keys, g over gw"
grep -q "carve.keys.top: 'g' starts a sequence bound in carve.keys" \
	"$tmp/err" || fail "gw lost: $(cat "$tmp/err")"
grep -q '^show-commit  *gw  ' "$tmp/out" || fail "gw gone"
grep -q '^top  *Home, gg  ' "$tmp/out" || fail "top lost its defaults"
git config --unset carve.keys.top

# a default sequence can be taken, like a default key
git config carve.keys.search-list zz
tui_cmd keys >"$tmp/out" 2>"$tmp/err" || fail "keys, zz taken"
grep -q '^search-list  *zz  ' "$tmp/out" || fail "zz: $(cat "$tmp/out")"
grep -q 'action scroll-center has no key any more' "$tmp/err" ||
	fail "zz lost silently: $(cat "$tmp/err")"

tui_cmd keys extra >/dev/null 2>&1 && fail "keys takes no arguments"
exit 0
