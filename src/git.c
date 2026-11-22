// SPDX-License-Identifier: MIT
#include "git.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define EMPTY_TREE_SHA1 "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
#define NOT_A_WORK_TREE_MSG "not inside a git work tree"

extern char **environ;

int repo_err(struct repo *r, const char *fmt, ...)
{
	va_list ap;

	sb_reset(&r->err);
	sb_reset(&r->err_detail);
	va_start(ap, fmt);
	sb_vaddf(&r->err, fmt, ap);
	va_end(ap);
	return -1;
}

void repo_on_release(struct repo *r, void (*fn)(struct repo *r))
{
	size_t i;

	for (i = 0; i < r->nrelease; i++)
		if (r->release[i] == fn)
			return;
	if (r->nrelease == sizeof(r->release) / sizeof(r->release[0]))
		die("too many caches in struct repo");
	r->release[r->nrelease++] = fn;
}

void repo_err_detail(struct repo *r, const struct strbuf *out)
{
	const char *s = sb_str(out);
	size_t len = out->len;

	while (len && (s[len - 1] == '\n' || s[len - 1] == ' '))
		len--;
	sb_reset(&r->err_detail);
	sb_add(&r->err_detail, s, len);
}

void repo_warn(struct repo *r, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	sb_vaddf(&r->warn, fmt, ap);
	va_end(ap);
	sb_addch(&r->warn, '\n');
}

static char **build_env(const char *const *extra)
{
	size_t n = 0, m = 0, i, j, k = 0;
	char **env;

	while (environ[n])
		n++;
	while (extra && extra[m])
		m++;
	env = xcalloc(n + m + 1, sizeof(*env));
	for (i = 0; i < n; i++) {
		const char *eq = strchr(environ[i], '=');
		bool overridden = false;
		size_t kl;

		kl = eq ? (size_t)(eq - environ[i]) + 1 : 0;

		for (j = 0; kl && j < m; j++)
			if (!strncmp(environ[i], extra[j], kl))
				overridden = true;
		if (!overridden)
			env[k++] = environ[i];
	}
	for (j = 0; j < m; j++)
		env[k++] = (char *)extra[j];
	env[k] = NULL;
	return env;
}

static void worker_stop(struct cat_worker *w);

static void set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);

	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void close_pair(int p[2])
{
	if (p[0] >= 0)
		close(p[0]);
	if (p[1] >= 0)
		close(p[1]);
}

/* Milliseconds since t0. */
static long elapsed_ms(const struct timespec *t0)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0->tv_sec) * 1000 +
	       (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/*
 * Feed the child its input and collect its output until both pipes close,
 * a key stops it (o->lim.cancel_on_tty) or it prints too much
 * (o->lim.max_lines). Then it is killed. The caller closes what is still
 * open.
 */
static enum run_stop exchange(pid_t pid, const struct run_opts *o, int pout[2],
			      int perr[2], int pin[2], const char *in,
			      size_t inleft, struct strbuf *out,
			      struct strbuf *errb)
{
	size_t lines = 0, got = 0, reported = 0;
	enum run_stop stop = RUN_DONE;
	struct pollfd fds[4];
	bool armed = false;
	struct timespec t0;
	char buf[65536];

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		int nf = 0, i, prc;

		if (pout[0] >= 0)
			fds[nf++] = (struct pollfd){ .fd = pout[0],
						     .events = POLLIN };
		if (perr[0] >= 0)
			fds[nf++] = (struct pollfd){ .fd = perr[0],
						     .events = POLLIN };
		if (pin[1] >= 0)
			fds[nf++] = (struct pollfd){ .fd = pin[1],
						     .events = POLLOUT };
		if (!nf)
			break;
		/*
		 * A key on the terminal stops a slow load and stays there
		 * to be read. Keys typed ahead do not stop a quick one.
		 */
		if (o->lim.cancel_on_tty && !armed)
			armed = elapsed_ms(&t0) >= 300;
		if (armed)
			fds[nf++] = (struct pollfd){ .fd = STDIN_FILENO,
						     .events = POLLIN };
		prc = poll(fds, (nfds_t)nf,
			   o->lim.progress || (o->lim.cancel_on_tty && !armed) ?
				   300 :
				   -1);
		if (prc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (!prc) {
			/*
			 * a slow git reports progress even before a
			 * megabyte came
			 */
			if (o->lim.progress)
				o->lim.progress(o->lim.progress_ctx, got);
			continue;
		}
		for (i = 0; i < nf; i++) {
			ssize_t n;

			if (!fds[i].revents)
				continue;
			if (fds[i].fd == STDIN_FILENO && o->lim.cancel_on_tty) {
				stop = RUN_CANCELLED;
				break;
			}
			if (fds[i].fd == pin[1]) {
				n = write(pin[1], in, inleft);
				if (n < 0 &&
				    (errno == EAGAIN || errno == EINTR))
					continue;
				if (n > 0) {
					in += n;
					inleft -= (size_t)n;
				}
				if (n < 0 || !inleft) {
					close(pin[1]);
					pin[1] = -1;
				}
				continue;
			}
			n = read(fds[i].fd, buf, sizeof(buf));
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				continue;
			if (n <= 0) {
				close(fds[i].fd);
				if (fds[i].fd == pout[0])
					pout[0] = -1;
				else
					perr[0] = -1;
				continue;
			}
			if (fds[i].fd == pout[0]) {
				if (out)
					sb_add(out, buf, (size_t)n);
				got += (size_t)n;
				if (o->lim.max_lines)
					lines += count_lines(buf, (size_t)n);
				if (o->lim.max_lines &&
				    lines > o->lim.max_lines)
					stop = RUN_LIMIT;
			} else if (fds[i].fd == perr[0] && errb) {
				sb_add(errb, buf, (size_t)n);
			}
		}
		if (stop != RUN_DONE) {
			/* git removes its lock files on SIGTERM, not SIGKILL */
			kill(pid, SIGTERM);
			break;
		}
		/* every megabyte */
		if (o->lim.progress && got - reported >= (1 << 20)) {
			reported = got;
			o->lim.progress(o->lim.progress_ctx, got);
		}
	}
	return stop;
}

/* Microseconds on a monotonic clock, for the trace. */
static long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/*
 * GIT_CARVE_TRACE=<file>: one line per process spawned, its time in ms
 * ("bg" for the background scan) and its arguments (newlines as \n).
 */
static void trace(const char *const *argv, long long t0)
{
	const char *file = getenv("GIT_CARVE_TRACE");
	struct strbuf l = STRBUF_INIT;
	const char *p;
	size_t i;
	FILE *f;

	if (!file || !*file)
		return;
	if (t0 >= 0)
		sb_addf(&l, "%.1fms", (double)(now_us() - t0) / 1000.0);
	else
		sb_adds(&l, "bg");
	for (i = 0; argv[i]; i++) {
		sb_add(&l, " ", 1);
		for (p = argv[i]; *p; p++)
			if (*p == '\n')
				sb_adds(&l, "\\n");
			else
				sb_add(&l, p, 1);
	}
	f = fopen(file, "a");
	if (f) {
		fprintf(f, "%s\n", l.buf);
		fclose(f);
	}
	sb_release(&l);
}

/* The questions repo_prefetch_startup asks ahead */
#define HEAD_INFO_ARGS                                     \
	"log", "-1", "--no-color", "--decorate-refs=HEAD", \
		"--decorate-refs=refs/heads/", "--format=%h %s%n%D"
#define CONFIG_ARGS "config", "-z", "--list"
#define UPSTREAM_ARGS \
	"rev-parse", "-q", "--verify", "--symbolic-full-name", "@{upstream}"

const char *const git_quiet_editor[] = { GIT_EDITOR_QUIET, NULL };

static void write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t k = write(fd, p, n);

		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return;
		p += k;
		n -= (size_t)k;
	}
}

