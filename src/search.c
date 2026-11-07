/* search: one synchronous walk with a result cap and a time budget. Names are
 * matched with fnmatch, content with POSIX regex or a plain substring. */
#include "mcp.h"
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <fts.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const skip_dirs[] = { ".git", ".hg", ".svn", "node_modules", "__pycache__" };

typedef struct {
	regex_t re;
	const char *lit;
	size_t litn;
	bool fixed, icase, has_re;
} Pat;

static bool line_match(const Pat *p, const char *s, size_t n)
{
	if (p->fixed) {
		if (n < p->litn) return false;
		for (size_t i = 0; i + p->litn <= n; i++)
			if (p->icase ? !strncasecmp(s + i, p->lit, p->litn) : !memcmp(s + i, p->lit, p->litn))
				return true;
		return false;
	}
	regmatch_t m[1] = { { 0, (regoff_t)n } };
#ifdef REG_STARTEND
	return regexec(&p->re, s, 0, m, REG_STARTEND) == 0;
#else
	char save = s[n];
	((char *)s)[n] = 0;
	int rc = regexec(&p->re, s, 0, m, 0);
	((char *)s)[n] = save;
	return rc == 0;
#endif
}

static Buf g_file;

/* Returns matches added, or -1 when the cap is hit. */
static long grep_file(Res *r, const Pat *p, const char *path, const char *shown,
                      off_t size, long left)
{
	if (size > (off_t)CFG_SEARCH_FSIZE) return 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) return 0;
	buf_clear(&g_file);
	buf_reserve(&g_file, (size_t)size + 1);
	ssize_t k;
	while ((k = read(fd, g_file.p + g_file.n, g_file.cap - g_file.n - 1)) > 0) {
		g_file.n += (size_t)k;
		if (g_file.cap - g_file.n <= 1) buf_reserve(&g_file, 65536);
	}
	close(fd);
	g_file.p[g_file.n] = 0;
	if (memchr(g_file.p, 0, g_file.n < 8192 ? g_file.n : 8192)) return 0;   /* binary */

	long hits = 0;
	unsigned ln = 0;
	char *s = g_file.p, *end = g_file.p + g_file.n;
	while (s < end) {
		char *e = memchr(s, '\n', (size_t)(end - s));
		if (!e) e = end;
		ln++;
		if (line_match(p, s, (size_t)(e - s))) {
			if (hits == left) return -1;
			size_t n = (size_t)(e - s);
			while (n && (s[n - 1] == '\r')) n--;
			const char *t = s;
			while (n && (*t == ' ' || *t == '\t')) { t++; n--; }   /* indentation is noise here */
			buf_printf(&r->out, "%s:%u:", shown, ln);
			if (n > 200) { buf_put(&r->out, t, 200); buf_puts(&r->out, "..."); }
			else buf_put(&r->out, t, n);
			buf_putc(&r->out, '\n');
			hits++;
		}
		s = e + 1;
	}
	return hits;
}

int t_search(const Json *j, uint32_t a, Res *r, const Id *id)
{
	(void)id;
	char root[PATH_MAX];
	const char *in = arg_str(j, a, "path", NULL);
	if (path_resolve(r, in, root, true)) return T_ERR;
	const char *glob = arg_str(j, a, "glob", NULL);
	size_t ren;
	const char *re = arg_str(j, a, "re", &ren);
	bool hidden = arg_bool(j, a, "hidden", 0);
	long max = (long)arg_int(j, a, "max", CFG_SEARCH_MAX);
	if (max <= 0) max = CFG_SEARCH_MAX;
	if (!glob && !re) return err(r, "glob or re required");

	Pat p = { .icase = arg_bool(j, a, "i", 0), .fixed = arg_bool(j, a, "fixed", 0) };
	if (re && !ren) return err(r, "re is empty");
	if (re && p.fixed) { p.lit = re; p.litn = ren; }
	else if (re) {
		int rc = regcomp(&p.re, re, REG_EXTENDED | REG_NOSUB | REG_NEWLINE | (p.icase ? REG_ICASE : 0));
		if (rc) {
			char eb[128];
			regerror(rc, &p.re, eb, sizeof eb);
			return err(r, "bad regex: %s", eb);
		}
		p.has_re = true;
	}
	bool glob_path = glob && strchr(glob, '/');

	/* Show paths relative to what the caller typed. */
	size_t rootl = strlen(root);
	char *argv[] = { root, NULL };
	FTS *fts = fts_open(argv, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
	if (!fts) { if (p.has_re) regfree(&p.re); return errno_err(r, in); }
	buf_clear(&r->out);
	uint64_t deadline = now_ms() + CFG_SEARCH_MS;
	long found = 0;
	unsigned files = 0;
	const char *stop = NULL;
	FTSENT *e;
	char shown[PATH_MAX];
	while ((e = fts_read(fts))) {
		if (e->fts_info == FTS_D) {
			if (e->fts_level > 0) {
				const char *n = e->fts_name;
				bool skip = !hidden && n[0] == '.';
				for (size_t i = 0; !skip && i < countof(skip_dirs); i++) skip = !strcmp(n, skip_dirs[i]);
				if (skip) fts_set(fts, e, FTS_SKIP);
			}
			continue;
		}
		if (e->fts_info != FTS_F && e->fts_info != FTS_SL) continue;
		if (!hidden && e->fts_name[0] == '.' && e->fts_level > 0) continue;
		const char *rel = e->fts_path + rootl;
		if (*rel == '/') rel++;
		if (glob && fnmatch(glob, glob_path ? rel : e->fts_name,
		                    p.icase ? FNM_CASEFOLD : 0)) continue;
		if (!*rel) rel = e->fts_name;
		if (in[0] == '.' && !in[1]) snprintf(shown, sizeof shown, "%s", rel);
		else snprintf(shown, sizeof shown, "%s%s%s", in, in[strlen(in) - 1] == '/' ? "" : "/", rel);
		if (e->fts_level == 0) snprintf(shown, sizeof shown, "%s", in);
		if (!re) {
			if (found == max) { stop = "cap"; break; }
			buf_printf(&r->out, "%s\n", shown);
			found++;
		} else if (e->fts_info == FTS_F) {
			long h = grep_file(r, &p, e->fts_path, shown, e->fts_statp->st_size, max - found);
			if (h < 0) { stop = "cap"; found = max; break; }
			found += h;
		}
		if ((++files & 63) == 0 && now_ms() > deadline) { stop = "time"; break; }
	}
	fts_close(fts);
	if (p.has_re) regfree(&p.re);
	if (g_file.cap > (1u << 20)) buf_free(&g_file);
	if (stop && !strcmp(stop, "cap")) buf_printf(&r->out, "[stopped at %ld results]\n", max);
	else if (stop) buf_printf(&r->out, "[stopped after %ds, %ld results]\n", CFG_SEARCH_MS / 1000, found);
	if (!r->out.n) buf_puts(&r->out, "no matches");
	return T_OK;
}
