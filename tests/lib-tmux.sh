# shellcheck shell=sh
# SPDX-License-Identifier: MIT
# Drive the TUI inside a private tmux server.
. "$(dirname "$0")/../lib.sh"
command -v tmux >/dev/null 2>&1 || skip "tmux not installed"

# tui_start runs git carve in a 120x40 detached session in the cwd.
tui_start() {
	TMUX_SOCK="carve-test-$$"
	unset TMUX
	: >"$tmp/idle"
	tmux -L "$TMUX_SOCK" -f /dev/null new-session -d -x "${TUI_W:-120}" -y "${TUI_H:-40}" \
		-s t "LC_ALL=${TUI_LOCALE:-C} TERM=${TUI_TERM:-screen} \
		GIT_CARVE_IDLE=$tmp/idle ${TUI_ENV} \
		$CARVE 2>>$tmp/tui.err" ||
		fail "tmux new-session"
	# TUI_CLEANUP is more to do on exit, since a script's own trap is
	# replaced
	trap 'tmux -L "$TMUX_SOCK" kill-server 2>/dev/null; eval "${TUI_CLEANUP:-:}";
		cleanup_tmp' EXIT
	# the first screen can be slow on a cold machine
	wait_screen "Files" 150
}

screen() {
	tmux -L "$TMUX_SOCK" capture-pane -p -t t
}

# inner: the screen without its first column, the left line of the frames
# when the panes are stacked or zoomed, or of the lists side by side
inner() {
	screen | cut -c2-
}

# wait_inner <regex> is wait_match on inner.
wait_inner() {
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ]; do
		inner | grep -q -- "$1" && return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	return 1
}

# row_sgr <text>: the screen rows with text, with their colour codes
row_sgr() {
	tmux -L "$TMUX_SOCK" capture-pane -e -p -t t | grep -F -- "$1"
}

keys() {
	tmux -L "$TMUX_SOCK" send-keys -t t "$@"
}

# How long the waits below try by default, in tenths of a second. It is
# long, since the suites run in parallel and a busy machine is slow. A wait
# that succeeds returns at once.
WAIT_TRIES=${WAIT_TRIES:-200}

# wait_screen <fixed text> [tries]: wait until the screen shows text.
wait_screen() {
	_n=${2:-$WAIT_TRIES}
	while [ "$_n" -gt 0 ]; do
		screen 2>/dev/null | grep -F -q -- "$1" && return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	echo "--- screen:" >&2
	screen >&2
	check_sanitizers
	fail "timed out waiting for '$1'"
}

# wait_cursor <regex> [pane]: wait until the cursor row of a diff pane
# (cursor_row) matches.
wait_cursor() {
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ]; do
		cursor_row "${2:-1}" | grep -q -- "$1" && return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	return 1
}

# wait_match <regex> waits until a line of the screen matches, and is false
# if it never does.
wait_match() {
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ]; do
		screen | grep -q -- "$1" && return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	return 1
}

# wait_gone <fixed text>: wait until text disappears from the screen.
wait_gone() {
	_n=${2:-$WAIT_TRIES}
	while [ "$_n" -gt 0 ]; do
		screen 2>/dev/null | grep -F -q -- "$1" || return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	screen >&2
	fail "timed out waiting for '$1' to disappear"
}

# wait_index <path> <expected text>: poll until the index holds text.
wait_index() {
	printf '%b' "$2" >"$tmp/want"
	_want=$(file_hash "$tmp/want")
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ]; do
		[ "$(index_blob "$1")" = "$_want" ] && return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	screen >&2
	assert_index "$1" "$tmp/want"
}

# close_box closes the box a refusal or a warning shows mid-screen, whose
# footer ends in "key: close". The key does nothing else.
close_box() {
	wait_screen "key: close"
	keys Escape
	wait_gone "key: close"
}

# tui_quit sends q, after closing a box still shown
tui_quit() {
	if screen 2>/dev/null | grep -F -q -- "key: close"; then
		keys Escape
		wait_gone "key: close"
	fi
	keys q
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ]; do
		if ! tmux -L "$TMUX_SOCK" has-session -t t 2>/dev/null; then
			check_sanitizers
			return 0
		fi
		sleep 0.1
		_n=$((_n - 1))
	done
	screen >&2
	fail "TUI did not exit"
}

# Diff pane rows, inside the frames right of the Files pane, that carry
# the cursor marker. $1 is 1 for the Unstaged pane and 2 for the Staged
# pane.
cursor_row() {
	screen | cut -c33- | grep '^>' | sed -n "${1:-1}p" | sed 's/.$//; s/ *$//'
}

