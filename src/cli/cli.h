/* SPDX-License-Identifier: MIT */
#ifndef CARVE_CLI_H
#define CARVE_CLI_H

#include <stdbool.h>

struct repo;

/* Run a headless subcommand, argv[0] being its name. */
int cli_main(struct repo *r, int argc, char **argv);
/* git-carve-cli help, listing every command. */
void cli_usage(void);

#endif
