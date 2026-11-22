// SPDX-License-Identifier: MIT
#include "cli.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "absorb.h"
#include "conflict.h"
#include "diff.h"
#include "gc.h"
#include "git.h"
#include "index.h"
#include "patch.h"
#include "rename.h"
#include "move.h"
#include "series.h"
#include "split.h"
#include "message.h"
#include "subst.h"
#include "undo.h"
#include "opstate.h"
#include "ops.h"
#include "status.h"
#include "rebase.h"
#include "cdiff.h"

#define OUTSIDE_MSG "'%s' is outside the work tree"

static const char usage_text[] =
	"usage: git-carve-cli <command> [<args>]\n"
	"\n"
	"Every operation of the git carve TUI as a command: the interface the\n"
	"tests drive. Not installed by default, not kept stable.\n"
	"\n"
	"<sel> names lines of the diff that 'git-carve-cli diff' shows: n258 (new\n"
	"side), o12 (old side), n250-260, o12,n12.\n"
	"\n"
	"Looking at changes\n"
	"   diff [--cached] <path>           annotated diff with old/new line numbers\n"
	"   show <commit> [<path>]           the same for a commit (no path: its files)\n"
	"   renames                          staged and work-tree renames\n"
	"\n"
	"Staging\n"
	"   stage <path> <sel>               stage lines\n"
	"   unstage <path> <sel>             unstage lines (numbers from diff --cached)\n"
	"   stage-file <path>...             stage whole files\n"
	"   unstage-file <path>...           unstage whole files\n"
	"   set-staged <path> <sel> <file>   stage your own version of those lines\n"
	"   stage-move <from> <to>           stage only the move of a rename\n"
	"   unstage-move <from> <to>         unstage a rename, both halves\n"
	"\n"
	"Changing the work tree\n"
	"   discard <path> <sel>             throw unstaged lines away\n"
	"   discard-file <path>...           throw a file's unstaged changes away\n"
	"   substitute <path> <sel> s/a/b/   replace text in lines (--all: the file)\n"
	"   remove <path>...                 delete a file and stage the deletion\n"
	"   ignore <path>...                 never list an untracked file again\n"
	"   resolve --ours|--theirs <path>   take one side of a conflict\n"
	"\n"
	"Committing\n"
	"   commit [-m <msg>] [--amend] [-s] commit what is staged\n"
	"   commit --draft                   commit with the message being written\n"
	"   message [--set <file>|--clear|--template]\n"
	"                                    the message being written (TUI pane)\n"
	"   prefix                           the subsystem prefix for the staged files\n"
	"   undo [--list]                    undo the last change to index or HEAD\n"
	"\n"
	"Splitting and moving changes\n"
	"   split [<commit>]                 split HEAD or an older commit\n"
	"   split --finish|--abort|--status  the split in progress\n"
	"   fixup [--amend [-m <msg>]] <commit> a fixup! (amend!) commit of the staged\n"
	"                                    changes, for squash\n"
	"   squash [--dry-run] [<base>]      squash the fixup!/amend! commits into theirs\n"
	"   fixup-to <commit> <path> <sel>   move unstaged lines into an older commit\n"
	"   defer <commit> <path> <sel>      move lines of a commit into a new one\n"
	"   move <commit> <path> [<sel>] --out|--into <c>|--new-after <c>|--new-before <c>\n"
	"                                    lines of any commit: out, into another, new\n"
	"   drop <commit> <path> [<sel>]     take lines out of a commit (and the files)\n"
	"   edit <commit> <path> <sel> <file> the lines of a commit as file has them\n"
	"   reword <commit> [-m <msg>]       a new message for any commit\n"
	"   absorb [--apply] [<range>]       move each change to the commit it fixes\n"
	"   blame [--cached] <path> [<sel>]  the commits that last touched the lines\n"
	"   continue | abort | status        the operation in progress\n"
	"\n"
	"Housekeeping\n"
	"   gc [--force]                     remove what git carve left behind\n"
	"   version                          the version of git-carve-cli\n";

void cli_usage(void)
{
	fputs(usage_text, stdout);
}

/*
 * The physical path of arg, its directory resolved with realpath() since
 * the file itself may not exist. NULL if that fails.
 */
static char *physical_path(const char *arg)
{
	const char *slash = strrchr(arg, '/');
	char *dir = xstrndup(arg, slash == arg ? 1 : (size_t)(slash - arg));
	char buf[PATH_MAX], *res = NULL;

	if (realpath(dir, buf)) {
		struct strbuf sb = STRBUF_INIT;

		sb_addf(&sb, "%s/%s", strcmp(buf, "/") ? buf : "", slash + 1);
		res = sb_detach(&sb);
	}
	free(dir);
	return res;
}

/*
 * Turn a path given relative to the user's cwd into a path relative to
 * the top level (we chdir'ed there at startup). Handles "." and "..".
 */
static char *resolve_path(struct repo *r, const char *arg)
{
	struct strbuf full = STRBUF_INIT, out = STRBUF_INIT;
	size_t toplen = strlen(r->top);
	const char *p;

	if (arg[0] == '/') {
		char *real = NULL;

		if (strncmp(arg, r->top, toplen) ||
		    (arg[toplen] != '/' && arg[toplen])) {
			/* maybe through a symlink (/tmp on macOS) */
			real = physical_path(arg);
			if (!real || strncmp(real, r->top, toplen) ||
			    (real[toplen] != '/' && real[toplen])) {
				free(real);
				return NULL;
			}
			arg = real;
		}
		sb_adds(&full, arg + toplen);
		free(real);
	} else {
		sb_adds(&full, r->prefix);
		sb_adds(&full, arg);
	}
	p = full.buf;
	while (*p) {
		const char *slash = strchr(p, '/');
		size_t n;

		n = slash ? (size_t)(slash - p) : strlen(p);

		if (n == 0 || (n == 1 && p[0] == '.')) {
			/* skip */
		} else if (n == 2 && p[0] == '.' && p[1] == '.') {
			char *last;

			if (!out.len) {
				sb_release(&full);
				sb_release(&out);
				return NULL;
			}
			last = strrchr(out.buf, '/');
			out.len = last ? (size_t)(last - out.buf) : 0;
			out.buf[out.len] = '\0';
		} else {
			if (out.len)
				sb_addch(&out, '/');
			sb_add(&out, p, n);
		}
		p += n;
		if (*p == '/')
			p++;
	}
	sb_release(&full);
	if (!out.len) {
		sb_release(&out);
		return NULL;
	}
	return sb_detach(&out);
}

static char *path_arg(struct repo *r, const char *arg)
{
	char *p = resolve_path(r, arg);

	if (!p)
		repo_err(r, OUTSIDE_MSG, arg);
	return p;
}

