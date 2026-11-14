// SPDX-License-Identifier: MIT
/*
 * Commit splitting. The commit being split is kept in SPLIT_REF, and its
 * message and parent live in $GIT_DIR/carve/. After "git reset --mixed
 * HEAD~1" the user stages and commits piece by piece. Finishing requires
 * the resulting tree to equal the original one.
 */
#include "split.h"
#include "message.h"
#include "status.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diff.h"
#include "patch.h"
#include "series.h"
#include "opstate.h"
#include "rebase.h"
#include "cdiff.h"

/* ---- state files ------------------------------------------------------ */

static char *orig_commit(struct repo *r)
{
	return repo_rev(r, SPLIT_REF "^{commit}");
}

static char *target_tree(struct repo *r, const char *orig);

static char *tree_of(struct repo *r, const char *rev)
{
	struct strbuf spec = STRBUF_INIT;
	char *t;

	sb_addf(&spec, "%s^{tree}", rev);
	t = repo_rev(r, spec.buf);
	sb_release(&spec);
	return t;
}

static char *index_tree(struct repo *r)
{
	return git_line(r, "write-tree", NULL);
}

/* Number of commits made since the split started. */
static int commits_made(struct repo *r)
{
	struct strbuf base = STRBUF_INIT, range = STRBUF_INIT;
	int count = 0;
	char *n;

	if (!have_head(r))
		return 0;
	state_read(r, "base", &base);
	sb_rtrim(&base);
	if (base.len)
		sb_addf(&range, "%s..HEAD", base.buf);
	else
		sb_adds(&range, "HEAD");
	n = git_line(r, "rev-list", "--count", range.buf, NULL);
	count = (int)number_or(n, 0);
	free(n);
	sb_release(&base);
	sb_release(&range);
	return count;
}

/* ---- messages --------------------------------------------------------- */

enum keep_policy { KEEP_FIRST, KEEP_LAST, KEEP_NONE };

static enum keep_policy keep_policy(struct repo *r)
{
	char *v = repo_config_dup(r, "carve.keepChangeId");
	enum keep_policy p = KEEP_FIRST;

	if (v && !strcmp(v, "last"))
		p = KEEP_LAST;
	else if (v && !strcmp(v, "none"))
		p = KEEP_NONE;
	else if (v && strcmp(v, "first"))
		repo_warn(r,
			  "warning: carve.keepChangeId=%s is not one of "
			  "first, last, none; using first",
			  v);
	free(v);
	return p;
}

/* ---- commit ----------------------------------------------------------- */

/* Add "Change-Id: id" as a trailer unless msg already has one. */
static int add_change_id(struct repo *r, const char *msg, const char *id,
			 struct strbuf *out)
{
	struct strbuf t = STRBUF_INIT, in = STRBUF_INIT;
	int rc;

	if (strstr(msg, "\nChange-Id:") || starts_with(msg, "Change-Id:")) {
		sb_adds(out, msg);
		return 0;
	}
	sb_addf(&t, "Change-Id: %s", id);
	ended_line(&in, msg, strlen(msg));
	rc = git_in(r, in.buf, in.len, out, "interpret-trailers", "--trailer",
		    t.buf, NULL);
	sb_release(&t);
	sb_release(&in);
	return rc ? -1 : 0;
}

enum msg_mode { MSG_FILE, MSG_EDIT_FILE, MSG_TEMPLATE };

/*
 * MSG_EDIT_FILE (-F -e, or -m -e) fills the editor and accepts it
 * unchanged. MSG_TEMPLATE (-t) keeps leading blank lines, which -F strips,
 * and aborts when left unedited. With no message at all git opens an empty
 * editor, where -s is safe since there is nothing to duplicate.
 */
static int run_commit(struct repo *r, const struct commit_opts *o,
		      const char *file, enum msg_mode mode)
{
	struct run_opts ro = { .tty = true };
	struct strbuf err = STRBUF_INIT;
	struct argv a = ARGV_INIT;
	int rc;

	if (mode == MSG_TEMPLATE || mode == MSG_EDIT_FILE) {
		/*
		 * Our templates keep their notes below a scissors line.
		 * Only that part goes, and a line of the message starting
		 * with the comment character (#define) stays.
		 */
		char cc[32];

		snprintf(cc, sizeof(cc), "core.commentChar=%c",
			 commit_comment_char(r));
		argv_pushl(&a, "-c", cc, "commit", "--cleanup=scissors", NULL);
	} else {
		argv_push(&a, "commit");
	}
	if (o->quiet)
		argv_push(&a, "-q");
	if (o->amend)
		argv_push(&a, "--amend");
	if (o->no_status)
		argv_push(&a, "--no-status");
	if (file && mode == MSG_TEMPLATE) {
		argv_pushl(&a, "-t", file, NULL);
		if (o->note && !o->no_status)
			argv_push(&a, "--no-status");
	} else if (file) {
		argv_pushl(&a, "-F", file, NULL);
		if (mode == MSG_EDIT_FILE)
			argv_push(&a, "-e");
	} else if (o->message) {
		argv_pushl(&a, "-m", o->message, NULL);
		if (mode == MSG_EDIT_FILE)
			argv_push(&a, "-e");
	} else if (o->signoff) {
		argv_push(&a, "-s");
	}
	rc = run_git(&a, &ro, NULL, &err);
	argv_clear(&a);
	if (rc) {
		repo_err(r, "git commit failed");
		repo_err_detail(r, &err);
	}
	sb_release(&err);
	return rc ? -1 : 0;
}

/*
 * A commit in the editor that starts from "prefix: ", the subsystem the
 * staged files' history uses. Left unedited, git aborts as usual.
 */
