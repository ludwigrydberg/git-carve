# Contributing to git-carve

## Building and testing

The TUI tests need `tmux` and are skipped without it. The performance
test needs `perl` for its timing. `make check`
runs the lint and analysis tools that are installed and skips the
others. They are `clang-format` 22, `shellcheck`, `codespell`, `lizard`,
`clang-tidy` and `cppcheck`. `pip install --user lizard clang-tidy`
installs two of them.

| Target | What it does |
|---|---|
| `make` | build `git-carve` (`make WERROR=0` if an unfamiliar compiler warns) |
| `make check` | unit, scenario, fuzz, TUI and performance tests, then `make lint` and `make analyze` |
| `make check-unit`, `check-scenario`, `check-fuzz`, `check-ui`, `check-perf` | one stage of the tests |
| `make check SANITIZE=1` | the same with AddressSanitizer and UBSan (macOS: `ASAN_OPTIONS=detect_leaks=0`) |
| `make fuzz`, `make fuzz-long` | longer fuzzing runs (`FUZZ_ITERS`, `FUZZ_SEED`, `FUZZ_SEEDS`) |
| `make install` / `make uninstall` | binary and man page under `PREFIX` (`DESTDIR` for packaging) |
| `make dist` | `git-carve-<version>.tar.gz` of the committed tree |
| `make format` | format the sources with `clang-format` |
| `make lint` | the formatting, the comments and declaration order (`tools/style.pl`), `shellcheck`, `codespell`, and a complexity limit per function (`lizard`, at most 40, the exceptions in `tools/complexity.allow`) |
| `make analyze` | `clang-tidy` with `.clang-tidy` (else `clang --analyze`), `cppcheck` and `gcc -fanalyzer`, whichever are installed (checked false positives of gcc in `tools/analyzer.supp`) |
| `make coverage` | line coverage of all test stages with gcov or llvm-cov, a total, annotated sources in `coverage/` (`COVERAGE_MIN=<percent>` fails under it) |
| `make kpi` | the KPI suite on a huge change set, with the time and git processes of each step against `tests/perf/kpi-full.txt` (see [`docs/KPI.md`](docs/KPI.md)) |
| `make kpi-save` | the same, saving the results as the baseline |

## Layout

The modules form layers. A module uses its own layer and the ones
below it, and no two modules use each other:

| Layer | Modules |
|---|---|
| base | `util.c` (strbuf, memory), `git.c` (spawning git, `struct repo`, config, commit lookups) |
| reading git | `diff.c` (diff parser, selections, line mapping), `status.c` (git status) |
| one change | `patch.c` (partial patches, stage/unstage, discard), `index.c` (editing the index), `rename.c`, `conflict.c`, `subst.c` (`:s`) |
| shared by operations | `opstate.c` (an operation's state and refs), `rebase.c` (rebases, the series), `cdiff.c` (a commit's diff), `message.c` (commit messages) |
| operations | `split.c`, `series.c` (fixup-to, defer, squash), `move.c` (any commit of the series), `undo.c`, `absorb.c`, `gc.c`, `ops.c` (continue, abort) |
| frontends | `main.c` and `ui/` (the TUI), `cli_main.c` and `cli.c` (`git-carve-cli`) |

The TUI is `ui.c` (main loop, keys), `actions.c` (what keys do),
`state.c` (loading), `draw.c`, `term.c` (terminal, prompts, overlays),
`keys.c` (bindings), `sview.c` (Series pane) and `dcache.c` (diff
cache). Each has a header with what it offers the others. `ui_int.h`
holds the shared types and `struct ui`.

```
tests/          unit/ scenario/ fuzz/ ui/ perf/
docs/           git-carve.1 (man page), git-carve-cli.1, KPI.md, images/
```

## Code

- C (`-std=gnu11`) in Linux kernel style, with tabs, 80 columns as a soft
  limit, `snake_case` and early returns. The tree is formatted with
  clang-format (`.clang-format`, `make format`).
- Comments as in the kernel. One that spans lines opens with `/*` and
  closes with ` */` on lines of their own, and each line between starts
  with ` * `. No `//` comments, except the SPDX line at the top of a
  `.c`.