/* How much of the end of a terminal child's stderr is kept. */
#define TEE_KEEP ((size_t)65536)

/*
 * A child on the terminal with its stderr on fd. Copy that to our stderr
 * and keep its end in errb, until EOF or until the child is gone, since
 * what it started may keep the pipe open. True when it reaped the child.
 */
static bool tee_err(pid_t pid, int fd, struct strbuf *errb, int *status)
{
	bool reaped = false;
	char buf[4096];

	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int n = poll(&p, 1, reaped ? 0 : 200);
		ssize_t k;

		if (n < 0 && errno == EINTR)
			continue;
		if (n > 0) {
			k = read(fd, buf, sizeof(buf));
			if (k < 0 && errno == EINTR)
				continue;
			if (k <= 0)
				break;
			write_all(STDERR_FILENO, buf, (size_t)k);
			sb_add(errb, buf, (size_t)k);
			if (errb->len > 2 * TEE_KEEP) {
				memmove(errb->buf,
					errb->buf + errb->len - TEE_KEEP,
					TEE_KEEP);
				errb->len = TEE_KEEP;
				errb->buf[errb->len] = '\0';
			}
			continue;
		}
		if (n < 0 || reaped)
			break;
		/*
		 * after a while without output, check whether it is gone,
		 * and then read what is left and stop
		 */
		if (waitpid(pid, status, WNOHANG) == pid)
			reaped = true;
	}
	return reaped;
}

static int spawn_wait(const char *const *argv, const struct run_opts *o,
		      struct strbuf *out, struct strbuf *errb, long long t0)
{
	int pin[2] = { -1, -1 }, pout[2] = { -1, -1 }, perr[2] = { -1, -1 };
	static const struct run_opts none;
	posix_spawn_file_actions_t fa;
	bool reaped = false;
	int status, rc;
	const char *in;
	size_t inleft;
	char **env;
	pid_t pid;

	if (!o)
		o = &none;
	in = o->in;
	inleft = o->in ? o->inlen : 0;

	posix_spawn_file_actions_init(&fa);
	if (!o->tty) {
		if (pipe(pout) || pipe(perr) || (o->in && pipe(pin))) {
			close_pair(pout);
			close_pair(perr);
			close_pair(pin);
			posix_spawn_file_actions_destroy(&fa);
			return -1;
		}
		if (o->in) {
			posix_spawn_file_actions_adddup2(&fa, pin[0], 0);
			posix_spawn_file_actions_addclose(&fa, pin[1]);
		} else {
			posix_spawn_file_actions_addopen(&fa, 0, "/dev/null",
							 O_RDONLY, 0);
		}
		posix_spawn_file_actions_adddup2(&fa, pout[1], 1);
		posix_spawn_file_actions_adddup2(&fa, perr[1], 2);
		posix_spawn_file_actions_addclose(&fa, pout[0]);
		posix_spawn_file_actions_addclose(&fa, perr[0]);
	} else if (errb) {
		/* stderr goes through us to the terminal, and is kept */
		if (pipe(perr)) {
			posix_spawn_file_actions_destroy(&fa);
			return -1;
		}
		posix_spawn_file_actions_adddup2(&fa, perr[1], 2);
		posix_spawn_file_actions_addclose(&fa, perr[0]);
	}

	env = build_env(o->env);
	rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, env);
	free(env);
	posix_spawn_file_actions_destroy(&fa);
	if (rc) {
		close_pair(pout);
		close_pair(perr);
		close_pair(pin);
		if (errb)
			sb_addf(errb, "cannot run %s: %s\n", argv[0],
				strerror(rc));
		return -1;
	}

	if (o->lim.stopped)
		*o->lim.stopped = RUN_DONE;
	if (!o->tty) {
		enum run_stop stop;

		close(pout[1]);
		close(perr[1]);
		pout[1] = perr[1] = -1;
		if (o->in) {
			close(pin[0]);
			pin[0] = -1;
			set_nonblock(pin[1]);
			if (!inleft) {
				close(pin[1]);
				pin[1] = -1;
			}
		}
		stop = exchange(pid, o, pout, perr, pin, in, inleft, out, errb);
		/* after a poll error, do not leak what is still open */
		if (pout[0] >= 0)
			close(pout[0]);
		if (perr[0] >= 0)
			close(perr[0]);
		if (pin[1] >= 0)
			close(pin[1]);
		if (o->lim.stopped)
			*o->lim.stopped = stop;
	} else if (errb) {
		close(perr[1]);
		perr[1] = -1;
		if (tee_err(pid, perr[0], errb, &status))
			reaped = true;
		close(perr[0]);
	}

	while (!reaped && waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	trace(argv, t0);
	if (out && !out->buf)
		sb_add(out, "", 0);
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
}

int run_cmd(const char *const *argv, const struct run_opts *o,
	    struct strbuf *out, struct strbuf *errb)
{
	return spawn_wait(argv, o, out, errb, now_us());
}

int run_git(const struct argv *args, const struct run_opts *o,
	    struct strbuf *out, struct strbuf *errb)
{
	struct argv a = ARGV_INIT;
	size_t i;
	int rc;

	argv_push(&a, "git");
	for (i = 0; i < args->n; i++)
		argv_push(&a, args->v[i]);
	rc = run_cmd((const char *const *)a.v, o, out, errb);
	argv_clear(&a);
	return rc;
}

int git_fail(struct repo *r, const char *what, const struct strbuf *errb)
{
	const char *s = sb_str(errb), *nl;

	/* Prefer the first "error:"/"fatal:" line, else the first line. */
	const char *e = strstr(s, "error: ");
	const char *f = strstr(s, "fatal: ");

	if (f && (!e || f < e))
		e = f;
	if (e)
		s = e;
	while (*s == '\n')
		s++;
	nl = strchr(s, '\n');
	if (!*s)
		return repo_err(r, "%s failed", what);
	return repo_err(r, "%s: %.*s", what,
			nl ? (int)(nl - s) : (int)strlen(s), s);
}

static int vgit_env(struct repo *r, const char *const *env, const char *in,
		    size_t inlen, struct strbuf *out, bool record, va_list ap)
{
	struct run_opts o = { .in = in, .inlen = inlen, .env = env };
	struct strbuf errb = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	const char *s;
	int rc;

	while ((s = va_arg(ap, const char *)))
		argv_push(&a, s);
	if (in || env)
		rc = run_git(&a, &o, out, &errb);
	else
		rc = git_argv(r, &a, out, record ? &errb : NULL);
	if (rc && record) {
		struct strbuf what = STRBUF_INIT;

		sb_addf(&what, "git %s", a.n ? a.v[0] : "");
		git_fail(r, what.buf, &errb);
		sb_release(&what);
	}
	sb_release(&errb);
	argv_clear(&a);
	return rc;
}

static int vgit(struct repo *r, const char *in, size_t inlen,
		struct strbuf *out, bool record, va_list ap)
{
	return vgit_env(r, NULL, in, inlen, out, record, ap);
}

int git_env(struct repo *r, const char *const *env, struct strbuf *out, ...)
{
	va_list ap;
	int rc;

	va_start(ap, out);
	rc = vgit_env(r, env, NULL, 0, out, true, ap);
	va_end(ap);
	return rc;
}