static int prefixed_commit(struct repo *r, const struct commit_opts *o,
			   const char *prefix)
{
	struct strbuf t = STRBUF_INIT;
	char *path, *line = NULL;
	int rc = -1;

	if (prefix)
		sb_addf(&t, "%s: \n", prefix);
	else
		sb_addch(&t, '\n');
	if (o->signoff) {
		line = signoff_line(r);
		if (!line)
			goto out;
		sb_addf(&t, "\n%s\n", line);
	}
	if (o->note) {
		char cc = commit_comment_char(r);
		const char *p = o->note;

		sb_addch(&t, '\n');
		add_scissors(&t, cc);
		while (*p) {
			size_t n = strcspn(p, "\n");

			if (n)
				sb_addf(&t, "%c %.*s\n", cc, (int)n, p);
			else
				sb_addf(&t, "%c\n", cc);
			p += n + (p[n] ? 1 : 0);
		}
	}
	if (state_write(r, "COMMIT_TEMPLATE", t.buf, t.len))
		goto out;
	path = carve_path(r, "COMMIT_TEMPLATE");
	rc = run_commit(r, o, path, MSG_TEMPLATE);
	unlink(path);
	free(path);
	/* the directory goes unless an operation keeps state in it */
	rmdir(r->carve_dir);
out:
	free(line);
	sb_release(&t);
	return rc;
}

/* A commit outside a split's message handling, plain or an amend. */
static int plain_commit(struct repo *r, const struct commit_opts *o)
{
	struct strbuf given = STRBUF_INIT, text = STRBUF_INIT;
	enum msg_mode mode = MSG_FILE;
	struct commit_opts so = *o;
	int rc = -1;

	if (!o->message && !o->file && !o->amend) {
		char *pfx = suggest_prefix(r);

		if (pfx || o->note) {
			rc = prefixed_commit(r, o, pfx);
			free(pfx);
			return rc;
		}
	}
	if (!o->signoff)
		return run_commit(r, o, o->file, MSG_FILE);
	if (o->message) {
		sb_adds(&given, o->message);
	} else if (o->file) {
		if (read_file(o->file, &given)) {
			repo_err(r, "cannot read %s", o->file);
			goto out;
		}
	} else if (o->amend) {
		/* the editor starts from HEAD's message, signed off */
		if (repo_commit_message(r, "HEAD", &given))
			goto out;
		mode = MSG_EDIT_FILE;
	} else {
		rc = run_commit(r, o, NULL, MSG_FILE); /* git commit -s */
		goto out;
	}
	if (add_signoff(r, sb_str(&given), &text))
		goto out;
	so.message = sb_str(&text);
	so.file = NULL;
	rc = run_commit(r, &so, NULL, mode);
out:
	sb_release(&given);
	sb_release(&text);
	return rc;
}

int commit_template(struct repo *r, bool signoff, struct strbuf *out)
{
	char *orig = orig_commit(r), *line = NULL, *pfx = NULL;
	struct strbuf msg = STRBUF_INIT, t = STRBUF_INIT;
	enum keep_policy pol;
	int rc = -1;
	bool first;

	if (signoff && !(line = signoff_line(r)))
		goto out;
	if (!orig) {
		/* a plain commit gets the prefix, then the sign-off */
		pfx = suggest_prefix(r);
		if (pfx)
			sb_addf(&t, "%s: \n", pfx);
		else
			sb_addch(&t, '\n');
		if (line)
			sb_addf(&t, "\n%s\n", line);
		sb_add(out, t.buf, t.len);
		rc = 0;
		goto out;
	}
	/* a part of a split, as carve_commit's editor starts it */
	first = commits_made(r) == 0;
	pol = keep_policy(r);
	state_read(r, "msg", &msg);
	split_template(sb_str(&msg), first, pol == KEEP_FIRST && first,
		       commit_comment_char(r), first ? NULL : line, &t);
	if (first && line) {
		if (add_signoff(r, sb_str(&t), out))
			goto out;
	} else if (!first && (pfx = suggest_prefix(r))) {
		sb_addf(out, "%s: %s", pfx, sb_str(&t));
	} else {
		sb_add(out, t.buf, t.len);
	}
	rc = 0;
out:
	free(orig);
	free(line);
	free(pfx);
	sb_release(&msg);
	sb_release(&t);
	return rc;
}

int commit_draft(struct repo *r, const struct commit_opts *o)
{
	struct strbuf d = STRBUF_INIT, m = STRBUF_INIT;
	struct commit_opts c = *o;
	int rc = -1;

	if (draft_read(r, &d))
		goto out;
	message_strip(sb_str(&d), commit_comment_char(r), &m);
	if (!m.len) {
		repo_err(r, "the commit message is empty; {edit} in the "
			    "message pane writes it");
		goto out;
	}
	c.message = m.buf;
	c.file = NULL;
	rc = carve_commit(r, &c);
	if (!rc)
		draft_remove(r);
out:
	sb_release(&d);
	sb_release(&m);
	return rc;
}

