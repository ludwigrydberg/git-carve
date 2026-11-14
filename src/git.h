/* SPDX-License-Identifier: MIT */
#ifndef CARVE_GIT_H
#define CARVE_GIT_H

#include <sys/types.h>

#include "util.h"

/*
 * The repository context every core function gets. There is no global
 * state. Errors and warnings are collected here, and the CLI prints them
 * while the TUI shows them in its status line.
 */
struct undo_queued;

struct repo {
	char *top;	  /* absolute work tree top level */
	char *prefix;	  /* cwd relative to toplevel, "" or "dir/" */
	char *carve_dir;  /* $GIT_DIR/carve (per worktree) */
	char *empty_tree; /* computed when first needed */
	/* absolute git rev-parse --git-path answers, fixed for the run */
	char **gpath_name, **gpath;
	size_t ngpath;
	/* the index file's stamp and its tree, as undo last saw them */
	char *undo_stamp, *undo_index;
	/*
	 * In the TUI the undo entries of commands that change only the index
	 * wait here and undo_flush writes them together. undo_quiet says no
	 * operation was in progress when the history was last read.
	 */
	bool undo_defer, undo_quiet;
	/* the undo refs as listed by undo_begin, for its undo_end */
	struct strbuf undo_refs;
	bool undo_refs_valid;
	/*
	 * The last patch apply_selection applied to the index, whether it was
	 * reversed (an unstage) or -U0, and the stamp of the index it applied
	 * to. The undo entry of a line stage can be this patch.
	 */
	struct strbuf last_patch;
	bool last_patch_reverse, last_patch_zero;
	char *last_patch_from;
	struct undo_queued *undo_q;
	size_t undo_nq, undo_aq;
	/* the last index entry asked for (ls-files -s), while the index stays */
	char *ie_stamp, *ie_path;
	struct strbuf ie_line;
	/*
	 * git cat-file --batch-check and --batch run alongside us, and
	 * repo_rev and repo_cat ask them instead of starting a git each
	 */
	struct cat_worker {
		pid_t pid;
		int in, out;	   /* our ends of its stdin, stdout */
		struct strbuf buf; /* read, not consumed yet */
	} cf_check, cf_cat;
	/* HEAD's blobs sorted by id, for copy_source, while HEAD is hb_tree */
	struct head_blob {
		char *oid, *path;
	} *hb;
	size_t nhb;
	char *hb_tree;
	/*
	 * Blobs that git did not have while HEAD's tree was nc_tree. They are
	 * no copy, even after git add has written them.
	 */
	char **nc;
	size_t nnc;
	char *nc_tree;
	/* the commits absorb listed for ab_key, "<range> <HEAD>" */
	char *ab_key;
	struct strbuf ab_list;
	/*
	 * The commits above sl_id up to sl_head that check_series_commit
	 * listed (git rev-list --parents --boundary), for list_commits
	 */
	char *sl_id, *sl_head;
	struct strbuf sl;
	/*
	 * The index's tree while the index file is ix_tree_stamp. A status
	 * that saw nothing staged knows it, which spares undo a write-tree.
	 */
	char *ix_tree, *ix_tree_stamp;
	/* the last answer of find_renames and the key it was for */
	char *rn_key;
	struct rename *rn;
	size_t nrn;
	/* the counts split_state_load found, and the key they hold for */
	char *split_key;
	int split_made, split_added, split_removed;
	/*
	 * Set by the TUI for a split of a huge commit. Its status then counts
	 * the files left instead of the lines, because --numstat reads every
	 * blob, which takes 2.8 s on a kernel release.
	 */
	bool split_big;
	/* git config -z --list is read when repo_config is first asked */
	bool cfg_loaded;
	/* git var answers, kept with the configuration they come from */
	char *editor, *committer;
	char **cfg_key, **cfg_val; /* a NULL value is a key without "=" */
	size_t ncfg;
	struct strbuf err; /* last error, one line */
	/*
	 * What git printed behind the error, such as a hook's output. The
	 * TUI shows it. The CLI has already shown it as it came.
	 */
	struct strbuf err_detail;
	struct strbuf warn; /* accumulated warnings, one per line */
	/* how modules that keep caches here free them (repo_on_release) */
	void (*release[4])(struct repo *r);
	size_t nrelease;
	bool tty; /* may run interactive children (editor) */
};