int git_out(struct repo *r, struct strbuf *out, ...)
{
	va_list ap;
	int rc;

	va_start(ap, out);
	rc = vgit(r, NULL, 0, out, true, ap);
	va_end(ap);
	return rc;
}

int git_quiet(struct repo *r, ...)
{
	va_list ap;
	int rc;

	va_start(ap, r);
	rc = vgit(r, NULL, 0, NULL, true, ap);
	va_end(ap);
	return rc;
}

int git_status_of(struct repo *r, ...)
{
	va_list ap;
	int rc;

	va_start(ap, r);
	rc = vgit(r, NULL, 0, NULL, false, ap);
	va_end(ap);
	return rc;
}

int git_in(struct repo *r, const char *in, size_t inlen, struct strbuf *out,
	   ...)
{
	va_list ap;
	int rc;

	va_start(ap, out);
	rc = vgit(r, in ? in : "", in ? inlen : 0, out, true, ap);
	va_end(ap);
	return rc;
}

char *git_line(struct repo *r, ...)
{
	struct strbuf out = STRBUF_INIT;
	char *nl, *res;
	va_list ap;
	int rc;

	va_start(ap, r);
	rc = vgit(r, NULL, 0, &out, false, ap);
	va_end(ap);
	if (rc || !out.len) {
		sb_release(&out);
		return NULL;
	}
	nl = strchr(out.buf, '\n');
	if (nl)
		*nl = '\0';
	res = xstrdup(out.buf);
	sb_release(&out);
	return res;
}

void push_diff_args(struct argv *a)
{
	argv_pushl(a, GIT_PLAIN, "-c", "diff.noprefix=false", "-c",
		   "diff.mnemonicPrefix=false", "diff", "--no-color",
		   "--no-ext-diff", "--no-textconv", "--src-prefix=a/",
		   "--dst-prefix=b/", "-U3", NULL);
}

int parse_git_version(const char *s, int *major, int *minor)
{
	const char *p;

	if (!starts_with(s, "git version "))
		return -1;
	p = s + strlen("git version ");
	if (parse_int(p, &p, major) || *p != '.' ||
	    parse_int(p + 1, NULL, minor))
		return -1;
	return 0;
}

static int check_git_version(struct repo *r)
{
	char *v = git_line(r, "version", NULL);
	int major, minor;

	if (!v)
		return repo_err(r, "cannot run git; is it installed?");
	if (parse_git_version(v, &major, &minor)) {
		repo_err(r, "cannot parse '%s'", v);
		free(v);
		return -1;
	}
	if (major < GIT_MIN_MAJOR ||
	    (major == GIT_MIN_MAJOR && minor < GIT_MIN_MINOR)) {
		repo_err(r, "git %d.%d or newer is required (found %s)",
			 GIT_MIN_MAJOR, GIT_MIN_MINOR,
			 v + strlen("git version "));
		free(v);
		return -1;
	}
	free(v);
	return 0;
}

/* The git that posix_spawnp runs, the first executable "git" in PATH. */
static bool which_git(struct strbuf *path, struct stat *st)
{
	const char *p = getenv("PATH"), *colon;

	for (; p && *p; p = colon ? colon + 1 : NULL) {
		size_t n;

		colon = strchr(p, ':');
		n = colon ? (size_t)(colon - p) : strlen(p);
		sb_reset(path);
		if (n)
			sb_addf(path, "%.*s/git", (int)n, p);
		else
			sb_adds(path, "git");
		if (!stat(path->buf, st) && S_ISREG(st->st_mode) &&
		    !access(path->buf, X_OK))
			return true;
		if (!colon)
			break;
	}
	return false;
}

/*
 * The file remembering the git that passed the check, in the user's cache
 * directory ($XDG_CACHE_HOME or ~/.cache), which is made if needed. NULL
 * without either.
 */
static char *version_file(bool make)
{
	const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
	struct strbuf p = STRBUF_INIT;

	if (xdg && *xdg == '/')
		sb_adds(&p, xdg);
	else if (home && *home)
		sb_addf(&p, "%s/.cache", home);
	else
		return NULL;
	if (make)
		mkdir(p.buf, 0777);
	sb_adds(&p, "/git-carve");
	if (make)
		mkdir(p.buf, 0777);
	sb_adds(&p, "/git-version");
	return sb_detach(&p);
}

int check_git_version_cached(struct repo *r)
{
	struct strbuf git = STRBUF_INIT, want = STRBUF_INIT;
	struct strbuf have = STRBUF_INIT;
	char *file = version_file(false);
	struct stat st;
	int rc = 0;

	/*
	 * the same git binary as when it was last checked needs no git
	 * version
	 */
	if (file && which_git(&git, &st)) {
		sb_addf(&want, "%s %lld %lld\n", git.buf,
			(long long)st.st_mtime, (long long)st.st_size);
		if (!read_file(file, &have) && have.len == want.len &&
		    !memcmp(have.buf, want.buf, want.len))
			goto out;
	}
	rc = check_git_version(r);
	if (!rc && want.len) {
		free(file);
		file = version_file(true);
		if (file)
			write_file(file, want.buf, want.len);
	}
out:
	free(file);
	sb_release(&git);
	sb_release(&want);
	sb_release(&have);
	return rc;
}

/* The --git-path names git carve asks for, all answered at repo_open. */
static const char *const known_paths[] = {
	"rebase-merge",
	"rebase-apply",
	"rebase-merge/done",
	"index",
	"MERGE_HEAD",
	"CHERRY_PICK_HEAD",
	"REVERT_HEAD",
	"info/exclude",
	"carve/msg",
	"carve/op",
	"HEAD",
	"CARVE_MESSAGE",
	/* the Series pane without an upstream, and gc */
	"refs/remotes",
	"packed-refs",
	"reftable",
	"worktrees",
	NULL,
};

static void gpath_add(struct repo *r, const char *name, char *abs)
{
	r->gpath_name = xrealloc(r->gpath_name,
				 (r->ngpath + 1) * sizeof(*r->gpath_name));
	r->gpath = xrealloc(r->gpath, (r->ngpath + 1) * sizeof(*r->gpath));
	r->gpath_name[r->ngpath] = xstrdup(name);
	r->gpath[r->ngpath++] = abs;
}