- Local declarations in reverse xmas tree order, longest line first. A
  declaration whose initializer uses a variable declared above it comes
  after that one. When the order needs it elsewhere, its initialization
  moves below the declarations.
- `make check` and `make lint` check all three with
  `tests/scenario/style.sh`. The clang-format part runs only with the
  version `.clang-format` names.
- Only POSIX APIs. No `/proc`, and no glibc-only functions without a
  fallback. It must build and pass `make check` on Linux and macOS, with
  gcc and clang, `-Wall -Wextra -Werror`.
- The only runtime dependency is ncursesw.
- git is always run as the `git` binary, never through a library. The
  user's configuration, hooks, filters and fsmonitor then apply. `src/git.c`
  owns all process spawning. Any git output that is parsed is made
  immune to user configuration (`push_diff_args()` in `src/git.c`),
  and `git apply` always gets `--whitespace=nowarn`.
- Core modules are plain functions over explicit structs, without global
  state. Only `src/ui/` keeps state.
- A function that returns a `char *` or fills a `struct strbuf` gives it
  to the caller, who frees it. A `const char *` stays owned by whoever
  returned it.
- Comment what the code cannot say, such as why, and what is not
  obvious. Write plain sentences. A function's comment sits at its definition, or in the header when the
  definition has none.

## Rules for new code

They keep the code base easy to change. Those marked *checked* fail
`make check` (`tests/scenario/structure.sh`, `tests/scenario/docs.sh`)
when broken. The others are for review. A change that breaks one says
why in its commit message.

Where code goes:

1. A module uses its own layer and lower ones, never a higher one, and
   modules do not use each other in a cycle. *checked*
2. The frontends ask the core. There are no git commands in `ui/` or
   `cli.c`, except the TUI's two background runs. Reading something to
   show it gets a core function too, and the CLI and the TUI see the
   same.
   *checked*
3. A whole operation (check, change, roll back on failure) lives in the
   core, once. The frontends collect input and show results.
4. Only `git.c` starts processes. *checked*

Sharing instead of copying:

5. Before writing a helper, look for one. The second copy of an idea
   moves to the lowest layer both users may call. No two files have a
   static function of the same name. *checked*
6. Options that make git's output parseable come from `git.h`
   (`GIT_PLAIN`, `GIT_APPLY_PLAIN`, `push_diff_args()`). *checked*
7. One text, one place. A message or argument used twice is a
   `#define` in the module that owns it. *checked*
8. A test helper used by two tests lives in `tests/lib.sh` or
   `tests/lib-tmux.sh`. *checked*

Size and visibility:

9. Functions stay under 60 lines where they can, never over 150. A
   switch over actions or commands is a table of small functions.
   *checked* (150)
10. Files over about 1500 lines are split by what they do.
11. A header declares what other files use. Everything else is
    `static`. *checked*

State:

12. No mutable global state outside `src/ui/`. *checked*
13. A module's cache in `struct repo` is freed by the module. It
    registers the cache with `repo_on_release()`, and `git.c` does not
    call up.
14. New TUI state goes into a group of `struct ui` (`ui.msg`,
    `ui.lay`, `ui.cfg`, `ui.sv`), or stays `static` in its file.

A new operation:

15. If it changes the index, HEAD, refs or the work tree, it is in the
    undo table (`undo_kind()` in `undo.c`) that the CLI and the TUI
    share. One that changes the work tree saves its files first.
16. Every TUI action has a function in `action_fn` (*checked*), a CLI
    command, a scenario test through the CLI, a tmux test of its key,
    a budget line in `tests/perf/spawns.sh` if it runs git, and its
    documentation in the README, the man page and the `?`
    help (*checked* for every key and option in the man page).
17. A refusal says "reason; what to do" in the user's terms, one "; "
    at most, lower case, no full stop, with keys as `{action}`. The TUI
    shows it in a box with the second part on its own lines. A failure
    from git keeps git's output with `repo_err_detail()`. *checked*
18. Count the git processes (below). Anything on a performance path is
    measured with `make kpi` before and after.

Removing:

19. A dropped feature goes everywhere in the same series, from code and
    fields to keys, options, tests and docs. What it left in users' repositories
    is for `gc.c` alone. *checked* for the names of dropped features.
