#!/bin/sh
# SPDX-License-Identifier: MIT
# Stage and unstage a single line deep inside a whole-file hunk of a new
# file, by number.
. "$(dirname "$0")/../lib.sh"
new_repo

git commit -q --allow-empty -m root
i=1
while [ $i -le 400 ]; do
	echo "line $i"
	i=$((i + 1))
done >big.c

carve stage big.c n258 || fail "stage n258"
assert_index_text big.c 'line 258\n'

# stage the whole file, then take line 258 back out
carve stage-file big.c || fail "stage-file"
carve unstage big.c n258 || fail "unstage n258"
awk 'NR != 258' big.c >"$tmp/exp"
assert_index big.c "$tmp/exp"
carve diff big.c | grep -q " 258 +line 258$" || fail "unstaged view"

# and put it back using the unstaged diff's number
carve stage big.c n258 || fail "restage"
assert_index big.c big.c