int repo_open(struct repo *r)
{
	struct strbuf out = STRBUF_INIT, cwd = STRBUF_INIT, e = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	const char *pos, *end, *l;
	size_t len, n = 3, k;
	char **lines;
	int i = 0;

	/*
	 * one git for the top, the prefix, our directory and the paths
	 * asked for later
	 */
	for (k = 0; known_paths[k]; k++)
		n++;
	lines = xcalloc(n, sizeof(*lines));
	argv_pushl(&a, "rev-parse", "--show-toplevel", "--show-prefix",
		   "--git-path", "carve", NULL);
	for (k = 0; known_paths[k]; k++)
		argv_pushl(&a, "--git-path", known_paths[k], NULL);
	if (run_git(&a, NULL, &out, &e)) {
		argv_clear(&a);
		sb_release(&out);
		free(lines);
		if (!strstr(sb_str(&e), "not a git repository")) {
			git_fail(r, "git rev-parse", &e);
			sb_release(&e);
			return -1;
		}
		sb_release(&e);
		return repo_err(r, NOT_A_WORK_TREE_MSG);
	}
	sb_release(&e);
	argv_clear(&a);
	pos = sb_str(&out);
	end = sb_str(&out) + out.len;
	while ((size_t)i < n && (l = next_line(&pos, end, &len)))
		lines[i++] = xstrndup(l, len);
	sb_release(&out);
	if (i < 3 || !*lines[0]) {
		for (k = 0; k < n; k++)
			free(lines[k]);
		free(lines);
		return repo_err(r, NOT_A_WORK_TREE_MSG);
	}
	/* --git-path is relative to the cwd (top + prefix): made absolute */
	sb_addf(&cwd, "%s/%s", lines[0], lines[1]);
	for (k = 2; k < (size_t)i; k++) {
		if (lines[k][0] != '/') {
			struct strbuf p = STRBUF_INIT;

			sb_addf(&p, "%s%s", cwd.buf, lines[k]);
			free(lines[k]);
			lines[k] = sb_detach(&p);
		}
		if (k >= 3)
			gpath_add(r, known_paths[k - 3], lines[k]);
	}
	sb_release(&cwd);
	if (chdir(lines[0])) {
		repo_err(r, "cannot chdir to %s: %s", lines[0],
			 strerror(errno));
		for (k = 0; k < 3; k++)
			free(lines[k]);
		free(lines);
		return -1;
	}
	r->top = lines[0];
	r->prefix = lines[1];
	r->carve_dir = lines[2];
	free(lines);
	return 0;
}

void repo_release(struct repo *r)
{
	size_t i;

	repo_prefetch_drop(r);
	for (i = 0; i < r->nrelease; i++)
		r->release[i](r);
	r->nrelease = 0;
	free(r->top);
	free(r->prefix);
	free(r->carve_dir);
	free(r->empty_tree);
	free(r->undo_stamp);
	free(r->undo_index);
	r->undo_stamp = r->undo_index = NULL;
	sb_release(&r->undo_refs);
	sb_release(&r->last_patch);
	free(r->last_patch_from);
	r->last_patch_from = NULL;
	free(r->ie_stamp);
	free(r->ie_path);
	r->ie_stamp = r->ie_path = NULL;
	sb_release(&r->ie_line);
	repo_config_drop(r);
	worker_stop(&r->cf_check);
	worker_stop(&r->cf_cat);
	free(r->split_key);
	r->split_key = NULL;
	free(r->ix_tree);
	free(r->ix_tree_stamp);
	free(r->ab_key);
	sb_release(&r->ab_list);
	free(r->sl_id);
	free(r->sl_head);
	sb_release(&r->sl);
	for (i = 0; i < r->ngpath; i++) {
		free(r->gpath_name[i]);
		free(r->gpath[i]);
	}
	free(r->gpath_name);
	free(r->gpath);
	r->gpath_name = r->gpath = NULL;
	r->ngpath = 0;
	sb_release(&r->err);
	sb_release(&r->err_detail);
	sb_release(&r->warn);
	r->top = r->prefix = r->carve_dir = r->empty_tree = NULL;
}

const char *repo_empty_tree(struct repo *r)
{
	struct strbuf out = STRBUF_INIT;
	char *head;

	if (r->empty_tree)
		return r->empty_tree;
	/*
	 * the object format from the length of an id, without git for
	 * SHA-1 or SHA-256
	 */
	head = repo_rev(r, "HEAD");
	if (head && (strlen(head) == 40 || strlen(head) == 64)) {
		r->empty_tree = xstrdup(
			strlen(head) == 40 ?
				EMPTY_TREE_SHA1 :
				"6ef19b41225c5369f1c104d45d8d85efa9b057b53b14b4b9"
				"b939dd74decc5321");
		free(head);
		return r->empty_tree;
	}
	free(head);
	if (git_in(r, "", 0, &out, "hash-object", "-t", "tree", "--stdin",
		   NULL) == 0) {
		sb_rtrim(&out);
		r->empty_tree = sb_detach(&out);
	} else {
		sb_release(&out);
		r->empty_tree = xstrdup(EMPTY_TREE_SHA1);
	}
	return r->empty_tree;
}

char *carve_path(struct repo *r, const char *name)
{
	struct strbuf p = STRBUF_INIT;

	sb_addf(&p, "%s/%s", r->carve_dir, name);
	return sb_detach(&p);
}

/* The editor git would use, from git var GIT_EDITOR asked once. */
static char *git_editor(struct repo *r)
{
	if (!r->editor) {
		char *e = git_line(r, "var", "GIT_EDITOR", NULL);

		r->editor = e ? e : xstrdup("vi");
	}
	return xstrdup(r->editor);
}

char *repo_committer(struct repo *r)
{
	if (!r->committer) {
		/* "Name <email> <time> <tz>": the time is not kept */
		char *id = git_line(r, "var", "GIT_COMMITTER_IDENT", NULL);
		char *gt = id ? strrchr(id, '>') : NULL;

		if (!gt) {
			free(id);
			return NULL;
		}
		gt[1] = '\0';
		r->committer = id;
	}
	return xstrdup(r->committer);
}

int run_editor(struct repo *r, const char *file, int line)
{
	struct run_opts o = { .tty = true };
	struct strbuf script = STRBUF_INIT;
	char *ed = git_editor(r);
	const char *argv[] = { "sh", "-c", NULL, ed, file, NULL };
	int rc;

	/* as for git, the editor string is a shell snippet */
	if (line > 0)
		sb_addf(&script, "%s +%d \"$@\"", ed, line);
	else
		sb_addf(&script, "%s \"$@\"", ed);
	argv[2] = script.buf;
	rc = run_cmd(argv, &o, NULL, NULL);
	if (rc)
		repo_err(r, "editor '%s' failed (exit %d)", ed, rc);
	free(ed);
	sb_release(&script);
	return rc ? -1 : 0;
}

int bg_start(struct bg_proc *bp, const struct argv *args)
{
	posix_spawn_file_actions_t fa;
	struct argv a = ARGV_INIT;
	int p[2], rc;
	size_t i;

	bg_stop(bp);
	if (pipe(p))
		return -1;
	argv_push(&a, "git");
	for (i = 0; i < args->n; i++)
		argv_push(&a, args->v[i]);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addclose(&fa, p[0]);
	rc = posix_spawnp(&bp->pid, "git", &fa, NULL, a.v, environ);
	trace((const char *const *)a.v, -1);
	posix_spawn_file_actions_destroy(&fa);
	argv_clear(&a);
	close(p[1]);
	if (rc) {
		close(p[0]);
		bp->pid = 0;
		return -1;
	}
	bp->fd = p[0];
	set_nonblock(bp->fd);
	sb_reset(&bp->out);
	bp->status = -1;
	return 0;
}