static int usage_err(struct repo *r, const char *u)
{
	return repo_err(r, "usage: git carve %s", u);
}

struct opt {
	const char *name;  /* "--cached" */
	const char *alias; /* short form such as "-m", or NULL */
	const char **arg;  /* option takes an argument, stored here */
	bool *flag;	   /* or it is a flag */
};

/*
 * Split argv[1..] into options and positional arguments. Options may come
 * anywhere before "--", "--name=value" and "-m value" both work, and a
 * lone "-" is positional (stdin). Selections never start with "-" and
 * cannot be mistaken for options. Returns the number of positional
 * arguments stored in pos, at most max, or -1 with a usage error.
 */
static int parse_args(struct repo *r, int argc, char **argv,
		      const struct opt *opts, const char **pos, int max,
		      const char *usage)
{
	bool dashdash = false;
	int i, np = 0;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const struct opt *o;
		size_t nl = 0;

		if (dashdash || a[0] != '-' || !a[1]) {
			if (np == max)
				return usage_err(r, usage);
			pos[np++] = a;
			continue;
		}
		if (!strcmp(a, "--")) {
			dashdash = true;
			continue;
		}
		for (o = opts; o && o->name; o++) {
			nl = strlen(o->name);
			if (!strcmp(a, o->name) ||
			    (o->alias && !strcmp(a, o->alias)) ||
			    (o->arg && !strncmp(a, o->name, nl) &&
			     a[nl] == '=') ||
			    /*
			     * a short option with its value, such as -U5
			     * or -mmsg
			     */
			    (o->arg && o->alias && !o->alias[2] &&
			     !strncmp(a, o->alias, 2) && a[2]))
				break;
		}
		if (!o || !o->name) {
			if (a[1] == 'L') {
				repo_err(r,
					 "'%s': selections are now n<N> (new "
					 "side) and o<N> (old side), e.g. "
					 "o12,n12",
					 a);
				return -1;
			}
			repo_err(r, "unknown option '%s'; usage: git carve %s",
				 a, usage);
			return -1;
		}
		if (o->flag) {
			*o->flag = true;
		} else if (o->alias && !o->alias[2] && a[1] != '-' && a[2]) {
			*o->arg = a + 2;
		} else if (!strncmp(a, o->name, nl) && a[nl] == '=') {
			*o->arg = a + nl + 1;
		} else if (i + 1 < argc) {
			*o->arg = argv[++i];
		} else {
			repo_err(r, "option '%s' needs a value", a);
			return -1;
		}
	}
	return np;
}

static int cmd_diff(struct repo *r, int argc, char **argv)
{
	static const char u[] = "diff [--cached] [-U<n>] [-W] [-w] <path>";
	struct diff_opts dopts = DIFF_OPTS_INIT;
	struct strbuf out = STRBUF_INIT;
	const char *unified = NULL;
	struct fdiff *fd = NULL;
	bool cached = false;
	const struct opt opts[] = {
		{ "--cached", NULL, NULL, &cached },
		{ "--staged", NULL, NULL, &cached },
		{ "--unified", "-U", &unified, NULL },
		{ "--function-context", "-W", NULL, &dopts.function_context },
		{ "--ignore-all-space", "-w", NULL, &dopts.ignore_ws },
		{ NULL, NULL, NULL, NULL },
	};
	const char *pos[1];
	char *path, *src;
	int rc;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 1, u);

	if (np < 0)
		return -1;
	if (np != 1)
		return usage_err(r, u);
	if (unified &&
	    (parse_int(unified, NULL, &dopts.context) || dopts.context < 0))
		return repo_err(r, "-U needs a number of lines, not '%s'",
				unified);
	path = path_arg(r, pos[0]);
	if (!path)
		return -1;
	/* a rename is diffed against its old file, as it will be staged */
	src = cached ? staged_rename_source(r, path) :
		       worktree_rename_source(r, path);
	if (!src)
		rc = load_diff_opts(r, path, cached, &dopts, &fd);
	else if (cached)
		rc = staged_rename_diff(r, src, path, &dopts, &fd);
	else
		rc = rename_diff(r, src, path, &dopts, &fd);
	if (!rc && src)
		printf("# renamed from %s%s\n", src,
		       cached ? "" : " (the move is not staged yet)");
	if (!rc && !src && fd && fd->is_new) {
		char *c = copy_source(r, path, cached, fd);

		if (c)
			printf("# new file, a copy of %s\n", c);
		free(c);
	}
	if (!rc && fd) {
		diff_annotate(fd, &out);
		fwrite(out.buf, 1, out.len, stdout);
	}
	fdiff_free(fd);
	free(fd);
	free(path);
	free(src);
	sb_release(&out);
	return rc;
}

static int cmd_stage_common(struct repo *r, int argc, char **argv,
			    enum patch_dir dir)
{
	const char *u = dir == PATCH_STAGE ? "stage <path> <sel>" :
					     "unstage <path> <sel>";
	const char *pos[2];
	char *path;
	int rc, np = parse_args(r, argc, argv, NULL, pos, 2, u);

	if (np < 0)
		return -1;
	if (np != 2)
		return usage_err(r, u);
	path = path_arg(r, pos[0]);
	if (!path)
		return -1;
	rc = stage_lines(r, path, pos[1], dir);
	free(path);
	return rc;
}

static int cmd_stage(struct repo *r, int argc, char **argv)
{
	return cmd_stage_common(r, argc, argv, PATCH_STAGE);
}

static int cmd_unstage(struct repo *r, int argc, char **argv)
{
	return cmd_stage_common(r, argc, argv, PATCH_UNSTAGE);
}

static int cmd_file_common(struct repo *r, int argc, char **argv, bool stage)
{
	const char *u = stage ? "stage-file <path>..." :
				"unstage-file <path>...";
	const char **pos = xcalloc((size_t)argc + 1, sizeof(*pos));
	char **paths;
	int i, rc = 0, np = parse_args(r, argc, argv, NULL, pos, argc, u);

	if (np == 0)
		rc = usage_err(r, u);
	if (np <= 0)
		rc = -1;
	paths = xcalloc((size_t)(np > 0 ? np : 0) + 1, sizeof(*paths));
	for (i = 0; i < np && !rc; i++) {
		paths[i] = path_arg(r, pos[i]);
		if (!paths[i])
			rc = -1;
	}
	/* all the paths in one git */
	if (!rc && np > 0)
		rc = stage ? stage_files(r, (const char *const *)paths,
					 (size_t)np) :
			     unstage_files(r, (const char *const *)paths,
					   (size_t)np);
	for (i = 0; i < np; i++)
		free(paths[i]);
	free(paths);
	free(pos);
	return rc;
}