int carve_commit(struct repo *r, const struct commit_opts *o)
{
	struct strbuf msg = STRBUF_INIT, text = STRBUF_INIT,
		      given = STRBUF_INIT, sob = STRBUF_INIT;
	char *cid = NULL, *path = NULL, *line = NULL;
	char *orig = orig_commit(r);
	bool first, last, keep;
	enum keep_policy pol;
	char *itree = NULL;
	char *otree = NULL;
	int n, rc = -1;

	if (!orig)
		return plain_commit(r, o);
	n = commits_made(r);
	if (o->amend) {
		if (!n) {
			free(orig);
			return repo_err(r, "nothing committed yet in this "
					   "split; amending would change the "
					   "commit below it");
		}
		free(orig);
		return plain_commit(r, o);
	}
	if (!have_staged(r)) {
		free(orig);
		return repo_err(r, "nothing staged to commit");
	}
	state_read(r, "msg", &msg);
	itree = index_tree(r);
	otree = tree_of(r, orig);
	first = n == 0;
	last = itree && otree && !strcmp(itree, otree);
	pol = keep_policy(r);
	keep = (pol == KEEP_FIRST && first) || (pol == KEEP_LAST && last);
	cid = change_id_of(sb_str(&msg));
	if (o->signoff && !(line = signoff_line(r)))
		goto out;

	if (o->message || o->file) {
		if (o->message) {
			sb_adds(&given, o->message);
		} else if (read_file(o->file, &given)) {
			repo_err(r, "cannot read %s", o->file);
			goto out;
		}
		if (keep && cid) {
			if (add_change_id(r, sb_str(&given), cid, &text))
				goto out;
		} else {
			sb_add(&text, sb_str(&given), given.len);
		}
		/* every part is signed off as the original was */
		if (add_signoffs(r, sb_str(&msg), &text))
			goto out;
		if (line) {
			if (add_signoff(r, sb_str(&text), &sob))
				goto out;
			sb_reset(&text);
			sb_add(&text, sb_str(&sob), sob.len);
		}
		if (state_write(r, "COMMIT_MSG", sb_str(&text), text.len))
			goto out;
		path = carve_path(r, "COMMIT_MSG");
		rc = run_commit(r, o, path, MSG_FILE);
	} else {
		split_template(sb_str(&msg), first, keep,
			       commit_comment_char(r), first ? NULL : line,
			       &text);
		if (first && line) {
			if (add_signoff(r, sb_str(&text), &sob))
				goto out;
			sb_reset(&text);
			sb_add(&text, sb_str(&sob), sob.len);
		}
		if (!first) {
			/* later commits start from the usual prefix */
			char *pfx = suggest_prefix(r);

			if (pfx) {
				struct strbuf t = STRBUF_INIT;

				sb_addf(&t, "%s: %s", pfx, sb_str(&text));
				sb_reset(&text);
				sb_add(&text, t.buf, t.len);
				sb_release(&t);
				free(pfx);
			}
		}
		if (state_write(r, "COMMIT_TEMPLATE", sb_str(&text), text.len))
			goto out;
		path = carve_path(r, "COMMIT_TEMPLATE");
		rc = run_commit(r, o, path,
				first ? MSG_EDIT_FILE : MSG_TEMPLATE);
	}
out:
	free(orig);
	free(itree);
	free(otree);
	free(cid);
	free(path);
	sb_release(&msg);
	sb_release(&text);
	sb_release(&given);
	sb_release(&sob);
	free(line);
	return rc;
}

/* ---- status ----------------------------------------------------------- */

/*
 * Lines added and deleted between HEAD and orig. With r->split_big it
 * counts files instead, *add the count and *del -1.
 */
static void remaining_diff(struct repo *r, const char *orig, int *add, int *del)
{
	const char *from = have_head(r) ? "HEAD" : repo_empty_tree(r);
	struct strbuf out = STRBUF_INIT;
	const char *pos, *end, *l;
	size_t len;

	*add = *del = 0;
	if (r->split_big) {
		*del = -1;
		if (!git_out(r, &out, "diff-tree", "-r", "--name-only",
			     "--no-renames", from, orig, NULL))
			*add = (int)count_lines(out.buf, out.len);
		sb_release(&out);
		return;
	}
	if (git_out(r, &out, "diff-tree", "-r", "--numstat", "--no-renames",
		    from, orig, NULL)) {
		sb_release(&out);
		return;
	}
	pos = sb_str(&out);
	end = sb_str(&out) + out.len;
	while ((l = next_line(&pos, end, &len))) {
		const char *p = l;
		int a = 0, d = 0;

		if (*p == '-')
			continue; /* binary */
		/* "<added>\t<deleted>\t<path>" */
		if (parse_int(p, &p, &a) || p[0] != '\t' ||
		    parse_int(p + 1, NULL, &d))
			continue;
		*add += a;
		*del += d;
	}
	sb_release(&out);
}

/*
 * The key the counts of a split hold for: HEAD, the original, its target,
 * the base and whether files are counted instead of lines.
 */
static void counts_key(struct repo *r, const char *head, const char *orig,
		       const char *ttree, struct strbuf *key)
{
	struct strbuf base = STRBUF_INIT;

	state_read(r, "base", &base);
	sb_rtrim(&base);
	sb_addf(key, "%s %s %s %s %d", head ? head : "-", orig,
		ttree ? ttree : "-", sb_str(&base), r->split_big);
	sb_release(&base);
}

int split_state_load(struct repo *r, struct split_state *st)
{
	return split_state_load_clean(r, st, -1);
}

int split_state_load_clean(struct repo *r, struct split_state *st, int clean)
{
	struct strbuf msg = STRBUF_INIT, mode = STRBUF_INIT;
	struct strbuf base = STRBUF_INIT, key = STRBUF_INIT;
	char *htree, *otree, *ttree, *head;

	memset(st, 0, sizeof(*st));
	carve_refs_migrate(r);
	st->orig = orig_commit(r);
	if (!st->orig)
		return 0;
	st->active = true;
	state_read(r, "mode", &mode);
	st->older = starts_with(sb_str(&mode), "older");
	sb_reset(&mode);
	st->in_rebase = !state_read(r, "rebase", &mode);
	state_read(r, "msg", &msg);
	st->subject = xstrndup(sb_str(&msg), strcspn(sb_str(&msg), "\n"));
	ttree = target_tree(r, st->orig);
	head = repo_rev(r, "HEAD");
	counts_key(r, head, st->orig, ttree, &key);
	if (r->split_key && !strcmp(r->split_key, key.buf)) {
		st->commits_made = r->split_made;
		st->added = r->split_added;
		st->removed = r->split_removed;
	} else {
		st->commits_made = commits_made(r);
		remaining_diff(r, ttree ? ttree : st->orig, &st->added,
			       &st->removed);
		free(r->split_key);
		r->split_key = sb_detach(&key);
		r->split_made = st->commits_made;
		r->split_added = st->added;
		r->split_removed = st->removed;
	}
	htree = head ? tree_of(r, "HEAD") : NULL;
	otree = tree_of(r, st->orig);
	st->done =
		htree && otree &&
		(!strcmp(htree, otree) || (ttree && !strcmp(htree, ttree))) &&
		(clean >= 0 ? clean : tree_is_clean(r, NULL));
	free(htree);
	free(otree);
	free(ttree);
	free(head);
	sb_release(&msg);
	sb_release(&mode);
	sb_release(&base);
	sb_release(&key);
	return 0;
}

void split_state_release(struct split_state *st)
{
	free(st->orig);
	free(st->subject);
	memset(st, 0, sizeof(*st));
}

