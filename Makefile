# SPDX-License-Identifier: MIT
# The git-carve build, described in README.md. On macOS set
#   PKG_CONFIG_PATH="$(brew --prefix ncurses)/lib/pkgconfig"

VERSION := 1.1.0
# On a release tag v$(VERSION) the version is "1.0.0", after it
# "1.0.0-3-gabc1234" (git describe without the v), and with no tag at all
# "1.0.0-gabc1234".
GIT_DESC := $(shell git describe --tags --match 'v[0-9]*' --dirty 2>/dev/null)
GIT_VERSION := $(shell git describe --always --dirty 2>/dev/null)
ifneq ($(GIT_DESC),)
FULL_VERSION := $(patsubst v%,%,$(GIT_DESC))
else ifneq ($(GIT_VERSION),)
FULL_VERSION := $(VERSION)-g$(GIT_VERSION)
else
FULL_VERSION := $(VERSION)
endif

CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
DESTDIR ?=

NCURSES_PC := $(shell $(PKG_CONFIG) --exists ncursesw && echo ncursesw || \
		($(PKG_CONFIG) --exists ncurses && echo ncurses))
ifeq ($(NCURSES_PC),)
$(warning pkg-config found neither ncursesw nor ncurses; guessing -lncursesw)
NCURSES_CFLAGS :=
NCURSES_LIBS := -lncursesw
else
NCURSES_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(NCURSES_PC))
NCURSES_LIBS := $(shell $(PKG_CONFIG) --libs $(NCURSES_PC))
endif

CFLAGS ?= -O2 -g
WERROR ?= 1
WARN := -Wall -Wextra -Wshadow -Wno-unused-parameter
ifeq ($(WERROR),1)
WARN += -Werror
endif
ALL_CFLAGS := -std=gnu11 $(WARN) $(CFLAGS) -DCARVE_VERSION='"$(FULL_VERSION)"'
ALL_LDFLAGS := $(LDFLAGS)

ifeq ($(COVERAGE),1)
ALL_CFLAGS += --coverage
ALL_LDFLAGS += --coverage
endif

ifeq ($(SANITIZE),1)
ALL_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -fno-sanitize-recover=undefined
ALL_LDFLAGS += -fsanitize=address,undefined
endif

CORE_SRCS := src/util.c src/git.c src/diff.c src/patch.c src/index.c \
	     src/status.c src/opstate.c src/rebase.c src/cdiff.c \
	     src/message.c \
	     src/split.c src/series.c src/undo.c src/conflict.c src/rename.c \
	     src/absorb.c src/gc.c src/subst.c \
	     src/move.c src/ops.c
