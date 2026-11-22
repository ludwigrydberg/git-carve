# git-carve

**Build and edit Git patch series in your terminal.**

Stage individual lines, even when unrelated changes share a diff hunk.
Split an existing commit or move a fix into the commit it belongs in.
The file list, diff, commit message and branch's commits are all visible
in one screen.

![git carve showing the file list, commit message and diff for a driver change](docs/images/screenshot.png)

## What you can do with it

- **Stage one line out of a hunk.** Two changes next to each other end
  up in separate commits.
- **Split a commit**, also one deep in your branch. The pieces always add
  up to the original.
- **Fix an older commit.** Move lines into it directly, or make a
  `fixup!` commit now and squash them all at the end.
- **Find the right commit for a fix.** git-carve looks up which commit
  last touched the lines and suggests it.
- **Undo** a staged line, a commit or a whole split.

git-carve uses your Git configuration and hooks. It checks the resulting
trees after rewriting commits and attempts to restore the original state
if a check fails. When a failed rollback can be retried, the error tells
you how to resume recovery.

## See it work

![Staging one fix out of a hunk, writing its message, committing it, and moving another into an older commit](docs/images/demo.gif)

*One fix is staged out of a hunk, its message written in the message
pane and committed. Another moves into an older commit. `J` and `K`
browse the result.*

## How git-carve compares with Lazygit and Tig