/* Options that make git's path output parseable whatever the config. */
#define GIT_PLAIN "-c", "core.quotePath=false"
/*
 * An editor that accepts the message it is given, for commits and
 * rebases that git-carve drives itself.
 */
#define GIT_EDITOR_QUIET "GIT_EDITOR=true"
extern const char *const git_quiet_editor[]; /* { GIT_EDITOR_QUIET } */
/* More paths than this go to git on stdin (--pathspec-from-file). */
#define PATHSPEC_ARGS_MAX 64

/* Minimum supported git version. */
#define GIT_MIN_MAJOR 2
#define GIT_MIN_MINOR 26

int repo_err(struct repo *r, const char *fmt, ...) PRINTF_LIKE(2, 3);
/*
 * fn frees a module's cache in r when r is released. A module registers
 * it when it fills its cache, and once is enough.
 */
void repo_on_release(struct repo *r, void (*fn)(struct repo *r));
/* HEAD's upstream by its full name (refs/remotes/origin/main), or NULL. */
char *repo_upstream(struct repo *r);
/*
 * HEAD for a title, from one git log. *line gets "abc1234 subject", or
 * NULL before the first commit, and *branch the branch, or NULL when
 * HEAD is detached.
 */
int repo_head_info(struct repo *r, char **branch, char **line);
/* rev as "abc1234 subject", the subject quoted if asked, or NULL. */
char *repo_commit_oneline(struct repo *r, const char *rev, bool quote);
/*
 * The newest n commits of rev that are not merges, one "<id> <short>
 * <subject>" line each. These are the commits a change can go into.
 */
int repo_recent_commits(struct repo *r, const char *rev, int n,
			struct strbuf *out);
/*
 * count first-parent commits from rev on, one line each with the id, the
 * parents, the branches (%D, full names) and "<short> <subject>",
 * separated by \x1f. The branch except, if not NULL, is left out.
 */
int repo_first_parents(struct repo *r, const char *rev, size_t count,
		       const char *except, struct strbuf *out);
/*
 * One line in format for each commit of ids, in their order, from git
 * log --no-walk=unsorted. With n == 0 no git runs.
 */
int repo_log_commits(struct repo *r, const char *format, char *const *ids,
		     size_t n, struct strbuf *out);
/* Whether the repository has remote-tracking branches, usually without git. */
bool repo_has_remotes(struct repo *r);
/* After repo_err(), keep what git printed as the error's detail. */
void repo_err_detail(struct repo *r, const struct strbuf *out);
void repo_warn(struct repo *r, const char *fmt, ...) PRINTF_LIKE(2, 3);

/* Locate the repository, chdir to its toplevel and fill *r. */
int repo_open(struct repo *r);
void repo_release(struct repo *r);
/*
 * check_git_version, skipped when the git in PATH is the same file (path,
 * mtime and size) as when it last passed. That is remembered in
 * ~/.cache/git-carve/git-version.
 */
int check_git_version_cached(struct repo *r);
int parse_git_version(const char *s, int *major, int *minor);

enum run_stop { RUN_DONE, RUN_LIMIT, RUN_CANCELLED };

/*
 * Stop a git early by killing it, and say why in *stopped. It stops after
 * max_lines lines of output (0 for no limit), or with cancel_on_tty when a
 * key is waiting on stdin after 300 ms. The key stays there to be read,
 * and keys typed ahead do not stop a quick run. progress, if set, gets the
 * bytes read until then after every megabyte, and every 300 ms while
 * nothing comes.
 */
struct run_limits {
	size_t max_lines;
	bool cancel_on_tty;
	void (*progress)(void *ctx, size_t bytes);
	void *progress_ctx;
	enum run_stop *stopped;
};

