// SPDX-License-Identifier: MIT
/* Search and replace on work-tree lines (subst.h). */
#include "subst.h"

#include <stdlib.h>
#include <string.h>

#include "diff.h"

int subst_parse(const char *expr, struct subst *s, struct strbuf *err)
{
	struct strbuf pat = STRBUF_INIT, rep = STRBUF_INIT;
	int flags = REG_EXTENDED, rc, part = 0;
	const char *p = expr;
	char delim;

	memset(s, 0, sizeof(*s));
	if (*p != 's' || !p[1] || p[1] == '\\' || p[1] == '\n' ||
	    (p[1] >= 'a' && p[1] <= 'z') || (p[1] >= '0' && p[1] <= '9')) {
		sb_addf(err, "not s/pattern/replacement/: %s", expr);
		return -1;
	}
	delim = p[1];
	for (p += 2; *p && part < 2; p++) {
		struct strbuf *to = part ? &rep : &pat;

		if (*p == '\\' && p[1] == delim) {
			/* an escaped delimiter is the character itself */
			sb_add(to, &delim, 1);
			p++;
		} else if (*p == '\\' && p[1]) {
			sb_add(to, p, 2);
			p++;
		} else if (*p == delim) {
			part++;
		} else {
			sb_add(to, p, 1);
		}
	}
	for (; *p; p++) {
		if (*p == 'g') {
			s->global = true;
		} else if (*p == 'i') {
			flags |= REG_ICASE;
		} else {
			sb_addf(err,
				"unknown flag '%c' (g: every match, i: "
				"ignore case)",
				*p);
			goto fail;
		}
	}
	if (!pat.len) {
		sb_adds(err, "empty pattern");
		goto fail;
	}
	rc = regcomp(&s->re, pat.buf, flags);
	if (rc) {
		char msg[200];

		regerror(rc, &s->re, msg, sizeof(msg));
		sb_addf(err, "pattern %s: %s", pat.buf, msg);
		goto fail;
	}
	s->compiled = true;
	s->repl = sb_detach(&rep);
	if (!s->repl)
		s->repl = xstrdup("");
	sb_release(&pat);
	return 0;
fail:
	sb_release(&pat);
	sb_release(&rep);
	return -1;
}

void subst_free(struct subst *s)
{
	if (s->compiled)
		regfree(&s->re);
	free(s->repl);
	memset(s, 0, sizeof(*s));
}

/* The replacement for one match, with & and \N from m and the escapes. */
static void expand(const char *repl, const char *subj, const regmatch_t *m,
		   struct strbuf *out)
{
	const char *p;

	for (p = repl; *p; p++) {
		if (*p == '&') {
			sb_add(out, subj + m[0].rm_so,
			       (size_t)(m[0].rm_eo - m[0].rm_so));
		} else if (*p == '\\' && p[1] >= '1' && p[1] <= '9') {
			int g = p[1] - '0';

			if (m[g].rm_so >= 0)
				sb_add(out, subj + m[g].rm_so,
				       (size_t)(m[g].rm_eo - m[g].rm_so));
			p++;
		} else if (*p == '\\' && p[1]) {
			sb_add(out, p + 1, 1);
			p++;
		} else {
			sb_add(out, p, 1);
		}
	}
}

int subst_line(const struct subst *s, const char *line, size_t len,
	       struct strbuf *out)
{
	char *subj = xstrndup(line, len);
	size_t off = 0;
	int n = 0;

	sb_reset(out);
	while (off <= len) {
		regmatch_t m[10];
		size_t i;

		if (regexec(&s->re, subj + off, 10, m, off ? REG_NOTBOL : 0))
			break;
		for (i = 0; i < 10; i++) {
			if (m[i].rm_so < 0)
				continue;
			m[i].rm_so += (regoff_t)off;
			m[i].rm_eo += (regoff_t)off;
		}
		sb_add(out, subj + off, (size_t)m[0].rm_so - off);
		expand(s->repl, subj, m, out);
		n++;
		off = (size_t)m[0].rm_eo;
		if (m[0].rm_eo == m[0].rm_so) {
			/* step over one character after an empty match */
			if (off < len)
				sb_add(out, subj + off, 1);
			off++;
		}
		if (!s->global)
			break;
	}
	if (off < len)
		sb_add(out, subj + off, len - off);
	free(subj);
	return n;
}

int subst_ranges(const char *spec, struct line_range **out, size_t *n,
		 struct strbuf *err)
{
	const char *p = spec;
	size_t alloc = 0;

	*out = NULL;
	*n = 0;
	while (*p) {
		bool old = false;
		char *end;
		long a, b;

		sel_side(&p, &old);
		if (old) {
			sb_addf(err,
				"%s: old-side lines are not in the work "
				"tree; give new-side (n) numbers",
				spec);
			goto fail;
		}
		a = strtol(p, &end, 10);
		if (end == p || a < 1)
			goto bad;
		b = a;
		p = end;
		if (*p == '-') {
			p++;
			b = strtol(p, &end, 10);
			if (end == p || b < a)
				goto bad;
			p = end;
		}
		ALLOC_GROW(*out, *n + 1, alloc);
		(*out)[*n].from = (int)a;
		(*out)[(*n)++].to = (int)b;
		if (*p == ',')
			p++;
		else if (*p)
			goto bad;
	}
	if (!*n)
		goto bad;
	return 0;
bad:
	sb_addf(err, "bad line selection '%s' (e.g. n12, n3-9)", spec);
fail:
	free(*out);
	*out = NULL;
	*n = 0;
	return -1;
}

static bool in_ranges(const struct line_range *rg, size_t n, int line)
{
	size_t i;

	if (!n)
		return true;
	for (i = 0; i < n; i++)
		if (line >= rg[i].from && line <= rg[i].to)
			return true;
	return false;
}

int subst_file(struct repo *r, const char *path, const struct line_range *rg,
	       size_t n, const struct subst *s)
{
	struct strbuf data = STRBUF_INIT, res = STRBUF_INIT, line = STRBUF_INIT;
	int no = 0, changed = 0;
	const char *p, *end;

	if (read_file(path, &data))
		return repo_err(r, "cannot read %s", path);
	p = sb_str(&data);
	end = p + data.len;
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t body;
		size_t len;

		len = nl ? (size_t)(nl - p) : (size_t)(end - p);
		body = len;

		no++;
		/* in a CRLF line the CR is its end, not text to match */
		if (body && p[body - 1] == '\r')
			body--;
		if (in_ranges(rg, n, no) && subst_line(s, p, body, &line) &&
		    (line.len != body || memcmp(line.buf, p, body))) {
			sb_add(&res, line.buf, line.len);
			changed++;
		} else {
			sb_add(&res, p, body);
		}
		sb_add(&res, p + body, len - body + (nl ? 1 : 0));
		p += len + (nl ? 1 : 0);
	}
	if (changed && write_file(path, res.buf, res.len))
		changed = repo_err(r, "cannot write %s", path);
	sb_release(&data);
	sb_release(&res);
	sb_release(&line);
	return changed;
}