void split_status_line(const struct split_state *st, struct strbuf *out)
{
	sb_addf(out, "SPLIT %.7s \"%s\" \xe2\x80\x94 %d commit%s made, ",
		st->orig, st->subject, st->commits_made,
		st->commits_made == 1 ? "" : "s");
	if (st->done)
		sb_adds(out, "nothing left");
	else
		sb_addf(out, "remaining diff +%d/-%d", st->added, st->removed);
	if (st->older)
		sb_adds(out, " (older commit; finishing replays the ones "
			     "above)");
	if (st->in_rebase)
		sb_adds(out, " (in your rebase: it goes on with git rebase "
			     "--continue once the split is finished)");
}

/* ---- start / finish / abort ------------------------------------------- */

static int check_no_operation(struct repo *r, bool head)
{
	/* HEAD where a rebase of yours stopped is split as anywhere else */
	if (rebase_in_progress(r) && !(head && foreign_rebase(r)))
		return repo_err(r, foreign_rebase(r) ? YOUR_REBASE_MSG :
						       OP_BUSY_MSG);
	if (git_path_exists(r, "MERGE_HEAD") ||
	    git_path_exists(r, "CHERRY_PICK_HEAD") ||
	    git_path_exists(r, "REVERT_HEAD"))
		return repo_err(r, "a merge, cherry-pick or revert is in "
				   "progress; finish or abort it first");
	return 0;
}

static void warn_if_pushed(struct repo *r, const char *commit)
{
	char *u = repo_rev(r, "@{upstream}");

	if (u && git_status_of(r, "merge-base", "--is-ancestor", commit, u,
			       NULL) == 0)
		repo_warn(r,
			  "warning: %.7s is already in the upstream branch "
			  "(pushed?); splitting rewrites published history",
			  commit);
	free(u);
}

/* Split HEAD (the older-commit case stops a rebase there first). */
/*
 * Keep a copy of the index as the split starts. It is the commit's tree
 * with the stat data of the files in the work tree, and lets an abort
 * check the work tree without hashing every file (split_abort). Without
 * the copy the abort takes the slow way.
 */
static void save_abort_index(struct repo *r)
{
	struct strbuf data = STRBUF_INIT;
	char *idx = repo_index_path(r);

	if (idx && !read_file(idx, &data) &&
	    state_write(r, "abort-index", data.buf, data.len))
		sb_reset(&r->err);
	free(idx);
	sb_release(&data);
}

static int split_here(struct repo *r, const char *mode)
{
	struct strbuf raw = STRBUF_INIT, par = STRBUF_INIT;
	char *head = NULL, *parents = NULL, *msg, *sym;
	int nparents = 0, rc = -1;
	const char *p, *q;
	bool known;

	head = repo_rev(r, "HEAD^{commit}");
	if (!head) {
		repo_err(r, "there is no commit to split");
		goto out;
	}
	/* the commit's parents and message from the cat-file worker */
	if (repo_cat(r, head, &raw, NULL))
		goto out;
	sb_adds(&par, head);
	for (p = sb_str(&raw);
	     !strncmp(p, "tree ", 5) || !strncmp(p, "parent ", 7); p = q + 1) {
		if (!(q = strchr(p, '\n')))
			break;
		if (!strncmp(p, "parent ", 7)) {
			sb_addf(&par, " %.*s", (int)(q - p - 7), p + 7);
			nparents++;
		}
	}
	parents = sb_detach(&par);
	if (nparents > 1) {
		repo_err(r, "%.7s is a merge commit; merges cannot be split",
			 head);
		goto out;
	}
	/* an older commit was checked before the rebase stopped here, clean */
	if (strcmp(mode, "older") && !tree_is_clean(r, NULL)) {
		refuse_dirty(r);
		goto out;
	}
	if (!nparents) {
		sym = repo_head_branch(r, &known);
		if (!known)
			sym = git_line(r, "symbolic-ref", "-q", "HEAD", NULL);
		if (!sym) {
			repo_err(r, "cannot split a root commit on a "
				    "detached HEAD");
			goto out;
		}
		free(sym);
	}
	msg = strstr(sb_str(&raw), "\n\n");
	msg = msg ? msg + 2 : "";
	if (state_write(r, "msg", msg, strlen(msg)) ||
	    state_write(r, "base", nparents ? parents + strlen(head) + 1 : "",
			nparents ? strlen(parents) - strlen(head) - 1 : 0) ||
	    state_write(r, "mode", mode, strlen(mode)))
		goto fail;
	if (!strcmp(mode, "head") && rebase_in_progress(r)) {
		/* where your rebase stopped, abort checks it is still there */
		char step[32];
		int n, total;

		rebase_step(r, &n, &total);
		snprintf(step, sizeof(step), "%d", n);
		if (state_write(r, "rebase", step, strlen(step)))
			goto fail;
	}
	if (git_quiet(r, "update-ref", "-m", "carve: split", SPLIT_REF, head,
		      NULL))
		goto fail;
	warn_if_pushed(r, head);
	{
		/* review tags were for the patch as it was, say they go */
		struct strbuf rest = STRBUF_INIT, dropped = STRBUF_INIT;

		strip_review_tags(msg, &rest, &dropped);
		if (dropped.len)
			repo_warn(r,
				  "note: %s not carried to the new commits: "
				  "given for the patch before the split",
				  dropped.buf);
		sb_release(&rest);
		sb_release(&dropped);
	}

	if (!strcmp(mode, "head"))
		save_abort_index(r);
	if (nparents) {
		/*
		 * reset --mixed HEAD~1, without its per-entry index update
		 * (5 s on a commit of 15,000 kernel files, 0.2 s this way):
		 * HEAD moves, then the index takes HEAD's entries back
		 */
		if (git_quiet(r, "reset", "-q", "--soft", "HEAD~1", NULL))
			goto fail_ref;
		if (git_quiet(r, "restore", "-q", "--staged", "--", ":/",
			      NULL)) {
			/* HEAD back where it was, the index never changed */
			git_status_of(r, "reset", "-q", "--soft", head, NULL);
			goto fail_ref;
		}
		repo_index_is(r, "HEAD");
	} else {
		/*
		 * a root commit goes back to an unborn branch and an empty
		 * index
		 */
		if (git_quiet(r, "update-ref", "-d", "HEAD", NULL) ||
		    git_quiet(r, "read-tree", "--empty", NULL))
			goto fail_ref;
	}
	rc = 0;
	goto out;
fail_ref:
	git_status_of(r, "update-ref", "-d", SPLIT_REF, NULL);
fail:
	state_remove(r);
out:
	free(head);
	free(parents);
	sb_release(&raw);
	return rc;
}