struct run_opts {
	const char *in; /* stdin data, NULL for /dev/null */
	size_t inlen;
	const char *const *env; /* extra "K=V" entries, NULL ended */
	/*
	 * Inherit stdin, stdout and stderr. With errb, stderr still reaches
	 * the terminal and its last TEE_KEEP bytes are kept in errb too.
	 */
	bool tty;
	struct run_limits lim;
};

/*
 * Spawn argv[0] (looked up in PATH). Captures stdout into *out and stderr
 * into *errb (either may be NULL to discard). Returns the exit status, or
 * -1 if the program could not be run. Killed children return 128 + signal.
 */
int run_cmd(const char *const *argv, const struct run_opts *o,
	    struct strbuf *out, struct strbuf *errb);

/* Like run_cmd() but prepends "git". */
int run_git(const struct argv *args, const struct run_opts *o,
	    struct strbuf *out, struct strbuf *errb);

/*
 * Run "git <args...>" and capture stdout. When git fails, its first line
 * on stderr goes to r->err.
 */
int git_out(struct repo *r, struct strbuf *out, ...) SENTINEL;
/* git_out without the output. */
int git_quiet(struct repo *r, ...) SENTINEL;
/* The exit status only, with no error recorded. */
int git_status_of(struct repo *r, ...) SENTINEL;
/* git_out with extra "K=V" environment entries, ended by NULL. */
int git_env(struct repo *r, const char *const *env, struct strbuf *out,
	    ...) SENTINEL;
/* git_out with data on stdin. */
int git_in(struct repo *r, const char *in, size_t inlen, struct strbuf *out,
	   ...) SENTINEL;
/* The first line git prints, as a new string, or NULL. */
char *git_line(struct repo *r, ...) SENTINEL;

/* Set r->err from a failed command's stderr. */
int git_fail(struct repo *r, const char *what, const struct strbuf *errb);

/* The start of every git diff we parse, immune to the user's config. */
void push_diff_args(struct argv *a);

const char *repo_empty_tree(struct repo *r);
/* The path of a file in r->carve_dir. */
char *carve_path(struct repo *r, const char *name);

/*
 * A git running in the background, such as the scan for untracked files.
 * Its stdin and stderr are /dev/null, and its output collects in out
 * without blocking the caller.
 */
struct bg_proc {
	pid_t pid;
	int fd;
	struct strbuf out;
	int status; /* exit status once finished */
};

int bg_start(struct bg_proc *bp, const struct argv *args);
/* Read what is there. Returns 1 when it finished, 0 while it runs, or -1. */
int bg_pump(struct bg_proc *bp);
/* Stop it if running and release everything. */
void bg_stop(struct bg_proc *bp);
static inline bool bg_running(const struct bg_proc *bp)
{
	return bp->pid > 0;
}

/*
 * Run the editor on file (at line if > 0) with the terminal attached.
 * Returns 0 if the editor exited successfully.
 */
int run_editor(struct repo *r, const char *file, int line);
/*
 * The absolute path of name in $GIT_DIR (git rev-parse --git-path), asked
 * once per repo, or NULL if git fails.
 */
const char *repo_git_path(struct repo *r, const char *name);
/* repo_git_path as a copy. */
char *repo_git_path_dup(struct repo *r, const char *name);
/*
 * The index file's checksum and size as "<hex>-<size>", read from its
 * last 20 bytes without git. It changes whenever the index does. NULL
 * when there is none to trust, with no index or with index.skipHash.
 */
char *repo_index_stamp(struct repo *r);
/* The index file of this worktree, or GIT_INDEX_FILE's. */
char *repo_index_path(struct repo *r);
/*
 * git ls-files -s for path, added to line if not NULL. Returns 1 when
 * path is in the index, 0 when not, -1 on error. git is asked again only
 * when the index file changed or for another path.
 */
int index_entry_of(struct repo *r, const char *path, struct strbuf *line);
/* The blob oid added to out, read through the running cat-file. */
int read_blob(struct repo *r, const char *oid, struct strbuf *out);
/*
 * The last value of key, or NULL. Names compare as git compares them,
 * and a key set without "=" gives "". The whole configuration is read
 * with one git config -z --list and kept until repo_config_drop.
 */
