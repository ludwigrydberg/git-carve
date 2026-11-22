#!/bin/sh
# SPDX-License-Identifier: MIT
# View options. + and - change the context lines, and staging keeps
# working, also at 0. W shows whole functions, and w ignores whitespace as
# a view only, with line operations refused meanwhile.
. "$(dirname "$0")/../lib-tmux.sh"
new_repo

{ echo "int f(void)"; echo "{"; i=1
  while [ $i -le 12 ]; do echo "	x$i();"; i=$((i + 1)); done
  echo "}"; } >f.c
commit_all base
sed 's/x6()/X6()/; s/x7();/x7(); /; s/x11()/X11()/' f.c >f.new && mv f.new f.c

tui_start
wait_screen "M f.c"
keys Tab
wait_screen "x4();"
keys - - -
wait_screen "0 context lines"
wait_active "Unstaged: f.c  [-U0]"
wait_gone "x4();"
# stage the X6 line pair at zero context
keys / x6 Enter m / X6 Enter m Space
wait_index f.c "$(sed 's/x7(); /x7();/; s/X11/x11/' f.c)\n"
keys + + + + +
wait_screen "5 context lines"
wait_screen "[-U5]"
keys - - W
wait_screen "3 context lines, whole functions"
wait_screen "[-W]"
wait_screen "int f(void)"
keys W w
wait_screen "whitespace ignored (view only)"
wait_screen "[-w (view only)]"
wait_screen "whitespace is ignored in this view"
keys Space
wait_screen "w turns line operations back on"
close_box
keys w
wait_screen "3 context lines"
wait_gone "view only"
tui_quit