/*
 * Right after the start, no commit is made and all files of the commit
 * are left. For a split of a huge commit, which counts files,
 * that is what split_state_load would ask git for next.
 */
static void seed_counts(struct repo *r, size_t files)
{
	char *head = repo_rev(r, "HEAD"), *orig = orig_commit(r);
	char *ttree = orig ? target_tree(r, orig) : NULL;
	struct strbuf key = STRBUF_INIT;
	bool big = r->split_big;

	/* lines taken out of the commit come later, not at the start */
	if (!orig || ttree)
		goto out;
	r->split_big = true;
	counts_key(r, head, orig, NULL, &key);
	r->split_big = big;
	free(r->split_key);
	r->split_key = sb_detach(&key);
	r->split_made = 0;
	r->split_added = (int)files;
	r->split_removed = -1;
out:
	free(head);
	free(orig);
	free(ttree);
	sb_release(&key);
}

int split_start_status(struct repo *r, struct file_status **out, size_t *n)
{
	size_t k = 0, alloc = 0, all = 0;
	struct strbuf o = STRBUF_INIT;
	struct file_status *v = NULL;
	const char *p, *end;

	*out = NULL;
	*n = 0;
	if (git_out(r, &o, GIT_PLAIN, "diff-tree", "-r", "--no-renames",
		    "--name-status", "-z", "HEAD", SPLIT_REF, NULL)) {
		sb_release(&o);
		return -1;
	}
	/* "<status>\0<path>\0" for each file */
	p = sb_str(&o);
	end = p + o.len;
	while (p < end) {
		const char *st = p, *path;

		p += strlen(p) + 1;
		if (p >= end)
			break;
		path = p;
		p += strlen(p) + 1;
		all++;
		if (*st != 'M' && *st != 'T' && *st != 'D')
			continue;
		ALLOC_GROW(v, k + 1, alloc);
		memset(&v[k], 0, sizeof(v[k]));
		v[k].path = xstrdup(path);
		v[k].x = ' ';
		v[k].y = *st;
		k++;
	}
	sb_release(&o);
	if (k > 1)
		qsort(v, k, sizeof(*v), status_cmp);
	*out = v;
	*n = k;
	seed_counts(r, all);
	return 0;
}

/* The mode of a split of an older commit until the rebase stopped. */
#define START_MODE "older-start"

int split_start(struct repo *r, const char *commit)
{
	struct strbuf old = STRBUF_INIT, range = STRBUF_INIT;
	char *orig = orig_commit(r);
	char *full = NULL;
	char *head = NULL;
	int rc = -1;

	if (orig) {
		repo_err(r, "a split of %.7s is already in progress", orig);
		free(orig);
		return -1;
	}
	head = repo_rev(r, "HEAD");
	if (commit && head) {
		/* HEAD by another name needs no rebase */
		sb_addf(&range, "%s^{commit}", commit);
		full = repo_rev(r, range.buf);
		sb_reset(&range);
		if (full && !strcmp(full, head))
			commit = NULL;
		free(full);
		full = NULL;
	}
	if (check_no_operation(r, !commit))
		goto out;
	if (!commit) {
		rc = split_here(r, "head");
		goto out;
	}

	if (check_series_commit(r, commit, &full))
		goto out;
	if (!head) {
		repo_err(r, "no commits yet");
		goto out;
	}
	if (!tree_is_clean(r, NULL)) {
		refuse_dirty(r);
		goto out;
	}
	/*
	 * The split is on record before the rebase starts. Killed from here
	 * until split_here, git carve finds it again, and abort gives it up
	 * with git rebase --abort alone.
	 */
	if (state_write(r, "mode", START_MODE, strlen(START_MODE)) ||
	    git_quiet(r, "update-ref", "-m", "carve: split", SPLIT_REF, full,
		      NULL)) {
		state_remove(r);
		goto out;
	}
	/* stop a rebase at the commit, then split it like HEAD */
	sb_addf(&range, "%s..%s", full, head);
	if (list_commits(r, range.buf, &old) || rebase_edit_at(r, full, NULL)) {
		git_status_of(r, "update-ref", "-d", SPLIT_REF, NULL);
		state_remove(r);
		goto out;
	}
	if (split_here(r, "older")) {
		rebase_abort(r);
		goto out;
	}
	if (state_write(r, "tip", head, strlen(head)) ||
	    state_write(r, "descendants", old.buf, old.len)) {
		split_abort(r, true);
		goto out;
	}
	rc = 0;
out:
	free(full);
	free(head);
	sb_release(&old);
	sb_release(&range);
	return rc;
}