const char *repo_config(struct repo *r, const char *key);
/* "Name <email>" of the committer (git var, asked once), or NULL. */
char *repo_committer(struct repo *r);
/* repo_config as a copy. */
char *repo_config_dup(struct repo *r, const char *key);
/* The value as git config --bool / --int would read it, else def. */
bool repo_config_bool(struct repo *r, const char *key, bool def);
long repo_config_int(struct repo *r, const char *key, long def);
/*
 * parse_config_bool reads a value as git reads a bool, NULL being a key
 * without "=", and returns 1, 0, or -1 when it is no bool.
 * parse_config_int takes git's k, m and g suffixes and returns 0 or -1.
 */
int parse_config_bool(const char *v);
int parse_config_int(const char *v, long *out);
/*
 * Every value of key, or of every key starting with it when it ends with
 * ".". They come as "key value" lines like git config --get-regexp, or
 * with values_only one value per line like --get-all. Returns how many.
 */
size_t repo_config_all(struct repo *r, const char *key, bool values_only,
		       struct strbuf *out);
/* Forget the configuration. The next question reads it again. */
void repo_config_drop(struct repo *r);
/*
 * The object id rev names, as git rev-parse -q --verify gives it for "HEAD",
 * "X^{tree}" or "HEAD:path", or NULL when it names none. The cat-file running
 * alongside answers without a process per question. cat-file reads the index,
 * the configuration and the reflogs only once, and ":path", "@{u}" and "@{1}"
 * go to git rev-parse instead.
 */
char *repo_rev(struct repo *r, const char *rev);
/* The size of the object rev names, or -1 if none. */
long repo_size(struct repo *r, const char *rev);
/* 0 when rev names an object and 1 when not, like an exit status. */
int repo_rev_status(struct repo *r, const char *rev);
/*
 * The object rev names, added to out, and its type ("commit", "blob")
 * in type if it is not NULL, which needs 16 bytes. Returns 0, or -1 when
 * there is no such object. One git cat-file --batch answers them all.
 */
int repo_cat(struct repo *r, const char *rev, struct strbuf *out, char *type);
/*
 * Each of the n objects oids names, given to fn with its index in oids,
 * or with NULL when there is none. The requests go to git cat-file in
 * batches, and the answers stream back without a round trip per object.
 * fn returns false to stop. Returns -1 when git went away.
 */
int repo_cat_each(struct repo *r, char *const *oids, size_t n,
		  bool (*fn)(void *ctx, size_t i, const char *data, size_t len),
		  void *ctx);
/*
 * A commit's message as it is stored, without the newline git log adds
 * after it, and its subject (the first paragraph on one line, or NULL if
 * rev is no commit).
 */
int repo_commit_message(struct repo *r, const char *rev, struct strbuf *out);
char *repo_commit_subject(struct repo *r, const char *rev);
/*
 * The index was just made from treeish by reset --mixed or read-tree.
 * Its tree is treeish's while the index file stays as it is now, and
 * undo needs no git write-tree for it.
 */
void repo_index_is(struct repo *r, const char *treeish);
/*
 * The branch HEAD is on ("main" for refs/heads/main), read from the HEAD
 * file without git, or NULL when HEAD is detached. *known is false when
 * the file says something else, such as another symref or a reftable
 * placeholder, or cannot be read. Ask git then.
 */
char *repo_head_branch(struct repo *r, bool *known);
/* Whether name exists in $GIT_DIR. */
bool git_path_exists(struct repo *r, const char *name);

/* Whether HEAD points to a commit (false on an unborn branch). */
bool have_head(struct repo *r);

/* The full id of rev (a commit), or NULL. */
char *resolve_commit(struct repo *r, const char *rev);

/* The parent of commit, NULL for a root commit. */
char *parent_of(struct repo *r, const char *commit);

/* git apply that keeps whitespace as it is, whatever the config. */
#define GIT_APPLY_PLAIN "-c", "apply.ignoreWhitespace=no"

#endif