static int cmd_stage_file(struct repo *r, int argc, char **argv)
{
	return cmd_file_common(r, argc, argv, true);
}

static int cmd_unstage_file(struct repo *r, int argc, char **argv)
{
	return cmd_file_common(r, argc, argv, false);
}

static int cmd_substitute(struct repo *r, int argc, char **argv)
{
	struct strbuf err = STRBUF_INIT;
	struct line_range *rg = NULL;
	static const char u[] =
		"substitute <path> <sel> s/<pattern>/<replacement>/[gi] | "
		"substitute --all <path> s/<pattern>/<replacement>/[gi]";
	const char *pos[3];
	bool all = false;
	const struct opt opts[] = {
		{ "--all", NULL, NULL, &all },
		{ NULL, NULL, NULL, NULL },
	};
	int changed = -1;
	struct subst s;
	size_t n = 0;
	char *path;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 3, u);

	if (np < 0)
		return -1;
	if (np != (all ? 2 : 3))
		return usage_err(r, u);
	path = path_arg(r, pos[0]);
	if (!path)
		return -1;
	if (subst_parse(pos[np - 1], &s, &err) ||
	    (!all && subst_ranges(pos[1], &rg, &n, &err))) {
		repo_err(r, "%s", err.buf);
		goto out;
	}
	changed = subst_file(r, path, rg, n, &s);
	if (!changed)
		changed = repo_err(r, "no match in %s",
				   all ? "the file" : "those lines");
	else if (changed > 0)
		printf("%d line%s changed\n", changed, changed == 1 ? "" : "s");
out:
	subst_free(&s);
	free(rg);
	free(path);
	sb_release(&err);
	return changed < 0 ? -1 : 0;
}

static int cmd_discard(struct repo *r, int argc, char **argv)
{
	static const char u[] = "discard <path> <sel>";
	struct split_take t;
	const char *pos[2];
	char *path;
	int rc;
	int np;

	np = parse_args(r, argc, argv, NULL, pos, 2, u);

	if (np < 0)
		return -1;
	if (np != 2)
		return usage_err(r, u);
	path = path_arg(r, pos[0]);
	if (!path)
		return -1;
	rc = split_take_begin(r, path, &t);
	if (!rc && discard_lines(r, path, pos[1])) {
		split_take_release(&t);
		rc = -1;
	} else if (!rc) {
		rc = split_take_end(r, &t);
	}
	free(path);
	return rc;
}

static int cmd_remove(struct repo *r, int argc, char **argv)
{
	const char **pos = xcalloc((size_t)argc + 1, sizeof(*pos));
	static const char u[] = "remove <path>...";
	int rc = 0;
	int np;
	int i;

	np = parse_args(r, argc, argv, NULL, pos, argc, u);

	if (np == 0)
		rc = usage_err(r, u);
	if (np <= 0)
		rc = -1;
	if (!rc && !repo_rev_status(r, SPLIT_REF))
		rc = repo_err(r, SPLIT_BUSY_MOVE_MSG);
	for (i = 0; i < np && !rc; i++) {
		char *path = path_arg(r, pos[i]);

		if (!path) {
			rc = -1;
			break;
		}
		rc = remove_file(r, path);
		free(path);
	}
	free(pos);
	return rc;
}

static int cmd_ignore(struct repo *r, int argc, char **argv)
{
	const char **pos = xcalloc((size_t)argc + 1, sizeof(*pos));
	static const char u[] = "ignore <path>...";
	int rc = 0;
	int np;
	int i;

	np = parse_args(r, argc, argv, NULL, pos, argc, u);

	if (np == 0)
		rc = usage_err(r, u);
	if (np <= 0)
		rc = -1;
	for (i = 0; i < np && !rc; i++) {
		char *path = path_arg(r, pos[i]);

		if (!path) {
			rc = -1;
			break;
		}
		/* the exclude file only hides untracked files */
		if (path_in_index(r, path) > 0)
			rc = repo_err(r,
				      "%s is tracked: git rm --cached it "
				      "first",
				      path);
		else
			rc = exclude_path(r, path);
		free(path);
	}
	free(pos);
	return rc;
}

static int cmd_discard_file(struct repo *r, int argc, char **argv)
{
	const char **pos = xcalloc((size_t)argc + 1, sizeof(*pos));
	static const char u[] = "discard-file <path>...";
	struct split_take t;
	int rc = 0;
	int np;
	int i;

	np = parse_args(r, argc, argv, NULL, pos, argc, u);

	if (np == 0)
		rc = usage_err(r, u);
	if (np <= 0)
		rc = -1;
	for (i = 0; i < np && !rc; i++) {
		char *path = path_arg(r, pos[i]);

		if (!path) {
			rc = -1;
			break;
		}
		rc = split_take_begin(r, path, &t);
		if (!rc && discard_file(r, path)) {
			split_take_release(&t);
			rc = -1;
		} else if (!rc) {
			rc = split_take_end(r, &t);
		}
		free(path);
	}
	free(pos);
	return rc;
}

static int cmd_resolve(struct repo *r, int argc, char **argv)
{
	static const char u[] = "resolve --ours | --theirs <path> [<n>]";
	bool ours = false, theirs = false;
	const struct opt opts[] = {
		{ "--ours", NULL, NULL, &ours },
		{ "--theirs", NULL, NULL, &theirs },
		{ NULL, NULL, NULL, NULL },
	};
	struct undo_state ub;
	const char *pos[2];
	char *path;
	int n = 0;
	int rc;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 2, u);

	if (np < 0)
		return -1;
	if (np < 1 || ours == theirs)
		return usage_err(r, u);
	if (np == 2 && (parse_int(pos[1], NULL, &n) || n < 1))
		return repo_err(r, "'%s': conflicts are numbered from 1",
				pos[1]);
	path = path_arg(r, pos[0]);
	if (!path)
		return -1;
	if (undo_conflict_begin(r, &ub, path)) {
		free(path);
		return -1;
	}
	rc = resolve_conflict(r, path, n, theirs);
	undo_conflict_end(r, &ub, path);
	free(path);
	return rc;
}

static int cmd_prefix(struct repo *r, int argc, char **argv)
{
	char *p;

	if (parse_args(r, argc, argv, NULL, NULL, 0, "prefix") < 0)
		return -1;
	p = suggest_prefix(r);
	if (!p)
		return repo_err(r,
				"no prefix to suggest (nothing staged, or "
				"no prefixes in the history of these files)");
	printf("%s: \n", p);
	free(p);
	return 0;
}