int split_finish(struct repo *r, struct strbuf *out)
{
	char *orig = orig_commit(r), *htree = NULL, *otree = NULL,
	     *ttree = NULL;
	struct strbuf what = STRBUF_INIT, base = STRBUF_INIT,
		      range = STRBUF_INIT, mode = STRBUF_INIT,
		      old = STRBUF_INIT;
	char *split_head = NULL;
	int rc = -1, add, del;

	if (!orig)
		return repo_err(r, "no split in progress");
	htree = have_head(r) ? tree_of(r, "HEAD") : NULL;
	otree = tree_of(r, orig);
	ttree = target_tree(r, orig);
	if (!htree || !otree ||
	    (strcmp(htree, otree) && (!ttree || strcmp(htree, ttree)))) {
		remaining_diff(r, ttree ? ttree : orig, &add, &del);
		repo_err(r,
			 "split not finished: the commits made so far "
			 "differ from %.7s by +%d/-%d; stage and commit "
			 "the rest",
			 orig, add, del);
		goto out;
	}
	if (!tree_is_clean(r, &what)) {
		repo_err(r, "split not finished: uncommitted changes in %s",
			 what.buf);
		goto out;
	}
	/* with nothing staged the index is HEAD's tree */
	repo_index_is(r, "HEAD");
	if (out) {
		state_read(r, "base", &base);
		sb_rtrim(&base);
		if (base.len)
			sb_addf(&range, "%s..HEAD", base.buf);
		else
			sb_adds(&range, "HEAD");
		git_out(r, out, "rev-list", "--reverse", range.buf, NULL);
	}
	state_read(r, "mode", &mode);
	if (starts_with(sb_str(&mode), "older")) {
		state_read(r, "descendants", &old);
		split_head = repo_rev(r, "HEAD");
	}
	if (git_quiet(r, "update-ref", "-d", SPLIT_REF, NULL))
		goto out;
	state_remove(r);
	rc = 0;
	if (!split_head)
		goto out;

	/*
	 * Replay the commits above. The split's tree equals the original
	 * one, and they must apply cleanly and keep their trees (I7).
	 */
	if (rebase_continue(r)) {
		repo_err(r,
			 "the split is done, but replaying the commits "
			 "above it stopped, which should not happen: %s; "
			 "finish with git rebase --continue or --abort",
			 r->err.len ? r->err.buf : "conflict");
		rc = -1;
		goto out;
	}
	sb_reset(&range);
	sb_addf(&range, "%s..HEAD", split_head);
	if (check_replayed(r, sb_str(&old), range.buf))
		rc = -1;
out:
	free(orig);
	free(htree);
	free(otree);
	free(ttree);
	free(split_head);
	sb_release(&what);
	sb_release(&base);
	sb_release(&range);
	sb_release(&mode);
	sb_release(&old);
	return rc;
}

/* Keep the paths in what (one per line) where index env differs from tree. */
static void keep_changed(struct repo *r, const char *tree, struct strbuf *what)
{
	struct strbuf diff = STRBUF_INIT, keep = STRBUF_INIT,
		      name = STRBUF_INIT;
	const char *pos = sb_str(what);
	const char *end;
	const char *l;
	size_t len;

	end = pos + what->len;

	/* "\n<path>\n" finds whole names */
	sb_addch(&diff, '\n');
	if (git_out(r, &diff, GIT_PLAIN, "diff", "--name-only", "--no-renames",
		    tree, NULL))
		goto out;
	while ((l = next_line(&pos, end, &len))) {
		sb_reset(&name);
		sb_addch(&name, '\n');
		sb_add(&name, l, len);
		sb_addch(&name, '\n');
		if (strstr(diff.buf, name.buf))
			sb_addf(&keep, "%.*s\n", (int)len, l);
	}
	sb_reset(what);
	sb_add(what, keep.buf ? keep.buf : "", keep.len);
out:
	sb_release(&diff);
	sb_release(&keep);
	sb_release(&name);
}

/*
 * The paths whose work-tree file differs from orig's, into what, from git
 * diff orig, leaving out paths orig does not have. A file orig has that is
 * not in the index (one orig added, untracked again during the split)
 * shows as deleted. Its file, if there, is compared with orig's blob, all
 * of them in one git hash-object. When it is gone nothing is lost.
 */
static int gone_or_same(struct repo *r, const char *orig, struct strbuf *what)
{
	struct strbuf out = STRBUF_INIT, in = STRBUF_INIT, ids = STRBUF_INIT;
	size_t len, n = 0, alloc = 0, i;
	const char *pos, *end, *l, *ip;
	struct run_opts ro = { 0 };
	struct argv a = ARGV_INIT;
	char **check = NULL;
	int rc = -1;

	if (git_out(r, &out, GIT_PLAIN, "diff", "--name-status", "--no-renames",
		    "--diff-filter=a", orig, NULL))
		goto out;
	pos = sb_str(&out);
	end = pos + out.len;
	while ((l = next_line(&pos, end, &len))) {
		char *p;

		if (len < 3 || l[1] != '\t')
			continue;
		p = xstrndup(l + 2, len - 2);
		if (l[0] != 'D') {
			sb_addf(what, "%s\n", p);
			free(p);
		} else if (path_in_worktree(p)) {
			ALLOC_GROW(check, n + 1, alloc);
			check[n++] = p;
			sb_addf(&in, "%s\n", p);
		} else {
			free(p);
		}
	}
	if (n) {
		argv_pushl(&a, "hash-object", "--stdin-paths", NULL);
		ro.in = in.buf;
		ro.inlen = in.len;
		if (run_git(&a, &ro, &ids, NULL))
			goto out;
		ip = sb_str(&ids);
		for (i = 0; i < n; i++) {
			struct strbuf spec = STRBUF_INIT;
			size_t il = strcspn(ip, "\n");
			char *blob;

			sb_addf(&spec, "%s:%s", orig, check[i]);
			blob = repo_rev(r, spec.buf);
			if (!blob || strlen(blob) != il ||
			    strncmp(blob, ip, il))
				sb_addf(what, "%s\n", check[i]);
			free(blob);
			sb_release(&spec);
			ip += il + (ip[il] ? 1 : 0);
		}
	}
	sb_reset(&r->err);
	rc = 0;
out:
	for (i = 0; i < n; i++)
		free(check[i]);
	free(check);
	argv_clear(&a);
	sb_release(&out);
	sb_release(&in);
	sb_release(&ids);
	return rc;
}

/*
 * Would restoring orig lose anything? The work tree is compared with orig
 * through git diff orig and the real index's stat data, leaving out paths
 * orig does not have, and then the index with orig. A temporary index
 * seeded from orig would hash every file of the tree, minutes on a kernel.
 */
static int unrelated_changes(struct repo *r, const char *orig,
			     struct strbuf *what)
{
	struct strbuf out = STRBUF_INIT;
	char *otree = tree_of(r, orig);
	char *ttree = NULL;
	int rc = -1;