int bg_pump(struct bg_proc *bp)
{
	char buf[65536];
	ssize_t n;
	int st;

	if (bp->pid <= 0)
		return -1;
	while (bp->fd >= 0) {
		n = read(bp->fd, buf, sizeof(buf));
		if (n > 0) {
			sb_add(&bp->out, buf, (size_t)n);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return 0;
		close(bp->fd); /* EOF or error */
		bp->fd = -1;
	}
	if (waitpid(bp->pid, &st, 0) < 0)
		return -1;
	bp->status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
	bp->pid = 0;
	return 1;
}

void bg_stop(struct bg_proc *bp)
{
	if (bp->pid > 0) {
		kill(bp->pid, SIGTERM);
		while (waitpid(bp->pid, NULL, 0) < 0 && errno == EINTR)
			;
	}
	if (bp->pid > 0 && bp->fd >= 0)
		close(bp->fd);
	bp->pid = 0;
	bp->fd = -1;
	sb_release(&bp->out);
}

struct prefetch {
	struct argv args;
	struct bg_proc bp;
};

void repo_prefetch(struct repo *r, const struct argv *args)
{
	struct prefetch *p;
	size_t i;

	r->pre = xrealloc(r->pre, (r->npre + 1) * sizeof(*r->pre));
	p = &r->pre[r->npre];
	memset(p, 0, sizeof(*p));
	p->bp.fd = -1;
	if (bg_start(&p->bp, args))
		return;
	for (i = 0; i < args->n; i++)
		argv_push(&p->args, args->v[i]);
	r->npre++;
}

static bool same_args(const struct argv *a, const struct argv *b)
{
	size_t i;

	if (a->n != b->n)
		return false;
	for (i = 0; i < a->n; i++)
		if (strcmp(a->v[i], b->v[i]))
			return false;
	return true;
}

static int bg_wait(struct bg_proc *bp)
{
	struct pollfd pfd = { .events = POLLIN };
	int rc;

	while (!(rc = bg_pump(bp))) {
		pfd.fd = bp->fd;
		poll(&pfd, 1, -1);
	}
	return rc;
}

/*
 * The exit status of the prefetched git args, with its output added to
 * out, or -1 when there is none. Its stderr is gone, so one that failed
 * counts as none when the error is wanted.
 */
static int take_prefetch(struct repo *r, const struct argv *args,
			 struct strbuf *out, bool want_err)
{
	struct prefetch p;
	size_t i;
	int rc;

	for (i = 0; i < r->npre && !same_args(&r->pre[i].args, args); i++)
		;
	if (i == r->npre)
		return -1;
	p = r->pre[i];
	r->pre[i] = r->pre[--r->npre];
	rc = bg_wait(&p.bp) == 1 ? p.bp.status : -1;
	if (rc > 0 && want_err)
		rc = -1;
	if (rc >= 0 && out)
		sb_add(out, p.bp.out.buf, p.bp.out.len);
	bg_stop(&p.bp);
	argv_clear(&p.args);
	return rc;
}

int git_argv(struct repo *r, const struct argv *args, struct strbuf *out,
	     struct strbuf *errb)
{
	int rc = r->npre ? take_prefetch(r, args, out, errb) : -1;

	return rc >= 0 ? rc : run_git(args, NULL, out, errb);
}

void repo_prefetch_startup(struct repo *r)
{
	struct argv a[] = { ARGV_INIT, ARGV_INIT, ARGV_INIT };
	size_t i;

	argv_pushl(&a[0], HEAD_INFO_ARGS, NULL);
	argv_pushl(&a[1], CONFIG_ARGS, NULL);
	argv_pushl(&a[2], UPSTREAM_ARGS, NULL);
	for (i = 0; i < ARRAY_SIZE(a); i++) {
		repo_prefetch(r, &a[i]);
		argv_clear(&a[i]);
	}
}

void repo_prefetch_drop(struct repo *r)
{
	while (r->npre) {
		struct prefetch *p = &r->pre[--r->npre];

		bg_stop(&p->bp);
		argv_clear(&p->args);
	}
	free(r->pre);
	r->pre = NULL;
}

const char *repo_git_path(struct repo *r, const char *name)
{
	struct strbuf abs = STRBUF_INIT;
	size_t i;
	char *p;

	for (i = 0; i < r->ngpath; i++)
		if (!strcmp(r->gpath_name[i], name))
			return r->gpath[i];
	p = git_line(r, "rev-parse", "--git-path", name, NULL);
	if (!p)
		return NULL;
	/* relative to the top, where repo_open went */
	if (p[0] != '/' && r->top)
		sb_addf(&abs, "%s/", r->top);
	sb_adds(&abs, p);
	free(p);
	gpath_add(r, name, sb_detach(&abs));
	return r->gpath[r->ngpath - 1];
}

/*
 * The index file's own checksum (its last 20 bytes, hex), or NULL when
 * there is none to trust (no index, index.skipHash). Nearly free, unlike
 * git write-tree on a large index.
 */
char *repo_index_path(struct repo *r)
{
	const char *env = getenv("GIT_INDEX_FILE");
	struct strbuf path = STRBUF_INIT;
	char *slash;

	if (env && *env)
		return xstrdup(env);
	/* carve_dir is $GIT_DIR/carve of this worktree */
	sb_adds(&path, r->carve_dir);
	slash = strrchr(path.buf, '/');
	if (!slash) {
		sb_release(&path);
		return NULL;
	}
	path.len = (size_t)(slash - path.buf);
	path.buf[path.len] = '\0';
	sb_adds(&path, "/index");
	return sb_detach(&path);
}

char *repo_index_stamp(struct repo *r)
{
	char *path = repo_index_path(r), *res = NULL;
	struct strbuf sb = STRBUF_INIT;
	unsigned char tail[20];
	struct stat s;
	size_t i;
	FILE *f;

	f = path ? fopen(path, "rb") : NULL;
	if (!f)
		goto out;
	if (!fstat(fileno(f), &s) && s.st_size > 40 &&
	    !fseek(f, -20L, SEEK_END) && fread(tail, 1, 20, f) == 20) {
		bool zero = true;

		for (i = 0; i < 20; i++) {
			zero &= !tail[i];
			sb_addf(&sb, "%02x", tail[i]);
		}
		sb_addf(&sb, "-%ld", (long)s.st_size);
		if (!zero)
			res = sb_detach(&sb);
	}
	fclose(f);
out:
	free(path);
	sb_release(&sb);
	return res;
}

int index_entry_of(struct repo *r, const char *path, struct strbuf *line)
{
	char *stamp = repo_index_stamp(r);
	int rc;

	/* the same index file as last time, asked for the same path */
	if (stamp && r->ie_stamp && !strcmp(stamp, r->ie_stamp) && r->ie_path &&
	    !strcmp(path, r->ie_path)) {
		free(stamp);
		if (line)
			sb_add(line, r->ie_line.buf ? r->ie_line.buf : "",
			       r->ie_line.len);
		return r->ie_line.len > 0;
	}
	free(r->ie_stamp);
	free(r->ie_path);
	r->ie_stamp = r->ie_path = NULL;
	sb_reset(&r->ie_line);
	if (git_out(r, &r->ie_line, "--literal-pathspecs", "ls-files", "-s",
		    "--", path, NULL)) {
		free(stamp);
		sb_reset(&r->ie_line);
		return -1;
	}
	rc = r->ie_line.len > 0;
	if (line)
		sb_add(line, sb_str(&r->ie_line), r->ie_line.len);
	r->ie_stamp = stamp;
	r->ie_path = stamp ? xstrdup(path) : NULL;
	return rc;
}

int read_blob(struct repo *r, const char *oid, struct strbuf *out)
{
	size_t was = out->len;
	char type[16];

	/* through the running cat-file */
	if (repo_cat(r, oid, out, type) || strcmp(type, "blob")) {
		out->len = was;
		if (out->buf)
			out->buf[was] = '\0';
		return repo_err(r, "%s is no blob", oid);
	}
	return 0;
}

void repo_config_drop(struct repo *r)
{
	size_t i;

	free(r->editor);
	free(r->committer);
	r->editor = r->committer = NULL;
	for (i = 0; i < r->ncfg; i++) {
		free(r->cfg_key[i]);
		free(r->cfg_val[i]);
	}
	free(r->cfg_key);
	free(r->cfg_val);
	r->cfg_key = r->cfg_val = NULL;
	r->ncfg = 0;
	r->cfg_loaded = false;
}

static void config_load(struct repo *r)
{
	struct strbuf out = STRBUF_INIT;
	size_t alloc = 0, alloc2 = 0;
	const char *p, *end;

	if (r->cfg_loaded)
		return;
	r->cfg_loaded = true;
	/* Nothing set at all is exit 0 too. A failure leaves it empty. */
	if (git_out(r, &out, CONFIG_ARGS, NULL)) {
		sb_reset(&r->err);
		sb_release(&out);
		return;
	}
	/* "key\nvalue\0", or "key\0" for a key without a value */
	for (p = sb_str(&out), end = p + out.len; p < end; p += strlen(p) + 1) {
		const char *nl = strchr(p, '\n');

		ALLOC_GROW(r->cfg_key, r->ncfg + 1, alloc);
		ALLOC_GROW(r->cfg_val, r->ncfg + 1, alloc2);
		r->cfg_key[r->ncfg] = nl ? xstrndup(p, (size_t)(nl - p)) :
					   xstrdup(p);
		r->cfg_val[r->ncfg++] = nl ? xstrdup(nl + 1) : NULL;
	}
	sb_release(&out);
}

/* The index + 1 of key's last value, 0 if unset. */
static size_t config_find(struct repo *r, const char *key)
{
	size_t i;

	config_load(r);
	for (i = r->ncfg; i > 0; i--)
		if (!strcasecmp(r->cfg_key[i - 1], key))
			return i;
	return 0;
}

const char *repo_config(struct repo *r, const char *key)
{
	size_t i = config_find(r, key);

	if (!i)
		return NULL;
	return r->cfg_val[i - 1] ? r->cfg_val[i - 1] : "";
}

int parse_config_bool(const char *v)
{
	static const char *const no[] = { "false", "no", "off", "", NULL };
	static const char *const yes[] = { "true", "yes", "on", NULL };
	size_t i;
	char *e;
	long n;

	/* a key without "=" is true, and an empty "key =" is false */
	if (!v)
		return 1;
	for (i = 0; yes[i]; i++)
		if (!strcasecmp(v, yes[i]))
			return 1;
	for (i = 0; no[i]; i++)
		if (!strcasecmp(v, no[i]))
			return 0;
	n = strtol(v, &e, 10);
	if (e != v && !*e)
		return n != 0;
	return -1;
}

int parse_config_int(const char *v, long *out)
{
	char *e;
	long n;

	if (!v || !*v)
		return -1;
	n = strtol(v, &e, 10);
	if (e == v)
		return -1;
	/* git's unit suffixes */
	switch (*e) {
	case 'k':
	case 'K':
		n *= 1024;
		e++;
		break;
	case 'm':
	case 'M':
		n *= 1024L * 1024;
		e++;
		break;
	case 'g':
	case 'G':
		n *= 1024L * 1024 * 1024;
		e++;
		break;
	default:
		break;
	}
	if (*e)
		return -1;
	*out = n;
	return 0;
}

char *repo_config_dup(struct repo *r, const char *key)
{
	const char *v = repo_config(r, key);

	return v ? xstrdup(v) : NULL;
}

bool repo_config_bool(struct repo *r, const char *key, bool def)
{
	size_t k = config_find(r, key);
	int b;

	b = k ? parse_config_bool(r->cfg_val[k - 1]) : -1;

	return b < 0 ? def : b;
}

long repo_config_int(struct repo *r, const char *key, long def)
{
	long n;

	return parse_config_int(repo_config(r, key), &n) ? def : n;
}

size_t repo_config_all(struct repo *r, const char *key, bool values_only,
		       struct strbuf *out)
{
	size_t kl = strlen(key);
	size_t n = 0;
	bool prefix;
	size_t i;

	prefix = kl && key[kl - 1] == '.';

	config_load(r);
	for (i = 0; i < r->ncfg; i++) {
		if (prefix ? strncasecmp(r->cfg_key[i], key, kl) :
			     strcasecmp(r->cfg_key[i], key))
			continue;
		if (values_only)
			sb_addf(out, "%s\n",
				r->cfg_val[i] ? r->cfg_val[i] : "");
		else if (r->cfg_val[i])
			sb_addf(out, "%s %s\n", r->cfg_key[i], r->cfg_val[i]);
		else
			sb_addf(out, "%s\n", r->cfg_key[i]);
		n++;
	}
	return n;
}

/* Start git cat-file <mode> with pipes both ways, unless it runs. */
static int worker_start(struct cat_worker *w, const char *mode)
{
	const char *argv[] = { "git", "cat-file", mode, NULL };
	posix_spawn_file_actions_t fa;
	int pin[2], pout[2], rc;

	if (w->pid > 0)
		return 0;
	if (pipe(pin))
		return -1;
	if (pipe(pout)) {
		close_pair(pin);
		return -1;
	}
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pin[0], 0);
	posix_spawn_file_actions_adddup2(&fa, pout[1], 1);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addclose(&fa, pin[1]);
	posix_spawn_file_actions_addclose(&fa, pout[0]);
	rc = posix_spawnp(&w->pid, "git", &fa, NULL, (char *const *)argv,
			  environ);
	posix_spawn_file_actions_destroy(&fa);
	trace(argv, -1);
	close(pin[0]);
	close(pout[1]);
	if (rc) {
		close(pin[1]);
		close(pout[0]);
		w->pid = 0;
		return -1;
	}
	w->in = pin[1];
	w->out = pout[0];
	/* not for the other children (an editor would keep it alive) */
	fcntl(w->in, F_SETFD, FD_CLOEXEC);
	fcntl(w->out, F_SETFD, FD_CLOEXEC);
	sb_reset(&w->buf);
	return 0;
}

