// SPDX-License-Identifier: MIT
/*
 * git-carve-cli, every operation of the TUI as a command. It is the
 * interface the tests drive, not installed by default and not kept stable.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "git.h"

/*
 * Messages of the core name the keys that continue or give up an operation
 * as {finish} and {abort}, and the TUI puts its keys there. Here they are
 * the commands.
 */
static void print_subst(const char *prefix, const char *s)
{
	static const struct {
		const char *from, *to;
	} subst[] = {
		{ "{finish}", "'git-carve-cli continue'" },
		{ "{abort}", "'git-carve-cli abort'" },
		{ "{undo}", "'git-carve-cli undo'" },
	};
	size_t i;

	fputs(prefix, stderr);
	while (*s) {
		for (i = 0; i < sizeof(subst) / sizeof(subst[0]); i++)
			if (!strncmp(s, subst[i].from, strlen(subst[i].from)))
				break;
		if (i < sizeof(subst) / sizeof(subst[0])) {
			fputs(subst[i].to, stderr);
			s += strlen(subst[i].from);
		} else {
			fputc(*s++, stderr);
		}
	}
}

int main(int argc, char **argv)
{
	struct repo r = { 0 };
	int rc;

	signal(SIGPIPE, SIG_IGN);

	if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") ||
	    !strcmp(argv[1], "help")) {
		cli_usage();
		return argc < 2;
	}
	if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "version")) {
		printf("git-carve-cli version %s\n", CARVE_VERSION);
		return 0;
	}

	if (repo_open(&r) || check_git_version_cached(&r)) {
		fprintf(stderr, "git-carve-cli: %s\n", sb_str(&r.err));
		repo_release(&r);
		return 1;
	}

	rc = cli_main(&r, argc - 1, argv + 1);

	if (r.warn.len)
		print_subst("", r.warn.buf);
	if (rc) {
		print_subst("git-carve-cli: ",
			    r.err.len ? r.err.buf : "failed");
		fputc('\n', stderr);
	}
	repo_release(&r);
	return rc ? 1 : 0;
}