	if (gone_or_same(r, orig, what))
		goto out;
	/* lines taken out during the split come back, nothing is lost */
	ttree = target_tree(r, orig);
	if (what->len && ttree && otree && strcmp(ttree, otree))
		keep_changed(r, ttree, what);
	/*
	 * Index paths the original does not have. reset --hard deletes
	 * them from the work tree, which loses nothing unless they exist
	 * there. A file the original deleted is still in the index after
	 * the reset to its parent, but not on disk.
	 */
	git_out(r, &out, GIT_PLAIN, "diff-index", "--cached", "--name-only",
		"--diff-filter=A", orig, NULL);
	{
		const char *pos = sb_str(&out), *end = pos + out.len, *l;
		size_t len;

		while ((l = next_line(&pos, end, &len))) {
			char *p = xstrndup(l, len);

			if (path_in_worktree(p))
				sb_addf(what, "%s\n", p);
			free(p);
		}
	}
	sb_rtrim(what);
	rc = what->len ? 1 : 0;
out:
	free(otree);
	free(ttree);
	sb_release(&out);
	return rc;
}

/*
 * Abort a split of HEAD with the index saved as it started. When git,
 * reading that index, finds the work tree equal to orig and the index to
 * be orig's tree, HEAD goes back and the saved index replaces the real
 * one. Nothing in the work tree is written, where reset --hard would
 * write every file again. Returns 1 when done, 0 when the slow way must
 * decide, and -1 on errors.
 */
/* Whether one of the paths, one per line, is in the work tree. */
static bool any_on_disk(const struct strbuf *paths)
{
	const char *pos = sb_str(paths), *end = pos + paths->len, *l;
	bool found = false;
	size_t len;

	while (!found && (l = next_line(&pos, end, &len))) {
		char *p = xstrndup(l, len);

		found = path_in_worktree(p);
		free(p);
	}
	return found;
}

static int abort_fast(struct repo *r, const char *orig)
{
	struct strbuf envs = STRBUF_INIT, out = STRBUF_INIT, lock = STRBUF_INIT;
	char *copy = carve_path(r, "abort-index"), *idx = repo_index_path(r);
	const char *env[2] = { NULL, NULL };
	int rc = 0;

	if (!idx || access(copy, F_OK))
		goto out;
	sb_addf(&envs, "GIT_INDEX_FILE=%s", copy);
	env[0] = envs.buf;
	/*
	 * A file changed or gone, or one in the index that orig does not
	 * have and that is on disk, is for the slow way. Files the commit
	 * deleted are in the index again but not on disk.
	 */
	if (git_env(r, env, &out, "--no-optional-locks", GIT_PLAIN, "diff",
		    "--name-only", "--no-renames", orig, NULL) ||
	    out.len ||
	    git_env(r, env, NULL, "diff-index", "--cached", "--quiet", orig,
		    NULL) ||
	    git_out(r, &out, GIT_PLAIN, "diff-index", "--cached", "--name-only",
		    "--diff-filter=A", orig, NULL) ||
	    any_on_disk(&out)) {
		sb_reset(&r->err);
		goto out;
	}
	rc = -1;
	if (have_head(r) ? git_quiet(r, "reset", "-q", "--soft", orig, NULL) :
			   git_quiet(r, "update-ref", "HEAD", orig, NULL))
		goto out;
	/*
	 * git's lock on the index, made as a link to the copy: it is there
	 * whole or not at all, and the rename right after puts it in place.
	 * With HEAD back at orig, an abort killed before that runs again.
	 */
	sb_addf(&lock, "%s.lock", idx);
	if (link(copy, lock.buf)) {
		repo_err(r, "cannot lock the index: %s", strerror(errno));
		goto out;
	}
	if (rename(lock.buf, idx)) {
		repo_err(r, "cannot put the index back: %s", strerror(errno));
		unlink(lock.buf);
		goto out;
	}
	unlink(copy);
	repo_index_is(r, orig);
	rc = 1;
out:
	free(copy);
	free(idx);
	sb_release(&envs);
	sb_release(&out);
	sb_release(&lock);
	return rc;
}

int split_abort(struct repo *r, bool force)
{
	struct strbuf what = STRBUF_INIT, mode = STRBUF_INIT,
		      step = STRBUF_INIT;
	char *orig = orig_commit(r);
	int rc = -1, u;

	if (!orig)
		return repo_err(r, "no split in progress");
	state_read(r, "mode", &mode);
	if (!strcmp(sb_str(&mode), START_MODE)) {
		/* killed as it started, before anything was split */
		if (rebase_in_progress(r) && rebase_abort(r))
			goto out;
		goto done;
	}
	sb_reset(&mode);
	if (!state_read(r, "rebase", &step)) {
		/*
		 * Your rebase moved on (continued or aborted): resetting to
		 * the commit would throw its steps away. --force forgets
		 * the split and leaves everything as it is.
		 */
		int n = 0, total;

		rebase_step(r, &n, &total);
		if (!rebase_in_progress(r) ||
		    n != number_or(sb_str(&step), -1)) {
			if (!force) {
				repo_err(r, "the rebase the split started in "
					    "has moved on (continued or "
					    "aborted?); --force forgets the "
					    "split and changes nothing else");
				goto out;
			}
			if (git_quiet(r, "update-ref", "-d", SPLIT_REF, NULL))
				goto out;
			state_remove(r);
			rc = 1;
			goto out;
		}
		sb_reset(&step);
	}
	state_read(r, "mode", &mode);
	if (!starts_with(sb_str(&mode), "older")) {
		int f = abort_fast(r, orig);

		if (f < 0)
			goto out;
		if (f)
			goto done;
	}
	u = unrelated_changes(r, orig, &what);
	if (u < 0)
		goto out;
	if (u && !force) {
		char *nl;

		while ((nl = strchr(what.buf, '\n')))
			*nl = ' ';
		repo_err(r,
			 "the work tree has changes that are not in %.7s "
			 "(%s); use --force to discard them",
			 orig, what.buf);
		goto out;
	}
	if (starts_with(sb_str(&mode), "older")) {
		/*
		 * Back to the original commit first. Files it added are
		 * untracked now, and "rebase --abort" would refuse to
		 * overwrite them. Then git restores the branch as it was.
		 */
		if (git_quiet(r, "reset", "-q", "--hard", orig, NULL) ||
		    rebase_abort(r))
			goto out;
	} else {
		if (!have_head(r) &&
		    git_quiet(r, "update-ref", "HEAD", orig, NULL))
			goto out;
		if (git_quiet(r, "reset", "-q", "--hard", orig, NULL))
			goto out;
		repo_index_is(r, orig);
	}
done:
	if (git_quiet(r, "update-ref", "-d", SPLIT_REF, NULL))
		goto out;
	state_remove(r);
	rc = 0;
out:
	free(orig);
	sb_release(&what);
	sb_release(&mode);
	sb_release(&step);
	return rc;
}