static int cmd_gc(struct repo *r, int argc, char **argv)
{
	struct strbuf out = STRBUF_INIT;
	bool force = false;
	const struct opt opts[] = {
		{ "--force", NULL, NULL, &force },
		{ NULL, NULL, NULL, NULL },
	};
	int rc;

	if (parse_args(r, argc, argv, opts, NULL, 0, "gc [--force]") < 0)
		return -1;
	rc = carve_gc(r, force, &out);
	fputs(sb_str(&out), stdout);
	sb_release(&out);
	return rc;
}

/* The commit before rev (for --new-before), or NULL. */
static char *before_of(struct repo *r, const char *rev)
{
	struct strbuf spec = STRBUF_INIT;
	char *id;

	sb_addf(&spec, "%s^{commit}^", rev);
	id = repo_rev(r, spec.buf);
	sb_release(&spec);
	if (!id)
		repo_err(r, "%s has no commit before it", rev);
	return id;
}

static int cmd_move(struct repo *r, int argc, char **argv)
{
	const char *into = NULL, *after = NULL, *before = NULL, *m = NULL,
		   *file = NULL, *pos[3] = { NULL, NULL, NULL };
	struct strbuf msg = STRBUF_INIT, text = STRBUF_INIT;
	struct commit_opts co = { 0 };
	struct move_req req = { 0 };
	char *path, *dest = NULL;
	static const char u[] =
		"move <commit> <path> [<sel>] (--out | --into <commit> | "
		"--new-after <commit> | --new-before <commit>) "
		"[-m <msg> | -F <file>]";
	bool out = false;
	const struct opt opts[] = {
		{ "--out", NULL, NULL, &out },
		{ "--into", NULL, &into, NULL },
		{ "--new-after", NULL, &after, NULL },
		{ "--new-before", NULL, &before, NULL },
		{ "--message", "-m", &m, NULL },
		{ "--file", "-F", &file, NULL },
		{ NULL, NULL, NULL, NULL },
	};
	int rc;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 3, u);

	if (np < 0)
		return -1;
	if (np < 2 || out + !!into + !!after + !!before != 1 || (m && file))
		return usage_err(r, u);
	path = path_arg(r, pos[1]);
	if (!path)
		return -1;
	if (file && read_file(strcmp(file, "-") ? file : "/dev/stdin", &text)) {
		free(path);
		return repo_err(r, "cannot read %s", file);
	}
	co.message = file ? sb_str(&text) : m;
	co.quiet = true;
	co.signoff = config_signoff(r);
	req.how = out ? MOVE_OUT : into ? MOVE_INTO : MOVE_NEW;
	req.commit = pos[0];
	req.path = path;
	req.sel = pos[2];
	req.co = &co;
	if (before) {
		dest = before_of(r, before);
		if (!dest) {
			free(path);
			sb_release(&text);
			return -1;
		}
	}
	req.dest = into ? into : after ? after : dest;
	rc = move_lines(r, &req, &msg);
	if (rc == 0)
		printf("%s\n", msg.buf);
	free(path);
	free(dest);
	sb_release(&msg);
	sb_release(&text);
	return rc ? -1 : 0;
}

static int cmd_drop(struct repo *r, int argc, char **argv)
{
	static const char u[] = "drop <commit> <path> [<sel>]";
	const char *pos[3] = { NULL, NULL, NULL };
	struct strbuf msg = STRBUF_INIT;
	struct move_req req = { 0 };
	char *path;
	int rc;
	int np;

	np = parse_args(r, argc, argv, NULL, pos, 3, u);

	if (np < 0)
		return -1;
	if (np < 2)
		return usage_err(r, u);
	path = path_arg(r, pos[1]);
	if (!path)
		return -1;
	req.how = MOVE_DROP;
	req.commit = pos[0];
	req.path = path;
	req.sel = pos[2];
	rc = move_lines(r, &req, &msg);
	if (rc == 0)
		printf("%s\n", msg.buf);
	free(path);
	sb_release(&msg);
	return rc ? -1 : 0;
}

static int cmd_reword(struct repo *r, int argc, char **argv)
{
	static const char u[] = "reword <commit> [-m <msg> | -F <file>]";
	const char *m = NULL, *file = NULL, *pos[1] = { NULL };
	const struct opt opts[] = {
		{ "--message", "-m", &m, NULL },
		{ "--file", "-F", &file, NULL },
		{ NULL, NULL, NULL, NULL },
	};
	struct strbuf msg = STRBUF_INIT, text = STRBUF_INIT;
	struct commit_opts co = { 0 };
	struct move_req req = { 0 };
	int rc;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 1, u);

	if (np < 0)
		return -1;
	if (np != 1 || (m && file))
		return usage_err(r, u);
	if (file && read_file(strcmp(file, "-") ? file : "/dev/stdin", &text))
		return repo_err(r, "cannot read %s", file);
	co.message = file ? sb_str(&text) : m;
	co.quiet = true;
	req.how = MOVE_REWORD;
	req.commit = pos[0];
	req.co = &co;
	rc = move_lines(r, &req, &msg);
	if (rc == 0)
		printf("%s\n", msg.buf);
	sb_release(&msg);
	sb_release(&text);
	return rc ? -1 : 0;
}

static int cmd_edit(struct repo *r, int argc, char **argv)
{
	static const char u[] = "edit <commit> <path> <sel> <file> "
				"(- = stdin; --print: show the lines)";
	struct strbuf blob = STRBUF_INIT, text = STRBUF_INIT,
		      content = STRBUF_INIT, msg = STRBUF_INIT;
	const char *pos[4] = { NULL, NULL, NULL, NULL };
	struct region rg = { 0, 0, STRBUF_INIT };
	struct move_req req = { 0 };
	bool print = false;
	const struct opt opts[] = {
		{ "--print", NULL, NULL, &print },
		{ NULL, NULL, NULL, NULL },
	};
	int rc = -1;
	char *path;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 4, u);

	if (np < 0)
		return -1;
	if (np != (print ? 3 : 4))
		return usage_err(r, u);
	path = path_arg(r, pos[1]);
	if (!path)
		return -1;
	if (commit_region(r, pos[0], path, pos[2], &rg, &blob))
		goto out;
	if (print) {
		fwrite(rg.text.buf ? rg.text.buf : "", 1, rg.text.len, stdout);
		rc = 0;
		goto out;
	}
	if (read_file(strcmp(pos[3], "-") ? pos[3] : "/dev/stdin", &text)) {
		repo_err(r, "cannot read %s", pos[3]);
		goto out;
	}
	splice_lines(sb_str(&blob), blob.len, rg.start, rg.count, sb_str(&text),
		     text.len, &content);
	req.how = MOVE_EDIT;
	req.commit = pos[0];
	req.path = path;
	req.content = sb_str(&content);
	req.len = content.len;
	rc = move_lines(r, &req, &msg);
	if (rc == 0)
		printf("%s\n", msg.buf);
	rc = rc ? -1 : 0;
