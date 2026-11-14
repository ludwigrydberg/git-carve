#!/bin/sh
# SPDX-License-Identifier: MIT
# Randomized invariant tests, in tests/fuzz/fuzz.c.
. "$(dirname "$0")/../lib.sh"
new_repo
"$TESTS_DIR/fuzz/fuzz"
