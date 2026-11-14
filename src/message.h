/* SPDX-License-Identifier: MIT */
#ifndef CARVE_MESSAGE_H
#define CARVE_MESSAGE_H

#include "git.h"

/* git's scissors line (after the comment character and a space) */
#define SCISSORS_LINE "------------------------ >8 ------------------------"

/* core.commentChar, '#' when it is "auto" or unset */
char commit_comment_char(struct repo *r);

/* The changed lines of patch (the first 20), indented, into note. */
void commit_note_patch(struct strbuf *note, const char *patch);

/* Message helpers (exposed for unit tests). */
char *change_id_of(const char *msg);

void strip_change_id(const char *msg, struct strbuf *out);

/*
 * msg without its Reviewed-by, Acked-by and Tested-by lines. Their names
 * are added to dropped if it is not NULL, each once and separated by ", ".
 */
void strip_review_tags(const char *msg, struct strbuf *out,
		       struct strbuf *dropped);

/* The Signed-off-by lines of msg, each with its newline. */
void signoffs_of(const char *msg, struct strbuf *out);

/*
 * The editor template of a commit during a split. Every part keeps the
 * original's Signed-off-by lines, the one with keep_change_id keeps the
 * Change-Id, and none keeps the review tags. signoff, if not NULL, is a
 * "Signed-off-by" line for the templates of later commits. The first
 * commit gets it when it is committed.
 */
void split_template(const char *msg, bool first, bool keep_change_id,
		    char comment, const char *signoff, struct strbuf *out);

/*
 * carve.signoff, which signs off commits by default in the CLI and at
 * first in the TUI.
 */
bool config_signoff(struct repo *r);

/*
 * The subsystem prefix ("net: phy") the history of the staged files uses
 * most, or NULL when there is none or carve.suggestPrefix is false.
 */
char *suggest_prefix(struct repo *r);

/*
 * The commit message being written, kept while you stage (the TUI's
 * message pane): $GIT_DIR/CARVE_MESSAGE, per worktree like MERGE_MSG.
 * draft_read gives "" when there is none.
 */
#define DRAFT_FILE "CARVE_MESSAGE"
int draft_read(struct repo *r, struct strbuf *out);
int draft_write(struct repo *r, const char *text, size_t len);
void draft_remove(struct repo *r);
/*
 * msg as git commit --cleanup=strip leaves it, without comment lines
 * (comment character c), without anything from a scissors line on, and
 * without trailing blank lines.
 */
void message_strip(const char *msg, char c, struct strbuf *out);

/* The scissors line and what it means, as comment lines c. */
void add_scissors(struct strbuf *out, char c);
/* The committer's "Signed-off-by: Name <email>" line, or NULL. */
char *signoff_line(struct repo *r);
/*
 * msg with the committer's Signed-off-by added, unless that exact trailer
 * is already there. git commit -s only looks at the last one.
 */
int add_signoff(struct repo *r, const char *msg, struct strbuf *out);
/*
 * msg added to sb with a newline at its end. The interpret-trailers of
 * older git (2.43) puts a trailer right after a last line without one,
 * where it becomes part of a one-line subject.
 */
void ended_line(struct strbuf *sb, const char *msg, size_t len);
/*
 * text (a message) with the Signed-off-by lines of orig added, each
 * unless it is there already.
 */
int add_signoffs(struct repo *r, const char *orig, struct strbuf *text);

#endif
