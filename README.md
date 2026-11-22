# git-carve

**Carve your changes into a patch series where every commit does one
thing.**

git-carve is a terminal UI for editing a patch series before review. It
stages single lines and splits commits. It moves changes into the commit
they belong in anywhere in the branch. You see the whole series while
you edit it.

![git carve showing the file list, commit message and diff for a driver change](docs/images/screenshot.png)

## What it is for

**A hunk mixes a fix and a cleanup.** Stage the fix line by line and
commit it. Then commit the cleanup.

**A reviewer wants a change in patch 2 of 5.** Make the change and press
`t` to move it into patch 2. Or press `f` for a `fixup!` commit and
squash all fixups with `S` before you send the next version.

**A commit does two things.** Press `s` and commit the pieces one at a
time. The split only finishes when the pieces add up to the original.

**You don't remember which commit a fix belongs in.** `ga` proposes the
commit that last touched the lines. You review the plan before anything
is rewritten.

**You got it wrong.** Press `u`. Every action that changes the index or
HEAD can be undone. That includes commits and splits and squashes.
Undoing a commit stages its changes again. Discarded lines are saved
first so `u` brings them back. A finished split or move is one undo
step. While it is in progress you can step back inside it or give it up
with `X`. The history is kept as refs so it survives a restart and
`git gc`.

Commit messages start with the subsystem prefix from the history of the
files. `Ctrl-S` toggles sign-off. A split keeps the Gerrit `Change-Id` in
one of its commits.

git-carve runs git with your config and hooks. After a rewrite it checks
the resulting trees and tries to restore the original state if a check
fails.

## See it work

![Staging one fix out of a hunk, writing its message, committing it, and moving another into an older commit](docs/images/demo.gif)

*One fix is staged out of a hunk, its message written in the message
pane and committed. Another moves into an older commit. `J` and `K`
browse the result.*

## How git-carve compares to other Git frontends

git-carve only edits and reviews patch series. It is not a full Git
client and Tig or Lazygit suit that better. Tig is a terminal Git
browser with line by line staging. Lazygit is a broader client that can
also stage lines and move changes between commits with custom patches.
([Tig](https://jonas.github.io/tig/) ·
[Lazygit](https://github.com/jesseduffield/lazygit))

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
frame, and its top line names the pane `Tab` goes to next. Put the
cursor on a changed line and press `Space` to stage it. For a replacement, press `gm` to
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
| `?` | the keys of this pane, `?` again for all keys |

The diff pane shows the unstaged changes of a file in Unstaged and the
staged changes of a file in Staged. Space moves a line from one to the
other.

The **Series** pane shows your commits since the upstream branch. With
no upstream set, it shows commits absent from all remote-tracking
branches. To choose a base for a session, run `git carve origin/main..`
with the appropriate ref.

## More keys

`?` lists the keys of the pane you are in, and `?` again every key. `/`
in the help searches them, and Enter runs the one under the cursor.
`D` shows the whole commit, its message, stat and diff. The most useful
keys beyond the quick start:

| Key | Action |
|---|---|
| `]` `[` | next or previous hunk |
| `}` `{` | next or previous file |
| `gD` | the diffs of all files in one view, scrolling from file to file (`}` and `{` jump between them) |
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
**0.18 s** to show its first screen and **0.21 s** to stage one line
on a MacBook Pro M2 (median of seven runs). See [benchmarks](docs/KPI.md)
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
| `carve.startPane` | `files` | the pane git carve starts in: `files`, `diff`, `series` or `message` |
| `carve.allFiles` | `true` | show the diffs of all files in one view at the start (`gD` toggles it) |
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
