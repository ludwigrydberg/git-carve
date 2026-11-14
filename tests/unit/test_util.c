// SPDX-License-Identifier: MIT
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "git.h"
#include "opstate.h"
#include "unit.h"
#include "util.h"

static void test_strbuf(void)
{
	struct strbuf sb = STRBUF_INIT;

	CHECK_STR(sb_str(&sb), "");
	sb_adds(&sb, "abc");
	sb_addch(&sb, '-');
	sb_addf(&sb, "%d/%s", 42, "x");
	CHECK_STR(sb.buf, "abc-42/x");
	CHECK_INT(sb.len, 8);
	sb_adds(&sb, "  \n");
	sb_rtrim(&sb);
	CHECK_STR(sb.buf, "abc-42/x");
	sb_release(&sb);
}

static void test_next_line(void)
{
	const char *s = "a\nbc\n\nd";
	const char *pos = s;
	const char *end;
	const char *l;
	size_t len;
	int n = 0;

	end = s + strlen(s);

	while ((l = next_line(&pos, end, &len))) {
		if (n == 0)
			CHECK(len == 1 && l[0] == 'a');
		if (n == 1)
			CHECK(len == 2 && !memcmp(l, "bc", 2));
		if (n == 2)
			CHECK_INT(len, 0);
		if (n == 3)
			CHECK(len == 1 && l[0] == 'd');
		n++;
	}
	CHECK_INT(n, 4);
}

static void test_version(void)
{
	int ma = 0, mi = 0;

	CHECK_INT(parse_git_version("git version 2.25.1", &ma, &mi), 0);
	CHECK_INT(ma, 2);
	CHECK_INT(mi, 25);
	CHECK_INT(parse_git_version("git version 2.39.3 (Apple Git-146)", &ma,
				    &mi),
		  0);
	CHECK_INT(mi, 39);
	CHECK(parse_git_version("hg 1.0", &ma, &mi) != 0);
}

static void test_run_cmd(void)
{
	const char *argv[] = { "sh", "-c", "cat; echo err >&2; exit 3", NULL };
	struct strbuf out = STRBUF_INIT, err = STRBUF_INIT;
	struct run_opts o = { .in = "hello", .inlen = 5 };

	CHECK_INT(run_cmd(argv, &o, &out, &err), 3);
	CHECK_STR(out.buf, "hello");
	CHECK_STR(err.buf, "err\n");
	sb_release(&out);
	sb_release(&err);
}

static void test_run_cmd_big(void)
{
	struct strbuf in = STRBUF_INIT, out = STRBUF_INIT;
	/* Large stdin and stdout at once must not deadlock. */
	const char *argv[] = { "cat", NULL };
	struct run_opts o;
	int i;

	for (i = 0; i < 200000; i++)
		sb_adds(&in, "0123456789");
	o = (struct run_opts){ .in = in.buf, .inlen = in.len };
	CHECK_INT(run_cmd(argv, &o, &out, NULL), 0);
	CHECK_INT(out.len, in.len);
	CHECK(out.len == in.len && !memcmp(out.buf, in.buf, in.len));
	sb_release(&in);
	sb_release(&out);
}

static void test_bg(void)
{
	struct bg_proc bp = { .fd = -1 };
	struct argv a = ARGV_INIT;
	int rc, spins = 0;

	argv_push(&a, "--version");
	CHECK_INT(bg_start(&bp, &a), 0);
	CHECK(bg_running(&bp));
	while ((rc = bg_pump(&bp)) == 0 && spins++ < 100000)
		usleep(1000);
	CHECK_INT(rc, 1);
	CHECK_INT(bp.status, 0);
	CHECK(!bg_running(&bp));
	CHECK(starts_with(sb_str(&bp.out), "git version "));
	bg_stop(&bp);

	/* a stopped scan leaves nothing behind */
	argv_clear(&a);
	argv_pushl(&a, "-c", "alias.zz=!sleep 2", "zz", NULL);
	CHECK_INT(bg_start(&bp, &a), 0);
	bg_stop(&bp);
	CHECK(!bg_running(&bp));
	argv_clear(&a);
}