out:
	free(path);
	region_release(&rg);
	sb_release(&blob);
	sb_release(&text);
	sb_release(&content);
	sb_release(&msg);
	return rc;
}

static int cmd_absorb(struct repo *r, int argc, char **argv)
{
	static const char u[] = "absorb [--apply] [<range>]";
	struct strbuf out = STRBUF_INIT;
	const char *pos[1] = { NULL };
	struct absorb_hunk *v;
	bool apply = false;
	const struct opt opts[] = {
		{ "--apply", NULL, NULL, &apply },
		{ NULL, NULL, NULL, NULL },
	};
	int moved;
	size_t n;
	int rc;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 1, u);

	if (np < 0)
		return -1;
	if (absorb_plan(r, pos[0], &v, &n))
		return -1;
	/* what moves where, and why the others stay */
	absorb_describe(r, v, n, &out);
	fputs(sb_str(&out), stdout);
	sb_release(&out);
	if (!apply) {
		if (n)
			printf("(a dry run: --apply moves them)\n");
		else
			printf("nothing to absorb\n");
		absorb_plan_free(v, n);
		return 0;
	}
	rc = absorb_apply_plan(r, pos[0], v, n, &moved);
	absorb_plan_free(v, n);
	if (rc >= 0)
		printf("absorbed %d change%s\n", moved, moved == 1 ? "" : "s");
	return rc ? -1 : 0;
}

/*
 * The commits that last touched the lines of a change (the f and t
 * pickers' b): "<lines>/<blamed> <short id> <subject>", most first.
 */
static int cmd_blame(struct repo *r, int argc, char **argv)
{
	static const char u[] = "blame [--cached] <path> [<sel>]";
	struct strbuf err = STRBUF_INIT, names = STRBUF_INIT;
	const char *pos[2] = { NULL, NULL };
	struct blame_count *v = NULL;
	struct fdiff *fd = NULL;
	bool cached = false;
	const struct opt opts[] = {
		{ "--cached", NULL, NULL, &cached },
		{ NULL, NULL, NULL, NULL },
	};
	char **ids = NULL;
	size_t n = 0, i;
	int rc = -1;
	char *path;
	int total;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 2, u);

	if (np < 0)
		return -1;
	if (np < 1)
		return usage_err(r, u);
	path = path_arg(r, pos[0]);
	if (!path)
		return -1;
	if (load_diff(r, path, cached, &fd))
		goto out;
	if (!fd) {
		repo_err(r, "%s: no %schanges", path, cached ? "staged " : "");
		goto out;
	}
	if (pos[1] && sel_apply(fd, pos[1], &err) < 0) {
		repo_err(r, "%s", err.buf);
		goto out;
	}
	if (blame_change(r, fd, &v, &n, &total))
		goto out;
	ids = xcalloc(n + 1, sizeof(*ids));
	for (i = 0; i < n; i++)
		ids[i] = v[i].id;
	if (repo_log_commits(r, "%H %h %s", ids, n, &names))
		goto out;
	for (i = 0; i < n; i++) {
		const char *p = strstr(sb_str(&names), v[i].id);
		const char *s;

		s = p ? p + strlen(v[i].id) + 1 : v[i].id;

		printf("%d/%d %.*s\n", v[i].n, total, (int)strcspn(s, "\n"), s);
	}
	rc = 0;
out:
	free(ids);
	blame_counts_free(v, n);
	fdiff_free(fd);
	free(fd);
	free(path);
	sb_release(&err);
	sb_release(&names);
	return rc;
}

static int cmd_renames(struct repo *r, int argc, char **argv)
{
	struct file_status *fs = NULL;
	struct rename *v = NULL;
	size_t n = 0, nv = 0, i;
	int rc;

	if (parse_args(r, argc, argv, NULL, NULL, 0, "renames") < 0)
		return -1;
	if (load_status(r, &fs, &n))
		return -1;
	rc = find_renames(r, fs, n, &v, &nv);
	for (i = 0; !rc && i < nv; i++)
		printf("R%03d %s -> %s (%s)\n", v[i].score, v[i].from, v[i].to,
		       v[i].staged ? "staged" : "in the work tree");
	free_renames(v, nv);
	free_status(fs, n);
	return rc;
}

static int cmd_move_common(struct repo *r, int argc, char **argv, bool stage)
{
	const char *u = stage ? "stage-move <from> <to>" :
				"unstage-move <from> <to>";
	const char *pos[2];
	char *from, *to;
	int rc = -1, np = parse_args(r, argc, argv, NULL, pos, 2, u);

	if (np < 0)
		return -1;
	if (np != 2)
		return usage_err(r, u);
	from = path_arg(r, pos[0]);
	to = from ? path_arg(r, pos[1]) : NULL;
	if (to)
		rc = stage ? stage_move(r, from, to) :
			     unstage_move(r, from, to);
	free(from);
	free(to);
	return rc;
}

static int cmd_stage_move(struct repo *r, int argc, char **argv)
{
	return cmd_move_common(r, argc, argv, true);
}

static int cmd_unstage_move(struct repo *r, int argc, char **argv)
{
	return cmd_move_common(r, argc, argv, false);
}

static int cmd_set_staged(struct repo *r, int argc, char **argv)
{
	struct strbuf content = STRBUF_INIT, show = STRBUF_INIT;
	bool cached = false, print = false;
	const struct opt opts[] = {
		{ "--cached", NULL, NULL, &cached },
		{ "--print", NULL, NULL, &print },
		{ NULL, NULL, NULL, NULL },
	};
	static const char u[] =
		"set-staged [--cached] [--print] <path> <sel> [<file>]";
	const char *pos[3];
	char *path;
	int rc;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 3, u);

	if (np < 0)
		return -1;
	if (np != (print ? 2 : 3))
		return usage_err(r, u);
	if (!print) {
		const char *f = pos[2];

		if (read_file(strcmp(f, "-") ? f : "/dev/stdin", &content)) {
			sb_release(&content);
			return repo_err(r, "cannot read %s", f);
		}
	}
	path = path_arg(r, pos[0]);
	if (!path) {
		sb_release(&content);
		return -1;
	}
	rc = set_staged(r, path, pos[1], cached,
			print ? NULL : sb_str(&content), content.len, &show);
	if (!rc && print)
		fwrite(sb_str(&show), 1, show.len, stdout);
	free(path);
	sb_release(&content);
	sb_release(&show);
	return rc;
}

/*
 * The commit message being written (the TUI's message pane): printed,
 * set from a file (- for stdin), cleared, or the text it starts from.
 */
