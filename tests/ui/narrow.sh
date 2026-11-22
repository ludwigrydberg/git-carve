#!/bin/sh
# SPDX-License-Identifier: MIT
# Readable in a quarter of a tmux window. Files are grouped by directory
# under a dim row, and sections are rules. Below carve.narrowWidth the
# lists stack above the diffs. o zooms any pane, gS hides the Series pane,
# and h and l scroll the Files pane's paths. F5 reads carve.narrowWidth
# again.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

for p in drivers/net/ethernet/intel/ice/ice_main.c \
	 drivers/net/ethernet/intel/ice/ice_txrx.c net/core/dev.c Makefile; do
	mkdir -p "$(dirname "$p")"
	awk 'BEGIN { for (i = 1; i <= 20; i++) print "line " i }' >"$p"
done
commit_all base
git branch -q up
git branch -q --set-upstream-to=up
echo x >>Makefile
commit_all "build: one"
for p in $(git ls-files); do
	sed 's/^line 7$/LINE 7/' "$p" >t && mv t "$p"
done
git add net/core/dev.c

# --- wide, side by side, grouped, with rules
TUI_W=134 TUI_H=30 tui_start
wait_screen "Unstaged (3)"
wait_screen " drivers/net/ethernet/intel/ice/"
# Staged first, as in git status, with the cursor on its file, and in
# Unstaged the top-level file first
inner | grep -q '^>M   dev.c' || fail "staged file first: $(screen)"
inner | grep -q '^ M Makefile' || fail "top-level file first: $(screen)"
inner | grep -q '^ M   ice_main.c' || fail "name under its directory"
screen | grep -q 'Staged (1)' || fail "staged rule"
inner | grep -q '^ net/core/' || fail "staged directory row"
[ "$(screen_row 'Series')" = 2 ] || fail "Series at the top"
[ "$(screen_row 'Files')" -lt "$(screen_row 'Staged (1)')" ] ||
	fail "layout"
# j stops on the Unstaged section's row and skips directory rows, Makefile,
# then ice_main.c
keys j j
wait_screen "Unstaged: Makefile"
keys j
wait_screen "Unstaged: drivers/net/ethernet/intel/ice/ice_main.c"
# o in Files shows the list alone, the whole width
keys o
wait_gone "Unstaged: drivers"
screen | grep -q 'drivers/net/ethernet/intel/ice/ *.$' ||
	fail "zoomed Files: $(screen)"
keys o
wait_screen "Unstaged: drivers"
# gS hides the Series pane, and J still moves
keys g S
wait_screen "Series pane hidden"
screen | grep -q ' Series: ' && fail "Series still shown"
keys J
wait_screen "Commit HEAD"
keys g S
wait_screen "Series pane shown"
wait_screen " Series: "
tui_quit

# --- narrow, the lists across the width and the diffs below them
TUI_W=80 TUI_H=30 tui_start
wait_screen "Unstaged (3)"
files=$(screen_row 'Files')
diff=$(screen_row 'Unstaged: net/core/dev.c')
[ -n "$diff" ] && [ "$diff" -gt "$files" ] ||
	fail "not stacked: $(screen)"
screen | sed -n "${diff}p" | grep -q '^.. Unstaged' ||
	fail "the diff pane not at the left edge: $(screen | sed -n "${diff}p")"
# changed in another shell, side by side again after F5
git config carve.narrowWidth 60
keys F5
wait_screen "refreshed"
[ "$(screen_row 'Unstaged: net/core/dev.c')" -le "$files" ] ||
	fail "F5 did not read carve.narrowWidth: $(screen)"
git config --unset carve.narrowWidth
tui_quit

# --- h and l in the Files pane, with a directory too long for it
d=documentation/devicetree/bindings/net/wireless
mkdir -p $d
echo a >$d/x.yaml
git add $d/x.yaml
TUI_W=44 TUI_H=30 tui_start
wait_screen "x.yaml"
# cut in the middle, its first directory and its end
screen | grep -q 'bindings/net/wireless/' || fail "the end: $(screen)"
inner | grep -q '^ documentation/~' || fail "the first directory: $(screen)"
inner | grep -q '^ documentation/devicetree' && fail "the start shown already"
# h and l scroll the selected file's rows, x.yaml, the first staged
keys G g g
wait_inner '^>A   x\.yaml' || fail "G: $(screen | grep x.yaml)"
keys h
wait_inner '^ documentation/devicetree/' || fail "h: $(screen | grep wireless)"
keys l
wait_inner '^ documentation/~.*bindings/net/wireless/' ||
	fail "l: $(screen | grep wireless)"
tui_quit
exit 0