int split_amend_base(struct repo *r)
{
	struct strbuf mode = STRBUF_INIT;
	char *orig = orig_commit(r);
	char *head = NULL;
	int rc = -1;

	if (!orig)
		return repo_err(r, "no split in progress");
	state_read(r, "mode", &mode);
	if (starts_with(sb_str(&mode), "older")) {
		repo_err(r, "only while splitting HEAD; finish this split "
			    "first");
		goto out;
	}
	if (commits_made(r)) {
		repo_err(r, "commits were made in this split already: the "
			    "commit below it can only take changes before "
			    "the first one");
		goto out;
	}
	if (!have_staged(r)) {
		repo_err(r, "nothing staged to move into the commit below");
		goto out;
	}
	if (git_env(r, git_quiet_editor, NULL, "commit", "-q", "--amend",
		    "--no-edit", NULL))
		goto out;
	/* the split now starts from the amended commit */
	head = repo_rev(r, "HEAD");
	if (!head || state_write(r, "base", head, strlen(head)))
		goto out;
	rc = 0;
out:
	free(orig);
	free(head);
	sb_release(&mode);
	return rc;
}

/* The tree the split adds up to, the original one minus what was taken out. */
static char *target_tree(struct repo *r, const char *orig)
{
	struct strbuf t = STRBUF_INIT;

	state_read(r, "target", &t);
	sb_rtrim(&t);
	if (t.len)
		return sb_detach(&t);
	sb_release(&t);
	return tree_of(r, orig);
}

/* "<mode> <blob>" of path in tree, or NULL when it is not there. */
static char *tree_entry(struct repo *r, const char *tree, const char *path)
{
	struct strbuf out = STRBUF_INIT, e = STRBUF_INIT;
	char *res = NULL, *tab, *blob;

	if (git_out(r, &out, "ls-tree", "--full-tree", tree, "--", path,
		    NULL) ||
	    !out.len)
		goto out;
	/* "<mode> blob <id>\t<path>" */
	tab = strchr(out.buf, '\t');
	blob = strstr(out.buf, " blob ");
	if (!tab || !blob || blob > tab)
		goto out;
	*tab = '\0';
	sb_addf(&e, "%.*s %s", (int)(blob - out.buf), out.buf, blob + 6);
	res = sb_detach(&e);
out:
	sb_release(&out);
	sb_release(&e);
	return res;
}

/* The work tree's path as a blob in the object database, NULL if absent. */
static char *worktree_blob(struct repo *r, const char *path)
{
	struct strbuf full = STRBUF_INIT;
	char *res = NULL;
	struct stat s;

	sb_addf(&full, "%s/%s", r->top, path);
	if (!lstat(full.buf, &s))
		res = git_line(r, "hash-object", "-w", "--", path, NULL);
	sb_release(&full);
	return res;
}

int split_take_begin(struct repo *r, const char *path, struct split_take *t)
{
	char *orig = orig_commit(r), *tree = NULL, *entry = NULL;
	struct strbuf mode = STRBUF_INIT;
	bool edited;
	int rc = -1;

	memset(t, 0, sizeof(*t));
	if (!orig)
		return 0;
	state_read(r, "mode", &mode);
	if (starts_with(sb_str(&mode), "older")) {
		repo_err(r,
			 "lines can be taken out of a split commit only while "
			 "splitting HEAD: the commits above %.7s must keep "
			 "their trees",
			 orig);
		goto out;
	}
	tree = target_tree(r, orig);
	if (!tree)
		goto out;
	entry = tree_entry(r, tree, path);
	t->before = worktree_blob(r, path);
	/* in a file edited during the split the change is not the commit's */
	if (entry && t->before)
		edited = strcmp(strchr(entry, ' ') + 1, t->before) != 0;
	else
		edited = entry || t->before;
	if (edited) {
		repo_err(r,
			 "%s was edited during the split: commit or restore "
			 "those edits first, then take lines out of %.7s",
			 path, orig);
		goto out;
	}
	t->path = xstrdup(path);
	t->mode = entry ? xstrndup(entry, strcspn(entry, " ")) : NULL;
	t->tree = tree;
	tree = NULL;
	rc = 0;
out:
	if (rc)
		split_take_release(t);
	free(orig);
	free(tree);
	free(entry);
	sb_release(&mode);
	return rc;
}

int split_take_end(struct repo *r, struct split_take *t)
{
	struct strbuf envs = STRBUF_INIT, out = STRBUF_INIT, ci = STRBUF_INIT;
	const char *env[2] = { NULL, NULL };
	char *idx = NULL, *after = NULL;
	int rc = -1;

	if (!t->path)
		return 0;
	after = worktree_blob(r, t->path);
	idx = carve_path(r, "target-index");
	sb_addf(&envs, "GIT_INDEX_FILE=%s", idx);
	env[0] = envs.buf;
	unlink(idx);
	if (git_env(r, env, NULL, "read-tree", t->tree, NULL))
		goto out;
	if (after) {
		sb_addf(&ci, "%s,%s,%s", t->mode ? t->mode : "100644", after,
			t->path);
		if (git_env(r, env, NULL, "update-index", "--add",
			    "--cacheinfo", ci.buf, NULL))
			goto out;
	} else if (git_env(r, env, NULL, "update-index", "--force-remove", "--",
			   t->path, NULL)) {
		goto out;
	}
	if (git_env(r, env, &out, "write-tree", NULL))
		goto out;
	if (state_write(r, "target", out.buf, out.len))
		goto out;
	rc = 0;
out:
	if (idx)
		unlink(idx);
	free(idx);
	free(after);
	sb_release(&envs);
	sb_release(&out);
	sb_release(&ci);
	split_take_release(t);
	return rc;
}

void split_take_release(struct split_take *t)
{
	free(t->path);
	free(t->before);
	free(t->mode);
	free(t->tree);
	memset(t, 0, sizeof(*t));
}