static int cmd_message(struct repo *r, int argc, char **argv)
{
	static const char u[] = "message [--set <file> | --clear | --template]";
	bool clear = false, tmpl = false;
	struct strbuf t = STRBUF_INIT;
	const char *set = NULL;
	const struct opt opts[] = {
		{ "--set", NULL, &set, NULL },
		{ "--clear", NULL, NULL, &clear },
		{ "--template", NULL, NULL, &tmpl },
		{ NULL, NULL, NULL, NULL },
	};
	int np, rc = 0;

	np = parse_args(r, argc, argv, opts, NULL, 0, u);
	if (np < 0)
		return -1;
	if (!!set + clear + tmpl > 1)
		return usage_err(r, u);
	if (clear) {
		draft_remove(r);
	} else if (tmpl) {
		rc = commit_template(r, config_signoff(r), &t);
		fputs(sb_str(&t), stdout);
	} else if (set) {
		rc = read_file(strcmp(set, "-") ? set : "/dev/stdin", &t) ?
			     repo_err(r, "cannot read %s", set) :
			     draft_write(r, t.buf, t.len);
	} else {
		rc = draft_read(r, &t);
		fputs(sb_str(&t), stdout);
	}
	sb_release(&t);
	return rc;
}

static int cmd_commit(struct repo *r, int argc, char **argv)
{
	static const char u[] = "commit [-m <msg> | -F <file> | --draft] "
				"[--amend] [-s | --no-signoff]";
	bool signoff = false, no_signoff = false, draft = false;
	struct commit_opts o = { 0 };
	const struct opt opts[] = {
		{ "--message", "-m", &o.message, NULL },
		{ "--file", "-F", &o.file, NULL },
		{ "--amend", NULL, NULL, &o.amend },
		{ "--signoff", "-s", NULL, &signoff },
		{ "--no-signoff", NULL, NULL, &no_signoff },
		{ "--draft", NULL, NULL, &draft },
		{ NULL, NULL, NULL, NULL },
	};
	int np;

	np = parse_args(r, argc, argv, opts, NULL, 0, u);

	if (np < 0)
		return -1;
	if ((o.message && o.file) || (signoff && no_signoff) ||
	    (draft && (o.message || o.file || o.amend)))
		return usage_err(r, u);
	o.signoff = signoff || (!no_signoff && config_signoff(r));
	return draft ? commit_draft(r, &o) : carve_commit(r, &o);
}

/* "  <short> <subject>" for each commit of ids, in one git log. */
static int print_commits(struct repo *r, const struct strbuf *ids)
{
	const char *pos = sb_str(ids), *end = pos + ids->len, *l;
	struct strbuf out = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	size_t len;

	while ((l = next_line(&pos, end, &len))) {
		char *id = xstrndup(l, len);

		argv_push(&a, id);
		free(id);
	}
	if (!repo_log_commits(r, "  %h %s", a.v, a.n, &out))
		fputs(sb_str(&out), stdout);
	argv_clear(&a);
	sb_release(&out);
	return 0;
}

static int cmd_split(struct repo *r, int argc, char **argv)
{
	bool finish = false, abrt = false, force = false, status = false;
	const struct opt opts[] = {
		{ "--finish", NULL, NULL, &finish },
		{ "--abort", NULL, NULL, &abrt },
		{ "--force", NULL, NULL, &force },
		{ "--status", NULL, NULL, &status },
		{ NULL, NULL, NULL, NULL },
	};
	struct strbuf sb = STRBUF_INIT;
	static const char u[] =
		"split [<commit>] | --finish | --abort [--force] | --status";
	struct split_state st;
	const char *pos[1];
	int rc;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 1, u);

	if (np < 0)
		return -1;
	if (finish + abrt + status > 1 || (force && !abrt) ||
	    (np && (finish || abrt || status)))
		return usage_err(r, u);
	if (status) {
		split_state_load(r, &st);
		if (!st.active) {
			printf("no split in progress\n");
		} else {
			split_status_line(&st, &sb);
			printf("%s\n", sb.buf);
		}
		split_state_release(&st);
		sb_release(&sb);
		return 0;
	}
	if (finish) {
		rc = split_finish(r, &sb);
		if (!rc) {
			int n = (int)count_lines(sb.buf, sb.len);

			printf("split finished: %d commit%s\n", n,
			       n == 1 ? "" : "s");
			print_commits(r, &sb);
			if (foreign_rebase(r))
				printf("your rebase is still in progress: "
				       "git rebase --continue goes on\n");
		}
		sb_release(&sb);
		return rc;
	}
	if (abrt) {
		rc = split_abort(r, force);
		if (rc == 1)
			printf("split forgotten; HEAD, index and work tree "
			       "are as they were\n");
		else if (!rc)
			printf("split aborted; HEAD is the original commit "
			       "again\n");
		return rc > 0 ? 0 : rc;
	}
	rc = split_start(r, np ? pos[0] : NULL);
	if (rc)
		return rc;
	split_state_load(r, &st);
	fprintf(stderr, "%s", sb_str(&r->warn));
	sb_reset(&r->warn);
	printf("splitting %.7s \"%s\": its changes are now unstaged.\n"
	       "Stage and commit them piece by piece (git-carve-cli commit), "
	       "then run\n'git-carve-cli split --finish'.\n",
	       st.orig, st.subject);
	if (st.in_rebase)
		printf("(In your rebase: continue it after the finish.)\n");
	split_state_release(&st);
	return 0;
}

static int cmd_show(struct repo *r, int argc, char **argv)
{
	static const char u[] = "show <commit> [<path>]";
	struct strbuf out = STRBUF_INIT;
	struct fdiff *fd = NULL;
	const char *pos[2];
	char *path;
	int rc;
	int np;

	np = parse_args(r, argc, argv, NULL, pos, 2, u);

	if (np < 0)
		return -1;
	if (np == 1) {
		/* the files the commit changes, as the Commit section lists them */
		struct cfile *f;
		size_t n, i;

		if (commit_files(r, pos[0], &f, &n))
			return -1;
		for (i = 0; i < n; i++)
			if (f[i].from)
				printf("%c\t%s\t%s\n", f[i].status, f[i].from,
				       f[i].path);
			else
				printf("%c\t%s\n", f[i].status, f[i].path);
		cfiles_free(f, n);
		return 0;
	}
	if (np != 2)
		return usage_err(r, u);
	path = path_arg(r, pos[1]);
	if (!path)
		return -1;
	{
		const char *from = NULL;
		/* for a rename, the diff between its old and new content */
		struct cfile *f = NULL;
		size_t n = 0, i;

		if (!commit_files(r, pos[0], &f, &n))
			for (i = 0; i < n; i++)
				if (f[i].from && !strcmp(f[i].path, path))
					from = f[i].from;
		rc = load_commit_diff_from(r, pos[0], path, from, NULL, &fd);
		cfiles_free(f, n);
	}
	if (!rc && fd) {
		diff_annotate(fd, &out);
		fwrite(out.buf, 1, out.len, stdout);
	}
	fdiff_free(fd);
	free(fd);
	free(path);
	sb_release(&out);
	return rc;
}