static void test_smart_match(void)
{
	const char *s = "static int Foo_bar(void)";

	CHECK(smart_match(s, strlen(s), "foo_bar")); /* lower case, any case */
	CHECK(smart_match(s, strlen(s), "Foo"));
	CHECK(!smart_match(s, strlen(s), "FOO")); /* upper case, exact */
	CHECK(!smart_match(s, strlen(s), "foO"));
	CHECK(smart_match(s, strlen(s), ""));
	CHECK(!smart_match(s, 5, "static")); /* only within len */
	CHECK(smart_match(s, 6, "static"));
	/*
	 * the scan for the first character in both cases, when one case
	 * comes before the other, near misses and a match at the very end
	 */
	CHECK(smart_match("xAAAab", 6, "ab"));
	CHECK(smart_match("aaaaAb", 6, "ab"));
	CHECK(!smart_match("aaaaAa", 6, "ab"));
	CHECK(smart_match("aaab", 4, "aab"));
	CHECK(smart_match("xyzEND", 6, "end"));
	CHECK(!smart_match("xyzEN", 5, "end"));
	CHECK(smart_match("1-2-3", 5, "-3"));
	CHECK(!smart_match("ab", 2, "abc"));
}

/* sh -c cmd in the cwd, 0 on success */
static int sh(const char *cmd)
{
	const char *argv[] = { "sh", "-c", cmd, NULL };

	return run_cmd(argv, NULL, NULL, NULL);
}

/*
 * State files are replaced whole. A write that fails keeps the old
 * content and leaves no temporary file behind.
 */
static void test_state_write(void)
{
	char dir[] = "/tmp/carve-unit.XXXXXX", cwd[4096];
	struct strbuf sb = STRBUF_INIT;
	struct repo r = { 0 };
	char *tmp;

	if (!getcwd(cwd, sizeof(cwd)) || !mkdtemp(dir) || chdir(dir))
		return;
	setenv("HOME", dir, 1);
	setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
	unsetenv("GIT_DIR");
	CHECK_INT(sh("git init -q ."), 0);
	CHECK_INT(repo_open(&r), 0);
	CHECK_INT(state_write(&r, "tip", "one", 3), 0);
	CHECK_INT(state_write(&r, "tip", "two", 3), 0);
	CHECK_INT(state_read(&r, "tip", &sb), 0);
	CHECK_STR(sb_str(&sb), "two");
	/* the temporary file cannot be made: the old state stays */
	tmp = carve_path(&r, "tip.tmp");
	CHECK_INT(mkdir(tmp, 0777), 0);
	CHECK_INT(state_write(&r, "tip", "three", 5), -1);
	sb_reset(&sb);
	CHECK_INT(state_read(&r, "tip", &sb), 0);
	CHECK_STR(sb_str(&sb), "two");
	rmdir(tmp);
	/* a temporary file left by a crash goes with the state */
	CHECK_INT(write_file(tmp, "x", 1), 0);
	state_remove(&r);
	CHECK(access(tmp, F_OK) != 0);
	free(tmp);
	repo_release(&r);
	sb_reset(&sb);
	if (chdir(cwd))
		return;
	sb_addf(&sb, "rm -rf '%s'", dir);
	sh(sb.buf);
	sb_release(&sb);
}