static void worker_stop(struct cat_worker *w)
{
	if (w->pid <= 0)
		return;
	close(w->in);
	close(w->out);
	while (waitpid(w->pid, NULL, 0) < 0 && errno == EINTR)
		;
	w->pid = 0;
	sb_release(&w->buf);
}

/* Read until buf holds n bytes. Returns -1 when the worker is gone. */
static int worker_fill(struct cat_worker *w, size_t n)
{
	char chunk[65536];

	while (w->buf.len < n) {
		ssize_t got = read(w->out, chunk, sizeof(chunk));

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return -1;
		sb_add(&w->buf, chunk, (size_t)got);
	}
	return 0;
}

/* Take the first n bytes of buf into out (if not NULL). */
static void worker_take(struct cat_worker *w, size_t n, struct strbuf *out)
{
	if (out)
		sb_add(out, w->buf.buf, n);
	memmove(w->buf.buf, w->buf.buf + n, w->buf.len - n);
	w->buf.len -= n;
	w->buf.buf[w->buf.len] = '\0';
}

/* Ask for rev on a line, and put the first line of the answer into line. */
static int worker_ask(struct cat_worker *w, const char *mode, const char *rev,
		      struct strbuf *line)
{
	struct strbuf q = STRBUF_INIT;
	size_t off = 0;
	const char *p;
	int tries;