/*
 * 0 when done, 1 when stopped with a message in r->err, -1 on error. Both
 * of the last exit non-zero.
 */
static int series_result(struct repo *r, int rc, const char *done)
{
	if (rc == 0 && done)
		printf("%s\n", done);
	return rc ? -1 : 0;
}

static int cmd_fixup(struct repo *r, int argc, char **argv)
{
	static const char u[] = "fixup <commit> | "
				"fixup --amend [-m <msg> | -F <file>] <commit>";
	struct fixup_opts o = { 0 };
	const struct opt opts[] = {
		{ "--amend", NULL, NULL, &o.amend },
		{ "--message", "-m", &o.message, NULL },
		{ "--file", "-F", &o.file, NULL },
		{ NULL, NULL, NULL, NULL },
	};
	const char *pos[1];
	int np;

	np = parse_args(r, argc, argv, opts, pos, 1, u);

	if (np < 0)
		return -1;
	if (np != 1 || (o.message && o.file) ||
	    ((o.message || o.file) && !o.amend))
		return usage_err(r, u);
	return series_result(r, fixup_commit(r, pos[0], &o),
			     o.amend ? "made an amend! commit" :
				       "made a fixup! commit");
}

static int cmd_squash(struct repo *r, int argc, char **argv)
{
	static const char u[] = "squash [--dry-run] [<base>]";
	const char *pos[1] = { NULL };
	struct squash_plan p;
	bool dry = false;
	const struct opt opts[] = {
		{ "--dry-run", "-n", NULL, &dry },
		{ NULL, NULL, NULL, NULL },
	};
	size_t i;
	int rc;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 1, u);

	if (np < 0)
		return -1;
	if (squash_plan(r, np ? pos[0] : NULL, &p))
		return -1;
	/* each fixup and where it goes */
	for (i = p.first; i < p.n; i++)
		if (p.c[i].kind)
			printf("%.7s %s -> %.7s %s\n", p.c[i].id,
			       p.c[i].subject, p.c[p.c[i].target].id,
			       p.c[p.c[i].target].subject);
	if (p.left.len)
		printf("kept (no commit of theirs in the series):\n%s",
		       p.left.buf);
	if (dry) {
		squash_plan_release(&p);
		return 0;
	}
	rc = squash_run(r, &p);
	if (!rc)
		printf("squashed %d fixup%s into %d commit%s\n", p.nfix,
		       p.nfix == 1 ? "" : "s", p.ntargets,
		       p.ntargets == 1 ? "" : "s");
	squash_plan_release(&p);
	return rc ? -1 : 0;
}

static int cmd_fixup_to(struct repo *r, int argc, char **argv)
{
	static const char u[] = "fixup-to <commit> <path> <sel> | "
				"fixup-to --cached <commit>";
	bool cached = false;
	const struct opt opts[] = {
		{ "--cached", NULL, NULL, &cached },
		{ NULL, NULL, NULL, NULL },
	};
	const char *pos[3];
	char *path;
	int rc;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 3, u);

	if (np < 0)
		return -1;
	if (cached && np == 1)
		return series_result(r, fixup_to(r, pos[0], false),
				     "moved the staged changes");
	if (cached || np != 3)
		return usage_err(r, u);
	if (have_staged(r))
		return repo_err(r, "the index already has staged changes; "
				   "commit them, unstage them, or use "
				   "fixup-to --cached");
	path = path_arg(r, pos[1]);
	if (!path)
		return -1;
	rc = stage_lines(r, path, pos[2], PATCH_STAGE);
	free(path);
	if (rc)
		return -1;
	return series_result(r, fixup_to(r, pos[0], true),
			     "moved the selection");
}

static int cmd_defer(struct repo *r, int argc, char **argv)
{
	const char *after = NULL, *msg = NULL, *pos[3];
	const struct opt opts[] = {
		{ "--after", NULL, &after, NULL },
		{ "--message", "-m", &msg, NULL },
		{ NULL, NULL, NULL, NULL },
	};
	static const char u[] =
		"defer <commit> <path> <sel> [--after <commit>] [-m <msg>]";
	char *path;
	int rc;
	int np;

	np = parse_args(r, argc, argv, opts, pos, 3, u);

	if (np < 0)
		return -1;
	if (np != 3)
		return usage_err(r, u);
	path = path_arg(r, pos[1]);
	if (!path)
		return -1;
	rc = defer_lines(r, pos[0], path, pos[2], after, msg);
	free(path);
	return series_result(r, rc, "deferred the selection into a new commit");
}

static int cmd_continue(struct repo *r, int argc, char **argv)
{
	int np = parse_args(r, argc, argv, NULL, NULL, 0, "continue");
	struct split_state sp;

	if (np < 0)
		return -1;
	split_state_load(r, &sp);
	if (sp.active) {
		split_state_release(&sp);
		return repo_err(r, "a split is in progress; finish it with "
				   "'git-carve-cli split --finish'");
	}
	return series_result(r, op_continue(r), "done");
}

static int cmd_abort(struct repo *r, int argc, char **argv)
{
	struct split_state sp;
	bool force = false;
	const struct opt opts[] = {
		{ "--force", NULL, NULL, &force },
		{ NULL, NULL, NULL, NULL },
	};
	int rc, np = parse_args(r, argc, argv, opts, NULL, 0,
				"abort [--force]");

	if (np < 0)
		return -1;
	split_state_load(r, &sp);
	if (sp.active) {
		split_state_release(&sp);
		rc = split_abort(r, force);
		if (rc == 1) {
			printf("split forgotten; HEAD, index and work "
			       "tree are as they were\n");
			return 0;
		}
	} else {
		rc = op_abort(r);
	}
	if (!rc)
		printf("aborted\n");
	return rc;
}

static int cmd_status(struct repo *r, int argc, char **argv)
{
	struct strbuf sb = STRBUF_INIT;
	struct series_state st;
	struct split_state sp;

	if (parse_args(r, argc, argv, NULL, NULL, 0, "status") < 0)
		return -1;
	split_state_load(r, &sp);
	series_state_load(r, &st);
	if (sp.active)
		split_status_line(&sp, &sb);
	else
		series_status_line(&st, &sb);
	printf("%s\n", sb.len ? sb.buf : "nothing in progress");
	split_state_release(&sp);
	series_state_release(&st);
	sb_release(&sb);
	return 0;
}

