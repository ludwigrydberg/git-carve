#!/bin/sh
# SPDX-License-Identifier: MIT
# Re-run scenarios with hostile user configuration in the isolated
# ~/.gitconfig (diff.noprefix, color.ui=always, apply.whitespace=fix, ...).
# Results must not change.
dir=$(dirname "$0")
rc=0
for t in stage-lines new-file-lines line-258 diff-cli split-head split-older \
	fixup-to defer check; do
	[ -f "$dir/$t.sh" ] || continue
	if ! CARVE_HOSTILE=1 sh "$dir/$t.sh"; then
		echo "FAIL under hostile config: $t"
		rc=1
	fi
done
exit $rc