/* repo_rev / repo_cat / commit messages through the cat-file workers */
static void test_cat_worker(void)
{
	char dir[] = "/tmp/carve-unit.XXXXXX", cwd[4096];
	char *head, *tree, *v, type[16];
	struct strbuf sb = STRBUF_INIT;
	struct repo r = { 0 };

	if (!getcwd(cwd, sizeof(cwd)) || !mkdtemp(dir) || chdir(dir))
		return;
	setenv("HOME", dir, 1);
	setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
	unsetenv("GIT_DIR");
	CHECK_INT(sh("git init -q . && printf 'a b\\n' >'f 1' && git add . && "
		     "git -c user.name=T -c user.email=t@t commit -q "
		     "-m 'sub: one' -m 'line two' -m 'body'"),
		  0);
	CHECK_INT(repo_open(&r), 0);
	head = repo_rev(&r, "HEAD");
	CHECK(head && strlen(head) >= 40);
	tree = repo_rev(&r, "HEAD^{tree}");
	CHECK(tree && head && strcmp(tree, head));
	CHECK(repo_rev(&r, "refs/nope") == NULL);
	/* a path with a space, through the tree */
	v = repo_rev(&r, "HEAD:f 1");
	CHECK(v != NULL);
	free(v);
	CHECK_INT(repo_cat(&r, "HEAD:f 1", &sb, type), 0);
	CHECK_STR(sb.buf, "a b\n");
	CHECK_STR(type, "blob");
	sb_reset(&sb);
	CHECK_INT(repo_commit_message(&r, "HEAD", &sb), 0);
	CHECK_STR(sb.buf, "sub: one\n\nline two\n\nbody\n");
	v = repo_commit_subject(&r, "HEAD");
	CHECK_STR(v, "sub: one");
	free(v);
	/* new objects and refs are seen by the running workers */
	CHECK_INT(sh("git update-ref refs/carve/x HEAD^{tree}"), 0);
	v = repo_rev(&r, "refs/carve/x");
	CHECK_STR(v, tree);
	free(v);
	/* an index entry goes to rev-parse */
	v = repo_rev(&r, ":f 1");
	CHECK(v != NULL);
	free(v);
	/* the upstream comes from the configuration, read again each time */
	CHECK(repo_rev(&r, "@{upstream}") == NULL);
	CHECK_INT(sh("git branch -q up && git branch -q --set-upstream-to=up"),
		  0);
	v = repo_rev(&r, "@{upstream}");
	CHECK_STR(v, head);
	free(v);
	/* a worker that died is started again */
	kill(r.cf_check.pid, SIGKILL);
	v = repo_rev(&r, "HEAD");
	CHECK_STR(v, head);
	free(v);
	free(head);
	free(tree);
	sb_reset(&sb);
	repo_release(&r);
	if (chdir(cwd))
		return;
	sb_addf(&sb, "rm -rf '%s'", dir);
	sh(sb.buf);
	sb_release(&sb);
}

/* git config values as git reads them */
static void test_config_values(void)
{
	long n = 0;

	CHECK_INT(parse_config_bool(NULL), 1); /* "[carve] signoff" */
	CHECK_INT(parse_config_bool(""), 0);
	CHECK_INT(parse_config_bool("Yes"), 1);
	CHECK_INT(parse_config_bool("off"), 0);
	CHECK_INT(parse_config_bool("2"), 1);
	CHECK_INT(parse_config_bool("0"), 0);
	CHECK_INT(parse_config_bool("maybe"), -1);
	CHECK_INT(parse_config_int("12", &n), 0);
	CHECK_INT(n, 12);
	CHECK_INT(parse_config_int("5m", &n), 0);
	CHECK_INT(n, 5L << 20);
	CHECK_INT(parse_config_int("2k", &n), 0);
	CHECK_INT(n, 2048);
	CHECK_INT(parse_config_int("x", &n), -1);
	CHECK_INT(parse_config_int("3q", &n), -1);
	CHECK_INT(parse_config_int("", &n), -1);
}

int main(void)
{
	RUN(test_strbuf);
	RUN(test_config_values);
	RUN(test_state_write);
	RUN(test_cat_worker);
	RUN(test_bg);
	RUN(test_next_line);
	RUN(test_version);
	RUN(test_run_cmd);
	RUN(test_run_cmd_big);
	RUN(test_smart_match);
	return unit_done("util");
}
