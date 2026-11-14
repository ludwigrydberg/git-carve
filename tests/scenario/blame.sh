#!/bin/sh
# SPDX-License-Identifier: MIT
# blame, b in the f and t pickers, lists the commits that last touched the
# lines of a change, most lines first. Removed lines are blamed. An added
# line in a block blames the lines it replaces. An insertion blames the
# lines around it, or the line before at the end of the file. --cached
# blames the staged change, and a selection only its lines.
. "$(dirname "$0")/../lib.sh"
new_repo

printf '1\n2\n3\n4\n5\n6\n7\n8\n' >a.c
commit_all "drv: base"
sed 's/^3$/three/' a.c >"$tmp/x" && cat "$tmp/x" >a.c
commit_all "drv: three"
sed 's/^7$/seven/' a.c >"$tmp/x" && cat "$tmp/x" >a.c
commit_all "drv: seven"
base=$(git rev-parse --short HEAD~2)
three=$(git rev-parse --short HEAD~1)
seven=$(git rev-parse --short HEAD)

# three -> THREE, seven -> SEVEN, 6 removed, a line added at the end
sed -e 's/three/THREE/' -e 's/seven/SEVEN/' -e '/^6$/d' a.c >"$tmp/x"
cat "$tmp/x" >a.c
printf 'new\n' >>a.c

carve blame a.c >"$tmp/out" || fail "blame: $(cat "$tmp/out")"
# three, 6 and seven, one line each, and the insertion does not count
[ "$(wc -l <"$tmp/out")" -eq 3 ] || fail "all: $(cat "$tmp/out")"
grep -q "^1/3 $three drv: three$" "$tmp/out" || fail "all: $(cat "$tmp/out")"
grep -q "^1/3 $seven drv: seven$" "$tmp/out" || fail "all: $(cat "$tmp/out")"
grep -q "^1/3 $base drv: base$" "$tmp/out" || fail "all: $(cat "$tmp/out")"

# the added line THREE alone blames the line it replaces
[ "$(carve blame a.c n3)" = "1/1 $three drv: three" ] ||
	fail "n3: $(carve blame a.c n3)"
# SEVEN, whose block removes 6 and seven, both replaced
carve blame a.c n6 >"$tmp/out"
grep -q "^1/2 $seven" "$tmp/out" && grep -q "^1/2 $base" "$tmp/out" ||
	fail "n6: $(cat "$tmp/out")"
[ "$(carve blame a.c o3)" = "1/1 $three drv: three" ] ||
	fail "o3: $(carve blame a.c o3)"
# the line added at the end blames the line before it, 8 from the base
[ "$(carve blame a.c n8)" = "1/1 $base drv: base" ] ||
	fail "n8: $(carve blame a.c n8)"

# an insertion between two lines blames both, from the base and seven
git checkout -q a.c
awk '{ print } /^seven$/ { print "ins" }' a.c >"$tmp/x" && cat "$tmp/x" >a.c
carve blame a.c >"$tmp/out"
grep -q "^1/2 $seven" "$tmp/out" && grep -q "^1/2 $base" "$tmp/out" ||
	fail "insertion: $(cat "$tmp/out")"

# --cached blames the staged change only
git checkout -q a.c
sed 's/^three$/THREE/' a.c >"$tmp/x" && cat "$tmp/x" >a.c
git add a.c
sed 's/^seven$/SEVEN/' a.c >"$tmp/x" && cat "$tmp/x" >a.c
[ "$(carve blame --cached a.c)" = "1/1 $three drv: three" ] ||
	fail "cached: $(carve blame --cached a.c)"

# a new file has nothing in HEAD
printf 'n\n' >n.c
git add n.c
carve blame --cached n.c 2>"$tmp/err" && fail "new file blamed"
grep -q "new file" "$tmp/err" || fail "message: $(cat "$tmp/err")"
# no change
carve blame b.c 2>/dev/null && fail "no change blamed"
exit 0
