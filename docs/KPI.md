# KPIs for huge change sets

These benchmarks measure how long it takes for git-carve to show the
result of common actions with about 15,000 changed files.

## Running it

| Command | What it does |
|---|---|
| `make check` | process counts only, on a small tree of 450 changed files, against `tests/perf/kpi-quick.txt` |
| `make kpi` | time common actions on a generated tree (20,000 files with 15,000 changed and 1,500 new), against `tests/perf/kpi-full.txt` |
| `make kpi-save` | the same, saving the results as the new baseline (`KPI_SHOW=<step>` prints the git processes of one step) |
| `KPI_KERNEL=<linux repo> KPI_FROM=v7.1 KPI_TO=v7.2 make kpi` | a real kernel increment, in a temporary worktree of that repository, against `tests/perf/kpi-kernel.txt` |

A step's git processes are those its keys cause, from the TUI waiting
idle before them until it is idle again (`GIT_CARVE_IDLE`), and they
are the same on every machine. A step fails when it runs more git
processes than its baseline, or takes more than twice its baseline time
plus 0.3 s. Full runs compare timings with the baseline for the same
machine. Use `KPI_TIMES=0` to skip timing comparisons on other
machines.

## The steps

| KPI | What it measures |
|---|---|
| startup | `git carve` until the Files list is shown |
| untracked-scan | until the background scan of untracked files is merged |
| hold-j-files | git processes for 30 `j` in one burst, as a held key sends them, in the Files pane |
| hold-j-series | the same with 40 `j` in the Series pane |
| search | `/` for a line in the last changed file, until it is shown (or the search says the diffs are too big) |
| refresh | F5 |
| stage-file | Enter on one file |
| commit-one | `c` with one file staged |
| stage-all | Enter on the Unstaged section row |
| unstage-all | Enter on the Staged section row |
| commit-all | `c` with everything staged |
| split | `s` on that commit |
| split-abort | `X` on that split |
| rss-kb | memory of the TUI after startup |

## Results

Measured on November 22, 2026, on a MacBook Pro M2 with Git 2.50.1, on
mains power. The generated tree's column is the median of three runs,
the kernel's one run.

| KPI | generated tree (15,000 changed) | kernel v7.2 reset to v7.1 (14,900 changed, 1,535 new) |
|---|---|---|
| startup | 0.49 s | 1.61 s |
| untracked-scan | 0.02 s | 0.02 s |
| search | 0.46 s | 1.45 s |
| refresh | 0.46 s | 1.71 s |
| stage-file | 0.17 s | 0.39 s |
| commit-one | 0.67 s | 2.43 s |
| stage-all | 0.46 s | 2.10 s |
| unstage-all | 0.47 s | 1.80 s |
| commit-all | 0.17 s | 0.29 s |
| split | 0.32 s | 0.83 s |
| split-abort | 0.18 s | 0.33 s |