	/* one restart if the worker went away (killed, git upgraded) */
	for (tries = 0; tries < 2; tries++) {
		if (worker_start(w, mode))
			return -1;
		sb_reset(&q);
		sb_addf(&q, "%s\n", rev);
		for (off = 0; off < q.len;) {
			ssize_t n = write(w->in, q.buf + off, q.len - off);

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			off += (size_t)n;
		}
		if (off < q.len) {
			worker_stop(w);
			continue;
		}
		while (!(p = memchr(sb_str(&w->buf), '\n', w->buf.len)))
			if (worker_fill(w, w->buf.len + 1))
				break;
		if (!p) {
			worker_stop(w);
			continue;
		}
		worker_take(w, (size_t)(p - sb_str(&w->buf)), line);
		worker_take(w, 1, NULL);
		sb_release(&q);
		return 0;
	}
	sb_release(&q);
	return -1;
}

/*
 * What a cat-file started earlier still answers right. That excludes an
 * index entry (":path"), since it reads the index once, anything that
 * depends on the configuration ("@{upstream}"), read once too, or on the
 * reflogs ("@{1}"), and a newline.
 */
static bool worker_can(const char *rev)
{
	return *rev && *rev != ':' && !strstr(rev, "@{") && !strchr(rev, '\n');
}

char *repo_rev(struct repo *r, const char *rev)
{
	struct strbuf l = STRBUF_INIT;
	char *oid = NULL;
	const char *sp;

	if (!worker_can(rev))
		return git_line(r, "rev-parse", "-q", "--verify", rev, NULL);
	/* "<oid> <type> <size>", or "<rev> missing" / "ambiguous" */
	if (!worker_ask(&r->cf_check, "--batch-check", rev, &l) &&
	    (sp = strchr(sb_str(&l), ' ')) && strcmp(sp, " missing") &&
	    strcmp(sp, " ambiguous"))
		oid = xstrndup(l.buf, (size_t)(sp - l.buf));
	sb_release(&l);
	return oid;
}

/* A cat-file --batch header "<oid> <type> <size>": its type and size. */
static int batch_header(const char *h, char type[16], long *size)
{
	const char *t = strchr(h, ' '), *end;

	if (!t)
		return -1;
	t++;
	end = strchr(t, ' ');
	if (!end || end - t > 15)
		return -1;
	memcpy(type, t, (size_t)(end - t));
	type[end - t] = '\0';
	return parse_long(end + 1, NULL, size);
}

long repo_size(struct repo *r, const char *rev)
{
	struct strbuf l = STRBUF_INIT;
	long n = -1;
	char t[16];
	char *s;

	if (rev[0] == ':' && rev[1] &&
	    !(rev[1] >= '0' && rev[1] <= '3' && rev[2] == ':')) {
		/* an index entry's blob comes from the ls-files answer kept */
		struct strbuf e = STRBUF_INIT;
		const char *sp;

		if (index_entry_of(r, rev + 1, &e) == 1 &&
		    (sp = strchr(sb_str(&e), ' '))) {
			char *oid = xstrndup(sp + 1, strcspn(sp + 1, " "));

			n = repo_size(r, oid);
			free(oid);
		}
		sb_release(&e);
		return n;
	}
	if (!worker_can(rev)) {
		s = git_line(r, "cat-file", "-s", rev, NULL);
		n = number_or(s, -1);
		free(s);
		sb_reset(&r->err);
		return n;
	}
	if (!worker_ask(&r->cf_check, "--batch-check", rev, &l) &&
	    batch_header(sb_str(&l), t, &n))
		n = -1;
	sb_release(&l);
	return n;
}

int repo_rev_status(struct repo *r, const char *rev)
{
	char *oid = repo_rev(r, rev);

	free(oid);
	return oid ? 0 : 1;
}

int repo_cat(struct repo *r, const char *rev, struct strbuf *out, char *type)
{
	char t[16]; /* commit, tree, blob, tag */
	long size;
	struct cat_worker *w = &r->cf_cat;
	struct strbuf l = STRBUF_INIT;
	int rc = -1;

	if (!worker_can(rev)) {
		/* an index entry goes through git itself */
		char *oid =
			git_line(r, "rev-parse", "-q", "--verify", rev, NULL);

		if (oid)
			rc = repo_cat(r, oid, out, type);
		free(oid);
		return rc;
	}
	/* the header, then the content and a newline */
	if (worker_ask(w, "--batch", rev, &l) ||
	    batch_header(sb_str(&l), t, &size))
		goto out;
	if (worker_fill(w, (size_t)size + 1)) {
		worker_stop(w);
		goto out;
	}
	worker_take(w, (size_t)size, out);
	worker_take(w, 1, NULL);
	if (type)
		snprintf(type, 16, "%s", t);
	rc = 0;
out:
	sb_release(&l);
	return rc;
}

/*
 * Requests in one batch. Their lines fit in the pipe to cat-file, and the
 * write never waits for cat-file while it waits for us to read.
 */
#define CAT_BATCH 64

/* Send the requests of oids [from, to) on their lines. */
static int cat_send(struct cat_worker *w, char *const *oids, size_t from,
		    size_t to)
{
	struct strbuf q = STRBUF_INIT;
	size_t off, i;
	int rc;

	for (i = from; i < to; i++)
		sb_addf(&q, "%s\n", oids[i]);
	for (off = 0; off < q.len;) {
		ssize_t got = write(w->in, q.buf + off, q.len - off);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		off += (size_t)got;
	}
	rc = off == q.len ? 0 : -1;
	sb_release(&q);
	return rc;
}

/*
 * Read the answer to one request, with the object's content in *data, or
 * NULL when there is none. *used is how much of the buffer it takes.
 */
static int cat_answer(struct cat_worker *w, const char **data, size_t *len,
		      size_t *used)
{
	const char *nl;
	char type[16];
	long size;
	size_t hl;

	while (!(nl = memchr(sb_str(&w->buf), '\n', w->buf.len)))
		if (worker_fill(w, w->buf.len + 1))
			return -1;
	hl = (size_t)(nl - sb_str(&w->buf));
	*data = NULL;
	*len = 0;
	*used = hl + 1;
	/* "<name> missing" and the like have no content */
	if (batch_header(sb_str(&w->buf), type, &size))
		return 0;
	if (worker_fill(w, hl + 1 + (size_t)size + 1))
		return -1;
	*data = sb_str(&w->buf) + hl + 1;
	*len = (size_t)size;
	*used += (size_t)size + 1;
	return 0;
}

int repo_cat_each(struct repo *r, char *const *oids, size_t n,
		  bool (*fn)(void *ctx, size_t i, const char *data, size_t len),
		  void *ctx)
{
	struct cat_worker *w = &r->cf_cat;
	bool go = true;
	size_t i, k;

	for (i = 0; go && i < n; i += CAT_BATCH) {
		size_t to = i + CAT_BATCH < n ? i + CAT_BATCH : n;

		if (worker_start(w, "--batch") || cat_send(w, oids, i, to)) {
			worker_stop(w);
			return -1;
		}
		/* every answer is read, also after fn said stop */
		for (k = i; k < to; k++) {
			const char *data;
			size_t len, used;

			if (cat_answer(w, &data, &len, &used)) {
				worker_stop(w);
				return -1;
			}
			if (go)
				go = fn(ctx, k, data, len);
			worker_take(w, used, NULL);
		}
	}
	return 0;
}

int repo_commit_message(struct repo *r, const char *rev, struct strbuf *out)
{
	struct strbuf obj = STRBUF_INIT;
	const char *body;
	char type[16];
	size_t len;

	if (repo_cat(r, rev, &obj, type) || strcmp(type, "commit")) {
		sb_release(&obj);
		return -1;
	}
	/* the headers end at the first empty line */
	body = strstr(sb_str(&obj), "\n\n");
	body = body ? body + 2 : obj.buf + obj.len;
	len = obj.len - (size_t)(body - obj.buf);
	sb_add(out, body, len);
	sb_release(&obj);
	return 0;
}