static int cmd_version(struct repo *r, int argc, char **argv)
{
	printf("git-carve version %s\n", CARVE_VERSION);
	return 0;
}

static int cmd_undo(struct repo *r, int argc, char **argv)
{
	bool force = false, list = false;
	const struct opt opts[] = {
		{ "--force", NULL, NULL, &force },
		{ "--list", NULL, NULL, &list },
		{ NULL, NULL, NULL, NULL },
	};
	struct strbuf done = STRBUF_INIT;
	int np;

	np = parse_args(r, argc, argv, opts, NULL, 0,
			"undo [--force] | undo --list");

	if (np < 0)
		return -1;
	if (list) {
		struct undo_entry *v;
		size_t n, i;

		if (undo_list(r, &v, &n))
			return -1;
		for (i = n; i > 0; i--) {
			time_t t = (time_t)v[i - 1].time;
			char when[32];

			strftime(when, sizeof(when), "%Y-%m-%d %H:%M",
				 localtime(&t));
			printf("%-4d %s  %s%s%s\n", v[i - 1].seq, when,
			       v[i - 1].op, *v[i - 1].what ? " " : "",
			       v[i - 1].what);
		}
		undo_list_free(v, n);
		return 0;
	}
	if (undo_last(r, force, &done)) {
		sb_release(&done);
		return -1;
	}
	printf("undid %s\n", done.buf);
	sb_release(&done);
	return 0;
}

struct cmd {
	const char *name;
	int (*fn)(struct repo *r, int argc, char **argv);
	/*
	 * The paths among its arguments, for undo of a command that changes
	 * the work tree (undo_kind): f the first positional, a all of them,
	 * s the second (a commit comes first)
	 */
	char files;
};

static const struct cmd cmds[] = {
	{ "abort", cmd_abort, 0 },
	{ "absorb", cmd_absorb, 0 },
	{ "blame", cmd_blame, 0 },
	{ "commit", cmd_commit, 0 },
	{ "message", cmd_message, 0 },
	{ "continue", cmd_continue, 0 },
	{ "defer", cmd_defer, 0 },
	{ "discard", cmd_discard, 'f' },
	{ "discard-file", cmd_discard_file, 'a' },
	{ "remove", cmd_remove, 'a' },
	{ "ignore", cmd_ignore, 0 },
	{ "diff", cmd_diff, 0 },
	{ "fixup", cmd_fixup, 0 },
	{ "fixup-to", cmd_fixup_to, 0 },
	{ "gc", cmd_gc, 0 },
	{ "move", cmd_move, 0 },
	{ "drop", cmd_drop, 's' },
	{ "reword", cmd_reword, 0 },
	{ "edit", cmd_edit, 0 },
	{ "prefix", cmd_prefix, 0 },
	{ "renames", cmd_renames, 0 },
	{ "resolve", cmd_resolve, 0 },
	{ "set-staged", cmd_set_staged, 0 },
	{ "show", cmd_show, 0 },
	{ "split", cmd_split, 0 },
	{ "squash", cmd_squash, 0 },
	{ "stage", cmd_stage, 0 },
	{ "stage-file", cmd_stage_file, 0 },
	{ "stage-move", cmd_stage_move, 0 },
	{ "status", cmd_status, 0 },
	{ "substitute", cmd_substitute, 'f' },
	{ "undo", cmd_undo, 0 },
	{ "unstage", cmd_unstage, 0 },
	{ "unstage-file", cmd_unstage_file, 0 },
	{ "unstage-move", cmd_unstage_move, 0 },
	{ "version", cmd_version, 0 },
};

/* "stage f.c n3": the arguments, on one line and not too long. */
static void undo_what(int argc, char **argv, struct strbuf *out)
{
	int i;

	for (i = 1; i < argc && out->len < 80; i++) {
		const char *s;

		if (out->len)
			sb_addch(out, ' ');
		for (s = argv[i]; *s && out->len < 80; s++)
			sb_addch(out, *s == '\n' || *s == '\t' ? ' ' : *s);
	}
}

/*
 * The work-tree files a command may change, for undo. They are its
 * positional arguments, or only the first with 'f' in c->files, relative
 * to the top.
 */
static int undo_files(struct repo *r, const struct cmd *c, int argc,
		      char **argv, struct undo_state *before)
{
	int i, rc, k = 0;

	for (i = 1; i < argc; i++) {
		const char *cp;
		char *p;

		if (argv[i][0] == '-' && argv[i][1])
			continue;
		/*
		 * with 's', the second positional, since a commit comes
		 * first
		 */
		if (c->files == 's' && k++ != 1)
			continue;
		p = resolve_path(r, argv[i]);
		if (!p)
			return repo_err(r, OUTSIDE_MSG, argv[i]);
		cp = p;
		rc = undo_capture_files(r, before, &cp, 1, true);
		free(p);
		if (rc)
			return repo_err(r,
					"cannot save %s for undo; nothing "
					"changed",
					argv[i]);
		if (c->files == 'f' || c->files == 's')
			break;
	}
	return 0;
}

/*
 * A mode of an undoable command that changes nothing, such as split
 * --status.
 */
static bool read_only(const struct cmd *c, int argc, char **argv)
{
	int i;

	for (i = 1; i < argc && !strcmp(c->name, "split"); i++)
		if (!strcmp(argv[i], "--status"))
			return true;
	return false;
}

static int run_cmd_undoable(struct repo *r, const struct cmd *c, int argc,
			    char **argv)
{
	struct undo_state before = { 0 };
	struct strbuf what = STRBUF_INIT;
	int rc;

	if (undo_kind(c->name) == UNDO_NONE || read_only(c, argc, argv))
		return c->fn(r, argc, argv);
	if ((undo_kind(c->name) == UNDO_WORKTREE) != !!c->files)
		die("%s: undo_kind and its paths disagree", c->name);
	undo_begin(r, &before, c->name);
	if (c->files && undo_files(r, c, argc, argv, &before)) {
		undo_state_release(&before);
		return -1;
	}
	rc = c->fn(r, argc, argv);
	undo_what(argc, argv, &what);
	undo_end(r, &before, c->name, sb_str(&what));
	sb_release(&what);
	return rc;
}

int cli_main(struct repo *r, int argc, char **argv)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cmds); i++)
		if (!strcmp(argv[0], cmds[i].name))
			return run_cmd_undoable(r, &cmds[i], argc, argv);
	repo_err(r, "unknown command '%s' (see 'git-carve-cli help')", argv[0]);
	return -1;
}