Tig is a terminal Git browser with line-by-line staging. Lazygit is a
broader Git client that also supports line-by-line staging and moving
changes between commits with custom patches. git-carve focuses on
editing and reviewing a patch series.
([Tig manual](https://jonas.github.io/tig/doc/manual.html) ·
[Lazygit custom patches](https://github.com/jesseduffield/lazygit#rebase-magic-custom-patches))

- **Move a replacement from the diff.** `gm` marks both sides of a
  changed line. Press `t` to choose the commit that should receive the
  change.
- **Split a commit and check the result.** Commit the pieces one at a
  time. The split can finish once the pieces reproduce the original
  commit's tree. Use `X` to abort an in-progress split.
- **Review fixes for several commits at once.** `ga` proposes where each
  change belongs. Review the plan before applying it.

## Install

You need git 2.26 or newer, a C compiler, `make`, `pkg-config` and
ncursesw.

```sh
git clone https://github.com/ludwigrydberg/git-carve.git
cd git-carve
make
make install PREFIX="$HOME/.local"
```

With `$HOME/.local/bin` in your `PATH`, `git carve` works, and
`git carve --help` opens the man page.

| Platform | Packages |
|---|---|
| Debian, Ubuntu | `build-essential pkg-config libncurses-dev` |
| Fedora | `gcc make pkgconf-pkg-config ncurses-devel` |
| Arch Linux | `base-devel ncurses` |
| macOS | `brew install ncurses pkgconf`, then `export PKG_CONFIG_PATH="$(brew --prefix ncurses)/lib/pkgconfig"` |

## Quick start

Run `git carve` in a repository with unstaged changes. Select a file in
**Files** and use `Tab` to reach the diff. The active pane has a heavy
frame, and its top line names the pane `Tab` goes to next. Put the cursor on a changed
line and press `Space` to stage it. For a replacement, press `gm` to
mark both the old and new lines before pressing `Space`. The status bar
shows the shortcut when the diff cursor is on a paired line. Select the
file under **Staged** to check the result, then press `c` to commit.

| Key | Action |
|---|---|
| `Tab` | next pane |
| `j` `k` | move |
| `Space` | stage the line, or unstage it on a staged file |
| `v` | select a range |
| `gm` | mark both sides of a replacement |
| `e` | open the file in your editor at the line |
| `c` | commit (with the message pane's message, if you wrote one) |
| `gc` | show or hide the message pane |
| `J` `K` | browse the commits of your branch |
| `f` | make a fixup! commit for an older commit |
| `t` | move the lines into another commit now |
| `s` | split a commit |
| `u` | undo |
| `?` | all keys |

The diff pane shows the unstaged changes of a file in Unstaged and the
staged changes of a file in Staged. Space moves a line from one to the
other.

The **Series** pane shows your commits since the upstream branch. With
no upstream set, it shows commits absent from all remote-tracking
branches. To choose a base for a session, run `git carve origin/main..`
with the appropriate ref.

## More keys

`?` lists every action with its current key. The most useful ones beyond
the quick start:

| Key | Action |
|---|---|
| `]` `[` | next or previous hunk |
| `}` `{` | next or previous file |
| `/` `n` `N` | search all diffs (in the Files pane `/` filters the list) |
| `:N` | go to line N |
| `a` | select the hunk |
| `m` | mark lines that are not next to each other |
| `Enter` | stage or unstage the whole file, or all files of a section row |
| `d` | discard lines from the work tree |
| `:s/a/b/` | replace in the selected lines, as in vim |
| `O` `T` | in a conflict, take ours or theirs |
| `A` | amend |
| `r` | reword a commit |
| `F` | make an amend! commit with a new message |
| `S` | squash the fixup! and amend! commits |
| `Ctrl-S` | sign-off on or off |
| `ga` | absorb fixes into their commits |
| `ZZ` | finish what is in progress |
| `X` | give up what is in progress |
| `D` | show the whole commit |
| `+` `-` | more or fewer context lines |
| `W` | show whole functions |
| `w` | ignore whitespace |
| `o` | zoom the pane |
| `Ctrl-W` | resize the panes with `h` `j` `k` `l` |
| `gS` | hide the Series pane |
| `F5` | read everything again |

You can rebind any key in git config. `git carve keys` lists the action
names.

```ini
[carve "keys"]
	stage = y Space
	quit = Ctrl-Q
```

## Performance

In a Linux v7.2 checkout with one changed file, git-carve took
**0.34 s** to show its first screen and **0.19 s** to stage one line
on a MacBook Pro M2 (median of five runs). See [benchmarks](docs/KPI.md)
for measurements with about 15,000 changed files, including bulk staging.

## Configuration

Set these with `git config [--global] carve.<name> <value>`.

| Setting | Default | What it does |
|---|---|---|
| `carve.signoff` | `false` | sign off every commit |
| `carve.keepChangeId` | `first` | which piece of a split keeps the Gerrit `Change-Id` (`first`, `last` or `none`) |
| `carve.suggestPrefix` | `true` | start commit messages with the usual subsystem prefix of the files |
| `carve.untrackedFiles` | `async` | look for untracked files in the background, or `no` |
| `carve.narrowWidth` | `100` | below this many columns the panes stack |
| `carve.layout` | `one` | `one` shows one diff pane for the cursor's section, `two` shows the unstaged and the staged diff |
| `carve.messagePane` | `true` | show the commit message pane at the start (`gc` toggles it) |
| `carve.seriesPane` | `true` | set to `false` to start with the Series pane hidden |
| `carve.seriesStep` | `5` | how many older commits the Series pane loads at a time |
| `carve.tabWidth` | `8` | tab width |
| `carve.asciiLines` | `false` | plain ASCII frames, for a broken locale over ssh |
| `carve.colors` | `8` | `256` uses a fixed palette for dark terminal backgrounds |
| `carve.escDelay` | `100` | how many ms Esc waits for the rest of a key, more for a slow link |
| `carve.diffLimit` | `100000` | the most lines a diff pane loads (`0` for no limit) |
| `carve.maxFileSize` | `5m` | larger new or deleted files are not diffed |
| `carve.skipDiff` | tag files | files that are never diffed or searched |
| `carve.absorbCopies` | `false` | let absorb follow lines copied from other files (slower) |
| `carve.undoLimit` | `50` | how many undo steps are kept (`0` turns undo off) |
| `carve.keys.<action>` | | the keys for an action (`none` unbinds it) |

`NO_COLOR` turns colours off. See `git carve --help` for details.

## How it works

git-carve runs the git binary for everything, with options that make
its output the same whatever your config says. A selection of lines
becomes a small patch that git applies to the index. Changes to
older commits run a rebase and check the trees afterwards. Undo steps
and everything else git-carve keeps are stored as refs, and `git gc`
leaves them alone. If an operation gets interrupted, git-carve
notices what it left behind next time it starts and offers to clean it
up.

When git-carve refuses something, or git fails, a box in the middle of
the screen says why and what to do. A failing commit hook's output is
in it too. The next key only closes the box.

## Testing

`make check` runs unit tests, a test for every workflow, a fuzzer, tests
that drive the real TUI in tmux, and performance tests on a generated
50,000-file tree. CI runs it on Linux and macOS, also with sanitizers.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Limitations

- Binary files, renames, mode changes and deletions can only be staged
  as a whole file.
- Changing an older commit needs a branch without merges above it.
- Splitting an older commit, fixup-to and absorb wait until a rebase of
  your own is done.
- No Windows.

## License

MIT, see [LICENSE](LICENSE).

`.clang-format` is the Linux kernel's and keeps its GPL-2.0 license. It
is only a style setting for the build and is not part of the program.