# tui_start runs the TUI with GIT_CARVE_IDLE=$tmp/idle. It writes "idle" to
# that file when nothing is left to do but wait for a key, and "busy" when a key
# comes. The waits below use it to measure a step on any machine: every
# git that the step's keys cause runs before the TUI is idle again, and
# none of an earlier step's runs after.
# how often the TUI was idle so far
idle_count() {
	awk '$0 == "idle" { n++ } END { print n + 0 }' "$tmp/idle" 2>/dev/null ||
		echo 0
}

# wait_idle: until the TUI waits for a key with nothing pending
wait_idle() {
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ]; do
		[ "$(tail -n 1 "$tmp/idle" 2>/dev/null)" = idle ] && return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	screen >&2
	fail "the TUI never became idle (last: $(tail -n 1 "$tmp/idle"))"
}

# idle_after <count>: until the TUI is idle again after that count, once
# the keys sent since have been handled
idle_after() {
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ]; do
		[ "$(idle_count)" -gt "$1" ] &&
			[ "$(tail -n 1 "$tmp/idle" 2>/dev/null)" = idle ] && return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	screen >&2
	fail "the TUI did not become idle after the keys ($(idle_count) times idle, last: $(tail -n 1 "$tmp/idle"))"
}

# step_keys <keys...>: once idle, send the keys in one write and wait
# until they are handled and the TUI is idle again
step_keys() {
	wait_idle
	_ic=$(idle_count)
	keys "$@"
	idle_after "$_ic"
}

# wait_fcur <regex> waits until the Files pane's cursor row (inside its
# frame, the columns 2 to 30) matches, and fails the test if it never does.
wait_fcur() {
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ]; do
		screen | cut -c2-30 | grep '^>' | grep -q -- "$1" && return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	screen >&2
	fail "Files cursor not on '$1'"
}

# The TUI's stderr goes to $tmp/tui.err, where sanitizer reports end up.
check_sanitizers() {
	if grep -q -e 'runtime error' -e 'Sanitizer' "$tmp/tui.err" 2>/dev/null
	then
		cat "$tmp/tui.err" >&2
		fail "sanitizer report from the TUI"
	fi
}

# Whether the screen uses colours (SGR 30-37, 40-47, 90-107, 38;5 or 48;5).
screen_has_colors() {
	tmux -L "$TMUX_SOCK" capture-pane -e -p -t t |
		grep -E -q "$(printf '\033')\[([0-9]+;)*(3[0-7]|4[0-7]|9[0-7]|10[0-7]|38;5|48;5)(;|m)"
}

tui_resize() {
	tmux -L "$TMUX_SOCK" resize-window -t t -x "$1" -y "$2" ||
		fail "tmux resize-window $1x$2"
}

# The row (1-based) of the screen that contains text, or empty.
screen_row() {
	screen | grep -n -F -- "$1" | head -n 1 | cut -d: -f1
}

# active_title: the title of the active pane, as the top line of its frame
# shows it ("Files: 1/3"). It is the one title in bold on a background.
active_title() {
	tmux -L "$TMUX_SOCK" capture-pane -e -p -t t | awk '
	{
		line = $0; bold = 0; bg = 0; title = ""; n = length(line)
		for (i = 1; i <= n; i++) {
			c = substr(line, i, 1)
			if (c == "\033") {
				j = index(substr(line, i), "m")
				split(substr(line, i + 2, j - 3), p, ";")
				for (k = 1; k in p; k++) {
					v = p[k] + 0
					if (v == 0) { bold = 0; bg = 0 }
					else if (v == 1) bold = 1
					else if (v == 22) bold = 0
					else if (v == 7 || (v >= 40 && v <= 47) ||
						 v == 48) bg = 1
					else if (v == 27 || v == 49) bg = 0
					if (v == 38 || v == 48) k += 2
				}
				delete p
				i += j - 1
				continue
			}
			if (c == "\016" || c == "\017")
				continue
			if (bold && bg)
				title = title c
			else if (title != "")
				break
		}
		sub(/^ +/, "", title)
		sub(/ +$/, "", title)
		if (title != "") {
			print title
			exit
		}
	}'
}

# wait_active <fixed text>: wait until the active pane's title has text
wait_active() {
	_n=$WAIT_TRIES
	while [ "$_n" -gt 0 ]; do
		active_title | grep -F -q -- "$1" && return 0
		sleep 0.1
		_n=$((_n - 1))
	done
	screen >&2
	fail "active pane not '$1' (it is '$(active_title)')"
}