20. A comment describes the code below it. When the code moves or
    changes, the comment follows.

## A budget of git processes

A git process costs about 14 ms to start on macOS, and one that reads
the index or the work tree tens to hundreds of ms on a kernel tree.

- A command or a TUI key spawns a bounded number of git processes that
  never grows with the number of files, commits, hunks or refs involved.
  Batch (one git for many paths, `cat-file --batch`, `for-each-ref`,
  `update-ref --stdin`) instead of running one git per item.
- Never ask git twice for what cannot have changed during the operation
  (configuration, `--git-path`, HEAD while only the index changes).
- `tests/perf/spawns.sh` holds a budget per operation and fails above it
  or when a count depends on the size of the tree. More than five
  processes for one operation needs a reason in that file.
  `GIT_CARVE_TRACE=<file>` logs every git process run.

## Tests

`make check` runs everything and must pass, also with `make check
SANITIZE=1`.

- `tests/unit/` has the C unit tests, with golden files for the diff
  parser and tables for partial patches.
- `tests/scenario/` has one POSIX `sh` script per workflow. Each drives
  `git-carve-cli` in isolated repositories with their own `HOME`. A
  subset runs again under hostile git configuration.
- `tests/fuzz/` checks invariants on random input with a fixed seed
  (`FUZZ_SEED`, `FUZZ_ITERS`).
- `tests/ui/` drives the real TUI in tmux with `send-keys` and
  `capture-pane`. It is skipped without tmux. A test waits for what it
  checks, never for a fixed time. To count what keys cause, the TUI
  writes when it is idle to `GIT_CARVE_IDLE`, and `step_keys` in
  `tests/lib-tmux.sh` waits for that, so the counts are the same on
  every machine.
- `tests/perf/` has a generated 50,000-file tree and the process budgets.
- `tests/scenario/rollback.sh` makes every git process of an operation
  fail in turn. The repository must be as it was, or `abort` must
  bring it back, or the operation must be done.
  `tests/scenario/rollback-fail.sh` makes the rollback itself fail. The
  state must stay, and `abort` must then bring everything back.
- `tests/scenario/crash.sh` kills git carve at every git process of an
  operation, before that git runs and while it runs on by itself, and
  then starts it again. The repository must be as before or as done,
  after `abort` and the cleanup a fresh start offers. `CRASH_SEED` and
  `CRASH_ITERS` add random operations, points and signals.

The fuzz and scenario tests check these invariants by name:

| | Invariant |
|---|---|
| I1 | staging a selection and unstaging it again leaves the index blob as it was |
| I2 | staging every line equals `git add` (the same blob) |
| I3 | the staged blob equals an in-memory model of the selected changes |
| I4 | the unselected changes stay in `git diff` |
| I5 | staging, unstaging and editing the index never touch the work tree |
| I6 | a finished split has the tree of the commit split |
| I7 | after a split, defer or fixup of an older commit, every later commit keeps its tree (split, defer) or gets the expected one (fixup) |
| I8 | aborting a split restores HEAD, the index and the work tree exactly |
| I9 | undoing staging steps one by one restores each earlier index |

Shell tests use only tools present on stock macOS and Debian. That
rules out `sed -i`, `readlink -f`, `sha256sum` (use `git hash-object`)
and GNU-only flags. Tests never touch your own git configuration.

Never weaken or delete a test to make it pass. Fix the code. If the
test is wrong, fix the test in its own commit and say why.

## Commits

One logical change per commit, with a kernel-style message of
`subsystem: short imperative summary` and a body explaining why. The
subsystems are `diff`, `patch`, `index`, `split`, `series`, `cli`, `ui`,
`undo`, `absorb`, `rename`, `tests`, `build`, `docs`. `make check` passes
for every commit.

Documentation moves with the code. A change updates `README.md`, the
man page (`docs/git-carve.1`) where they describe it.
These describe the TUI only. The commands of `git-carve-cli` are an
internal interface for the tests, documented in its `help` and in
`docs/git-carve-cli.1`. `tests/scenario/docs.sh` checks that every key
and configuration option is documented and that the user documentation
does not describe the CLI.