char *repo_commit_subject(struct repo *r, const char *rev)
{
	struct strbuf msg = STRBUF_INIT, sub = STRBUF_INIT;
	const char *p, *end;

	if (repo_commit_message(r, rev, &msg)) {
		sb_release(&msg);
		return NULL;
	}
	/* the first paragraph, its lines joined by spaces, as %s */
	p = sb_str(&msg);
	while (*p == '\n')
		p++;
	for (; *p; p = end + 1) {
		size_t n;

		end = strchr(p, '\n');
		n = end ? (size_t)(end - p) : strlen(p);
		while (n && (p[n - 1] == ' ' || p[n - 1] == '\t'))
			n--;
		if (!n)
			break;
		if (sub.len)
			sb_add(&sub, " ", 1);
		sb_add(&sub, p, n);
		if (!end)
			break;
	}
	sb_release(&msg);
	if (!sub.buf)
		sb_add(&sub, "", 0);
	return sb_detach(&sub);
}

void repo_index_is(struct repo *r, const char *treeish)
{
	struct strbuf spec = STRBUF_INIT;

	free(r->ix_tree);
	free(r->ix_tree_stamp);
	sb_addf(&spec, "%s^{tree}", treeish);
	r->ix_tree = repo_rev(r, spec.buf);
	r->ix_tree_stamp = r->ix_tree ? repo_index_stamp(r) : NULL;
	sb_release(&spec);
}

char *repo_head_branch(struct repo *r, bool *known)
{
	const char *path = repo_git_path(r, "HEAD");
	char buf[512], *res = NULL, *nl;
	size_t n;
	FILE *f;

	*known = false;
	if (!path || !(f = fopen(path, "r")))
		return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
	if ((nl = strchr(buf, '\n')))
		*nl = '\0';
	if (!strncmp(buf, "ref: refs/heads/", 16)) {
		/* a reftable repository's HEAD names no real branch */
		if (strcmp(buf + 16, ".invalid")) {
			res = xstrdup(buf + 16);
			*known = true;
		}
	} else if (n >= 40 && strspn(buf, "0123456789abcdef") == strlen(buf)) {
		*known = true; /* detached */
	}
	return res;
}

char *repo_git_path_dup(struct repo *r, const char *name)
{
	const char *p = repo_git_path(r, name);

	return p ? xstrdup(p) : NULL;
}

bool repo_has_remotes(struct repo *r)
{
	const char *dir = repo_git_path(r, "refs/remotes");
	struct strbuf data = STRBUF_INIT;
	const char *packed;
	bool any = false;
	DIR *d;

	packed = repo_git_path(r, "packed-refs");

	/* the reftable backend keeps no files, ask git */
	if (git_path_exists(r, "reftable")) {
		char *one = git_line(r, "for-each-ref", "--count=1",
				     "--format=%(refname)", "refs/remotes/",
				     NULL);

		any = one && *one;
		free(one);
		sb_reset(&r->err);
		return any;
	}
	if (dir && (d = opendir(dir))) {
		struct dirent *e;

		while (!any && (e = readdir(d)))
			any = e->d_name[0] != '.';
		closedir(d);
	}
	if (!any && packed && !read_file(packed, &data))
		any = strstr(sb_str(&data), " refs/remotes/") != NULL;
	sb_release(&data);
	return any;
}

bool git_path_exists(struct repo *r, const char *name)
{
	const char *p = repo_git_path(r, name);
	struct stat s;

	return p && stat(p, &s) == 0;
}

bool have_head(struct repo *r)
{
	return repo_rev_status(r, "HEAD^{commit}") == 0;
}

char *resolve_commit(struct repo *r, const char *rev)
{
	struct strbuf spec = STRBUF_INIT;
	char *id;

	sb_addf(&spec, "%s^{commit}", rev);
	id = repo_rev(r, spec.buf);
	sb_release(&spec);
	return id;
}

char *parent_of(struct repo *r, const char *commit)
{
	struct strbuf spec = STRBUF_INIT;
	char *id;

	sb_addf(&spec, "%s^", commit);
	id = repo_rev(r, spec.buf);
	sb_release(&spec);
	return id;
}

char *repo_upstream(struct repo *r)
{
	char *up = git_line(r, UPSTREAM_ARGS, NULL);

	sb_reset(&r->err);
	if (up && !*up) {
		free(up);
		return NULL;
	}
	return up;
}

int repo_head_info(struct repo *r, char **branch, char **line)
{
	struct strbuf out = STRBUF_INIT;
	char *nl;

	*branch = *line = NULL;
	if (!git_out(r, &out, HEAD_INFO_ARGS, NULL) &&
	    (nl = strchr(sb_str(&out), '\n'))) {
		const char *arrow = strstr(nl + 1, "HEAD -> "), *end;

		*line = xstrndup(out.buf, (size_t)(nl - out.buf));
		/* the names are separated by ", " (a name has no space) */
		if (arrow) {
			arrow += 8;
			end = strstr(arrow, ", ");
			*branch = xstrndup(arrow, end ? (size_t)(end - arrow) :
							strcspn(arrow, "\n"));
		}
	} else {
		/* before the first commit, the branch it will be on */
		*branch = git_line(r, "symbolic-ref", "--short", "-q", "HEAD",
				   NULL);
		sb_reset(&r->err);
	}
	sb_release(&out);
	return 0;
}

char *repo_commit_oneline(struct repo *r, const char *rev, bool quote)
{
	return git_line(r, "log", "-1", "--no-color",
			quote ? "--format=%h \"%s\"" : "--format=%h %s", rev,
			"--", NULL);
}

int repo_log_commits(struct repo *r, const char *format, char *const *ids,
		     size_t n, struct strbuf *out)
{
	struct strbuf fmt = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	size_t i;
	int rc;

	if (!n)
		return 0;
	sb_addf(&fmt, "--format=%s", format);
	argv_pushl(&a, "log", "--no-walk=unsorted", "--no-color", fmt.buf,
		   NULL);
	for (i = 0; i < n; i++)
		argv_push(&a, ids[i]);
	rc = run_git(&a, NULL, out, NULL);
	argv_clear(&a);
	sb_release(&fmt);
	return rc;
}

int repo_recent_commits(struct repo *r, const char *rev, int n,
			struct strbuf *out)
{
	char num[32];

	snprintf(num, sizeof(num), "-n%d", n);
	return git_out(r, out, "log", "--no-color", "--no-merges", num,
		       "--format=%H %h %s", rev, "--", NULL);
}

int repo_first_parents(struct repo *r, const char *rev, size_t count,
		       const char *except, struct strbuf *out)
{
	struct strbuf excl = STRBUF_INIT;
	char num[32];
	int rc;

	snprintf(num, sizeof(num), "-n%zu", count);
	sb_addf(&excl, "--decorate-refs-exclude=refs/heads/%s",
		except ? except : "");
	/* the branches at each commit come with it (%D): no git more */
	rc = git_out(r, out, "log", "--first-parent", "--no-color",
		     "--decorate=full", "--decorate-refs=refs/heads/",
		     "--decorate-refs=refs/remotes/", excl.buf,
		     "--format=%H%x1f%P%x1f%D%x1f%h %s", num, rev, "--", NULL);
	sb_release(&excl);
	return rc;
}
