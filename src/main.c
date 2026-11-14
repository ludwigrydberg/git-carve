// SPDX-License-Identifier: MIT
/* git-carve, the TUI. The commands the tests drive are in git-carve-cli. */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "git.h"
#include "ui/ui.h"

static const char usage[] =
	"usage: git carve [<base>..]\n"
	"\n"
	"Carve your changes into a clean patch series: stage lines, split\n"
	"commits, move lines between the commits of your branch (<base>..HEAD,\n"
	"by default @{u}..HEAD). Press ? in it for the keys.\n"
	"\n"
	"   git carve keys      the keys and the actions they run\n"
	"   git carve --help    the manual (man git-carve)\n";

static const struct option options[] = {
	{ "help", no_argument, NULL, 'h' },
	{ "version", no_argument, NULL, 'V' },
	{ NULL, 0, NULL, 0 },
};

static void version(void)
{
	printf("git-carve version %s\n", CARVE_VERSION);
}

int main(int argc, char **argv)
{
	struct repo r = { 0 };
	const char *arg;
	int c, rc;

	signal(SIGPIPE, SIG_IGN);

	/* the options, with our messages instead of getopt's */
	opterr = 0;
	while ((c = getopt_long(argc, argv, "hV", options, NULL)) != -1) {
		switch (c) {
		case 'h':
			fputs(usage, stdout);
			return 0;
		case 'V':
			version();
			return 0;
		default:
			fprintf(stderr, "git-carve: unknown option '%s'\n\n%s",
				argv[optind - 1], usage);
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	/* at most one word, help, version, keys or <base>.. */
	arg = argc ? argv[0] : NULL;
	if (arg && !strcmp(arg, "help")) {
		fputs(usage, stdout);
		return 0;
	}
	if (arg && !strcmp(arg, "version")) {
		version();
		return 0;
	}
	if ((arg && strcmp(arg, "keys") && !strstr(arg, "..")) || argc > 1) {
		/* the first word that is not one of those */
		const char *bad =
			arg && (!strcmp(arg, "keys") || strstr(arg, "..")) ?
				argv[1] :
				arg;

		fprintf(stderr, "git-carve: unknown argument '%s'\n\n%s", bad,
			usage);
		return 1;
	}

	/*
	 * the version check comes after, since it remembers the git that
	 * passed
	 */
	if (repo_open(&r) || check_git_version_cached(&r)) {
		fprintf(stderr, "git-carve: %s\n", sb_str(&r.err));
		repo_release(&r);
		return 1;
	}

	if (!arg)
		rc = ui_main(&r, NULL);
	else if (!strcmp(arg, "keys"))
		rc = ui_print_keys(&r);
	else
		rc = ui_main(&r, arg); /* git carve <base>.. */

	if (r.warn.len)
		fprintf(stderr, "%s", r.warn.buf);
	if (rc)
		fprintf(stderr, "git-carve: %s\n",
			r.err.len ? r.err.buf : "failed");
	repo_release(&r);
	return rc ? 1 : 0;
}