UI_SRCS := $(wildcard src/ui/*.c)
CORE_OBJS := $(CORE_SRCS:.c=.o)
UI_OBJS := $(UI_SRCS:.c=.o)
HDRS := $(wildcard src/*.h src/ui/*.h src/cli/*.h)
# what clang-format and tools/style.pl check
STYLE_FILES = src/*.c src/*.h src/ui/*.c src/ui/*.h src/cli/*.c src/cli/*.h \
	      tests/unit/*.c tests/unit/*.h tests/fuzz/*.c

UNIT_SRCS := $(wildcard tests/unit/test_*.c)
UNIT_BINS := $(UNIT_SRCS:.c=)
FUZZ_BIN := tests/fuzz/fuzz

# Rebuild everything when flags change (e.g. SANITIZE=1 after a plain build).
FLAGS_STAMP := .build-flags
FLAGS_NOW := $(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) $(NCURSES_CFLAGS)
$(shell echo '$(FLAGS_NOW)' | cmp -s - $(FLAGS_STAMP) || \
	echo '$(FLAGS_NOW)' > $(FLAGS_STAMP))

# The stamp is written above while parsing, and make 3.81 (on macOS) may
# not see it as existing yet. It gets an empty rule. There are no built-in
# rules either, since a silent fallback to "cc -c" would build without our
# flags.
$(FLAGS_STAMP): ;
.SUFFIXES:

all: git-carve git-carve-cli

git-carve: src/main.o $(CORE_OBJS) $(UI_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ src/main.o $(UI_OBJS) $(CORE_OBJS) \
		$(ALL_LDFLAGS) $(NCURSES_LIBS)

# The commands the tests drive, without curses and not installed by default.
git-carve-cli: src/cli/cli_main.o src/cli/cli.o $(CORE_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ src/cli/cli_main.o src/cli/cli.o $(CORE_OBJS) \
		$(ALL_LDFLAGS)

# src/%.o also matches src/ui/ui.o, and make 3.81 (on macOS) uses the first
# matching pattern rule. The ui rule must come first.
src/ui/%.o: src/ui/%.c $(HDRS) $(FLAGS_STAMP)
	$(CC) $(ALL_CFLAGS) $(NCURSES_CFLAGS) -c -o $@ $<

src/cli/%.o: src/cli/%.c $(HDRS) $(FLAGS_STAMP)
	$(CC) $(ALL_CFLAGS) -Isrc -c -o $@ $<

src/%.o: src/%.c $(HDRS) $(FLAGS_STAMP)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

# test_keys needs the TUI key map (curses headers, no curses calls)
tests/unit/test_keys: tests/unit/test_keys.c tests/unit/unit.h src/ui/keys.o \
		$(CORE_OBJS) $(HDRS)
	$(CC) $(ALL_CFLAGS) $(NCURSES_CFLAGS) -Isrc -o $@ $< src/ui/keys.o \
		$(CORE_OBJS) $(ALL_LDFLAGS)

tests/unit/test_%: tests/unit/test_%.c tests/unit/unit.h $(CORE_OBJS) $(HDRS)
	$(CC) $(ALL_CFLAGS) -Isrc -o $@ $< $(CORE_OBJS) $(ALL_LDFLAGS)

$(FUZZ_BIN): tests/fuzz/fuzz.c $(CORE_OBJS) $(HDRS)
	$(CC) $(ALL_CFLAGS) -Isrc -o $@ $< $(CORE_OBJS) $(ALL_LDFLAGS)

# The tests, then lint and the static analysis (a tool that is missing
# says SKIP). check-<stage> runs one stage of tests.
check: check-unit check-scenario check-fuzz check-ui check-perf
	@sh tools/quality.sh lint
	@sh tools/quality.sh analyze

check-unit: $(UNIT_BINS)
	@sh tests/run.sh unit $(UNIT_BINS)

check-scenario: git-carve git-carve-cli
	@sh tests/run.sh scenario tests/scenario/*.sh

check-fuzz: git-carve git-carve-cli $(FUZZ_BIN)
	@FUZZ_ITERS=$${FUZZ_ITERS:-400} sh tests/run.sh fuzz tests/fuzz/*.sh

check-ui: git-carve git-carve-cli
	@sh tests/run.sh ui tests/ui/*.sh

check-perf: git-carve git-carve-cli
	@JOBS=1 sh tests/run.sh perf tests/perf/*.sh

# The KPI suite on a huge change set (tests/perf/kpi.sh) compares timings
# and git processes with the baseline. kpi-save makes the results the
# baseline.
kpi: git-carve git-carve-cli
	@KPI_FULL=1 sh tests/perf/kpi.sh

kpi-save: git-carve git-carve-cli
	@KPI_FULL=1 KPI_SAVE=1 sh tests/perf/kpi.sh

fuzz: git-carve git-carve-cli $(FUZZ_BIN)
	@FUZZ_ITERS=$${FUZZ_ITERS:-5000} sh tests/run.sh fuzz tests/fuzz/*.sh

install: git-carve
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)/man1
	cp git-carve $(DESTDIR)$(BINDIR)/git-carve
	chmod 755 $(DESTDIR)$(BINDIR)/git-carve
	cp docs/git-carve.1 $(DESTDIR)$(MANDIR)/man1/git-carve.1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/git-carve.1

# Only for scripting against the internal commands.
install-cli: git-carve-cli
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)/man1
	cp git-carve-cli $(DESTDIR)$(BINDIR)/git-carve-cli
	chmod 755 $(DESTDIR)$(BINDIR)/git-carve-cli
	cp docs/git-carve-cli.1 $(DESTDIR)$(MANDIR)/man1/git-carve-cli.1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/git-carve-cli.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/git-carve $(DESTDIR)$(BINDIR)/git-carve-cli \
		$(DESTDIR)$(MANDIR)/man1/git-carve.1 \
		$(DESTDIR)$(MANDIR)/man1/git-carve-cli.1

# Source tarball of the committed tree.
dist:
	git archive --format=tar.gz --prefix=git-carve-$(VERSION)/ \
		-o git-carve-$(VERSION).tar.gz HEAD

# Local quality checks (tools/quality.sh): SKIP when a tool is missing.
lint analyze coverage fuzz-long:
	@sh tools/quality.sh $@

format:
	clang-format -i $(STYLE_FILES)

clean:
	rm -f git-carve git-carve-cli src/*.o src/ui/*.o src/cli/*.o $(UNIT_BINS) \
		$(FUZZ_BIN) $(FLAGS_STAMP) src/*.gc?? src/ui/*.gc?? src/cli/*.gc?? \
		tests/*/*.gc??
	rm -rf tests/unit/*.dSYM tests/fuzz/*.dSYM *.dSYM

.PHONY: all check check-unit check-scenario check-fuzz check-ui check-perf kpi kpi-save fuzz install install-cli uninstall dist format clean lint analyze \
	coverage fuzz-long
