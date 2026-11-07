/* File tools. Reads stream through one fixed chunk; writes go to a temp file
 * in the same directory and are renamed over the target. */
#include "mcp.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- paths ---- */

static bool in_roots(const char *p)
{
	if (!g_cfg.nroot) return true;
	for (int i = 0; i < g_cfg.nroot; i++) {
		size_t l = g_cfg.rootlen[i];
		if (!strncmp(p, g_cfg.root[i], l) && (p[l] == '/' || p[l] == 0 || l == 1))
			return true;
	}
	return false;
}

int path_resolve(Res *r, const char *in, char out[PATH_MAX], bool must_exist)
{
	char tmp[PATH_MAX];
	if (!in || !*in) return err(r, "path required");
	if (in[0] == '~' && (in[1] == '/' || !in[1])) {
		const char *h = getenv("HOME");
		if (!h) return err(r, "HOME unset");
		if ((size_t)snprintf(tmp, sizeof tmp, "%s%s", h, in + 1) >= sizeof tmp) return err(r, "path too long");
	} else if (strlen(in) >= sizeof tmp) {
		return err(r, "path too long");
	} else {
		strcpy(tmp, in);
	}
	if (realpath(tmp, out)) goto check;
	if (must_exist || errno != ENOENT) return errno_err(r, in);

	/* Find the deepest existing ancestor, resolve it, append the rest. */
	size_t l = strlen(tmp);
	while (l > 1 && tmp[l - 1] == '/') tmp[--l] = 0;
	char *cut = tmp + l;
	for (;;) {
		char *s = cut;
		while (s > tmp && s[-1] != '/') s--;
		if (s == tmp) {                     /* relative, nothing exists */
			if (!getcwd(out, PATH_MAX)) return errno_err(r, "getcwd");
			cut = tmp;
			break;
		}
		char save = s[-1];
		s[-1] = 0;
		bool ok = realpath(s - 1 == tmp ? "/" : tmp, out) != NULL;
		s[-1] = save;
		if (ok) { cut = s; break; }
		if (errno != ENOENT) return errno_err(r, in);
		cut = s - 1;
	}
	/* rest must not climb */
	for (char *c = cut; *c;) {
		char *e = strchr(c, '/');
		size_t cl = e ? (size_t)(e - c) : strlen(c);
		if ((cl == 2 && c[0] == '.' && c[1] == '.')) return err(r, "%s: '..' after missing component", in);
		c += cl + (e != NULL);
	}
	size_t ol = strlen(out);
	if (ol + 1 + strlen(cut) >= PATH_MAX) return err(r, "path too long");
	if (out[ol - 1] != '/') out[ol++] = '/';
	strcpy(out + ol, cut);
check:
	if (!in_roots(out)) return err(r, "%s: outside allowed roots", in);
	return T_OK;
}

int mkdir_p(char *p, size_t upto)
{
	/* creates every directory of p[0..upto) */
	for (size_t i = 1; i <= upto; i++) {
		if (i < upto && p[i] != '/') continue;
		char c = p[i];
		p[i] = 0;
		int rc = mkdir(p, 0777);
		p[i] = c;
		if (rc && errno != EEXIST) return -1;
	}
	return 0;
}

/* ---- read ---- */

/* A line reader over one fixed 64K chunk. */
typedef struct { int fd; char buf[65536]; size_t a, b; bool eof; } Rd;

/* Returns 1 with a line (no '\n') in s and n; 0 at EOF; -1 on error. A line
 * longer than the chunk is cut to CFG_LINE_MAX and `more` gets the rest. */
static int rd_line(Rd *rd, const char **s, size_t *n, size_t *more)
{
	*more = 0;
	for (;;) {
		char *nl = memchr(rd->buf + rd->a, '\n', rd->b - rd->a);
		if (nl) {
			*s = rd->buf + rd->a; *n = (size_t)(nl - *s);
			rd->a = (size_t)(nl - rd->buf) + 1;
			return 1;
		}
		if (rd->eof) {
			if (rd->a == rd->b) return 0;
			*s = rd->buf + rd->a; *n = rd->b - rd->a;
			rd->a = rd->b;
			return 1;
		}
		if (rd->a) { memmove(rd->buf, rd->buf + rd->a, rd->b - rd->a); rd->b -= rd->a; rd->a = 0; }
		if (rd->b == sizeof rd->buf) {           /* line longer than chunk: keep a prefix */
			static char keep[CFG_LINE_MAX];
			memcpy(keep, rd->buf, CFG_LINE_MAX);
			size_t drop = rd->b - CFG_LINE_MAX;
			rd->a = rd->b = 0;
			for (;;) {
				ssize_t k = read(rd->fd, rd->buf, sizeof rd->buf);
				if (k <= 0) { rd->eof = true; break; }
				char *e = memchr(rd->buf, '\n', (size_t)k);
				if (e) {
					drop += (size_t)(e - rd->buf);
					rd->a = (size_t)(e - rd->buf) + 1;
					rd->b = (size_t)k;
					break;
				}
				drop += (size_t)k;
			}
			*s = keep; *n = CFG_LINE_MAX; *more = drop;
			return 1;
		}
		ssize_t k = read(rd->fd, rd->buf + rd->b, sizeof rd->buf - rd->b);
		if (k < 0) { if (errno == EINTR) continue; return -1; }
		if (k == 0) rd->eof = true;
		rd->b += (size_t)k;
	}
}

static const char *img_mime(const unsigned char *h, size_t n)
{
	if (n >= 8 && !memcmp(h, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
	if (n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return "image/jpeg";
	if (n >= 6 && (!memcmp(h, "GIF87a", 6) || !memcmp(h, "GIF89a", 6))) return "image/gif";
	if (n >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WEBP", 4)) return "image/webp";
	if (n >= 3 && h[0] == 'P' && (h[1] == '2' || h[1] == '3' || h[1] == '5' || h[1] == '6') &&
	    (h[2] == ' ' || h[2] == '\n' || h[2] == '\r' || h[2] == '\t' || h[2] == '#'))
		return "pnm";                       /* re-encoded by img_view */
	return NULL;
}

static Rd g_rd;   /* 64K, static: one read at a time */

int t_read(const Json *j, uint32_t a, Res *r, const Id *id)
{
	(void)id;
	char path[PATH_MAX];
	if (path_resolve(r, arg_str(j, a, "path", NULL), path, true)) return T_ERR;
	long long off = arg_int(j, a, "offset", 1);
	long long lim = arg_int(j, a, "limit", CFG_READ_LINES);
	bool num = arg_bool(j, a, "n", 0);
	if (lim <= 0) lim = CFG_READ_LINES;

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return errno_err(r, path);
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		close(fd);
		return err(r, "%s: not a regular file", path);
	}
	Rd *rd = &g_rd;
	rd->fd = fd; rd->a = rd->b = 0; rd->eof = false;
	ssize_t k = read(fd, rd->buf, sizeof rd->buf);
	if (k < 0) { close(fd); return errno_err(r, path); }
	rd->b = (size_t)k;
	if (k == 0) rd->eof = true;

	const char *mime = img_mime((unsigned char *)rd->buf, rd->b);
	if (mime) {
		close(fd);
		if (!strcmp(mime, "image/gif") || !strcmp(mime, "image/webp")) {
			if (st.st_size > CFG_IMG_PASS) return err(r, "%s: image too large to pass through", path);
			FILE *f = fopen(path, "rb");
			if (!f) return errno_err(r, path);
			buf_clear(&r->img);
			buf_reserve(&r->img, (size_t)st.st_size);
			r->img.n = fread(r->img.p, 1, (size_t)st.st_size, f);
			fclose(f);
			r->mime = mime;
			return T_OK;
		}
		return img_view(r, path, CFG_IMG_DIM);
	}
	if (memchr(rd->buf, 0, rd->b < 4096 ? rd->b : 4096)) {
		close(fd);
		return err(r, "%s: binary, %lld bytes", path, (long long)st.st_size);
	}

	/* negative offset: count lines first */
	unsigned long long total = 0;
	const char *s; size_t n, more;
	if (off < 0) {
		int rc;
		while ((rc = rd_line(rd, &s, &n, &more)) > 0) total++;
		if (rc < 0) { close(fd); return errno_err(r, path); }
		long long st0 = (long long)total + off + 1;
		off = st0 < 1 ? 1 : st0;
		lseek(fd, 0, SEEK_SET);
		rd->a = rd->b = 0; rd->eof = false;
		total = 0;
	}
	if (off == 0) off = 1;

	unsigned long long first = (unsigned long long)off, last = 0, ln = 0;
	bool capped = false;
	int rc;
	buf_clear(&r->out);
	while ((rc = rd_line(rd, &s, &n, &more)) > 0) {
		ln++;
		if (ln < first || capped) continue;
		if (ln >= first + (unsigned long long)lim) { capped = true; continue; }
		if (r->out.n >= CFG_READ_BYTES) { capped = true; continue; }
		if (num) buf_printf(&r->out, "%llu\t", ln);
		if (n > CFG_LINE_MAX) { more += n - CFG_LINE_MAX; n = CFG_LINE_MAX; }
		buf_put(&r->out, s, n);
		if (more) buf_printf(&r->out, "...[+%zu]", more);
		buf_putc(&r->out, '\n');
		last = ln;
	}
	close(fd);
	if (rc < 0) return errno_err(r, path);
	total = ln;
	if (first > total && total) return err(r, "offset %llu past end (%llu lines)", first, total);
	if (first > 1 || last < total)
		buf_printf(&r->out, "[lines %llu-%llu of %llu]\n", first, last, total);
	return T_OK;
}

/* ---- write ---- */

static int write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t k = write(fd, p, n);
		if (k < 0) { if (errno == EINTR) continue; return -1; }
		p += k; n -= (size_t)k;
	}
	return 0;
}

/* Replace path atomically with data, keeping the old file's mode. */
static int replace_file(Res *r, char *path, const char *data, size_t n)
{
	struct stat st;
	mode_t mode;
	bool had = stat(path, &st) == 0;
	if (had) mode = st.st_mode & 07777;
	else {
		mode_t um = umask(0);
		umask(um);
		mode = 0666 & ~um;
		char *sl = strrchr(path, '/');
		if (sl && sl > path) {
			*sl = 0;
			struct stat ds;
			if (stat(path, &ds)) {
				*sl = '/';
				if (mkdir_p(path, (size_t)(sl - path))) return errno_err(r, "mkdir");
			} else *sl = '/';
		}
	}
	char tmp[PATH_MAX];
	if ((size_t)snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= sizeof tmp) return err(r, "path too long");
	int fd = mkstemp(tmp);
	if (fd < 0) return errno_err(r, path);
	if (write_all(fd, data, n) || fchmod(fd, mode)) {
		int e = errno; close(fd); unlink(tmp); errno = e;
		return errno_err(r, path);
	}
	if (close(fd) || rename(tmp, path)) {
		int e = errno; unlink(tmp); errno = e;
		return errno_err(r, path);
	}
	return T_OK;
}

int t_write(const Json *j, uint32_t a, Res *r, const Id *id)
{
	(void)id;
	char path[PATH_MAX];
	size_t n;
	const char *data = arg_str(j, a, "content", &n);
	if (!data) return err(r, "content required");
	if (path_resolve(r, arg_str(j, a, "path", NULL), path, false)) return T_ERR;
	if (arg_bool(j, a, "append", 0)) {
		char *sl = strrchr(path, '/');
		if (sl && sl > path) { *sl = 0; struct stat ds; bool miss = stat(path, &ds) != 0; *sl = '/';
			if (miss && mkdir_p(path, (size_t)(sl - path))) return errno_err(r, "mkdir"); }
		int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
		if (fd < 0) return errno_err(r, path);
		int rc = write_all(fd, data, n);
		if (close(fd) || rc) return errno_err(r, path);
		return T_OK;
	}
	return replace_file(r, path, data, n);
}

/* ---- edit ---- */

static size_t count_occ(const char *h, size_t hn, const char *nd, size_t nn, const char **first)
{
	size_t c = 0;
	*first = NULL;
	for (const char *p = h; nn && (size_t)(h + hn - p) >= nn;) {
		const char *q = memmem(p, (size_t)(h + hn - p), nd, nn);
		if (!q) break;
		if (!c) *first = q;
		c++;
		p = q + nn;
	}
	return c;
}

static unsigned line_of(const char *base, const char *p)
{
	unsigned l = 1;
	for (; base < p; base++) l += *base == '\n';
	return l;
}

/* Hint for a miss: lines where the first non-blank line of `old`, trimmed,
 * appears. Most misses are indentation or a stale copy. */
static void miss_hint(Res *r, const char *h, size_t hn, const char *old, size_t on)
{
	const char *s = old, *e = old + on;
	while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
	const char *le = memchr(s, '\n', (size_t)(e - s));
	if (!le) le = e;
	while (le > s && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r')) le--;
	if (le - s < 3) return;
	int shown = 0;
	const char *q, *p = h;
	while (shown < 5 && (q = memmem(p, (size_t)(h + hn - p), s, (size_t)(le - s)))) {
		buf_printf(&r->out, shown ? ",%u" : "; first line found at line %u", line_of(h, q));
		shown++;
		p = q + (le - s);
	}
}

int t_edit(const Json *j, uint32_t a, Res *r, const Id *id)
{
	(void)id;
	char path[PATH_MAX];
	if (path_resolve(r, arg_str(j, a, "path", NULL), path, true)) return T_ERR;

	/* gather edits: either top-level old/new or edits[] */
	uint32_t arr = json_get(j, a, "edits");
	uint32_t one[1] = { a }, *list = one, nlist = 1;
	uint32_t buf_idx[64];
	if (arr && j->type[arr] == J_ARR) {
		if (j->len[arr] > countof(buf_idx)) return err(r, "at most %zu edits", countof(buf_idx));
		nlist = 0;
		for (uint32_t i = arr + 1; i < j->next[arr]; i = j->next[i]) buf_idx[nlist++] = i;
		list = buf_idx;
	}
	FILE *f = fopen(path, "rb");
	if (!f) return errno_err(r, path);
	Buf cur = { 0 }, nxt = { 0 };
	char chunk[65536];
	size_t k;
	while ((k = fread(chunk, 1, sizeof chunk, f))) buf_put(&cur, chunk, k);
	bool rerr = ferror(f);
	fclose(f);
	if (rerr) { buf_free(&cur); return errno_err(r, path); }
	if (!cur.p) buf_put(&cur, "", 0);

	int rc = T_OK;
	for (uint32_t e = 0; e < nlist; e++) {
		size_t on, nn;
		const char *old = arg_str(j, list[e], "old", &on);
		const char *nw = arg_str(j, list[e], "new", &nn);
		long long want = arg_int(j, list[e], "count", 1);
		if (!old || !nw) { rc = err(r, "edit %u: old and new required", e + 1); break; }
		if (!on) { rc = err(r, "edit %u: old is empty", e + 1); break; }
		const char *first;
		size_t c = count_occ(cur.p, cur.n, old, on, &first);
		if (c == 0 || (want > 0 && c != (size_t)want)) {
			if (nlist > 1) buf_printf(&r->out, "edit %u: ", e + 1);
			else buf_clear(&r->out);
			if (c) buf_printf(&r->out, "old found %zu times, expected %lld", c, want);
			else { buf_puts(&r->out, "old not found"); miss_hint(r, cur.p, cur.n, old, on); }
			rc = T_ERR;
			break;
		}
		buf_clear(&nxt);
		const char *p = cur.p, *end = cur.p + cur.n, *q;
		while ((q = memmem(p, (size_t)(end - p), old, on))) {
			buf_put(&nxt, p, (size_t)(q - p));
			buf_put(&nxt, nw, nn);
			p = q + on;
		}
		buf_put(&nxt, p, (size_t)(end - p));
		Buf t = cur; cur = nxt; nxt = t;
	}
	if (rc == T_OK) rc = replace_file(r, path, cur.p, cur.n);
	buf_free(&cur);
	buf_free(&nxt);
	return rc;
}

/* ---- ls ---- */

typedef struct { Buf names; uint32_t *idx; uint32_t n, cap; } Names;

static const char *g_nb;   /* qsort has no context argument in C11 */
static int name_cmp(const void *x, const void *y)
{
	return strcmp(g_nb + *(const uint32_t *)x, g_nb + *(const uint32_t *)y);
}

static unsigned g_ls_count;

static void ls_dir(Res *r, char *path, size_t pl, int depth, int ind, bool hidden)
{
	DIR *d = opendir(path);
	if (!d) { buf_printf(&r->out, "%*s[%s]\n", ind * 2, "", strerror(errno)); return; }
	Names nm = { 0 };
	struct dirent *de;
	while ((de = readdir(d))) {
		const char *n = de->d_name;
		if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]))) continue;
		if (!hidden && n[0] == '.') continue;
		if (nm.n == nm.cap) { nm.cap = nm.cap ? nm.cap * 2 : 64; nm.idx = xrealloc(nm.idx, nm.cap * sizeof *nm.idx); }
		nm.idx[nm.n++] = (uint32_t)nm.names.n;
		buf_put(&nm.names, n, strlen(n) + 1);
	}
	closedir(d);
	g_nb = nm.names.p;
	if (nm.n) qsort(nm.idx, nm.n, sizeof *nm.idx, name_cmp);
	for (uint32_t i = 0; i < nm.n; i++) {
		if (g_ls_count >= CFG_LS_MAX) {
			buf_printf(&r->out, "%*s[+%u more]\n", ind * 2, "", nm.n - i);
			break;
		}
		g_ls_count++;
		const char *n = nm.names.p + nm.idx[i];
		size_t nl = strlen(n);
		if (pl + 1 + nl >= PATH_MAX) continue;
		path[pl] = '/';
		memcpy(path + pl + 1, n, nl + 1);
		struct stat st;
		if (lstat(path, &st)) { buf_printf(&r->out, "%*s%s ?\n", ind * 2, "", n); continue; }
		if (S_ISDIR(st.st_mode)) {
			buf_printf(&r->out, "%*s%s/\n", ind * 2, "", n);
			if (depth > 1) ls_dir(r, path, pl + 1 + nl, depth - 1, ind + 1, hidden);
		} else if (S_ISLNK(st.st_mode)) {
			char t[PATH_MAX];
			ssize_t k = readlink(path, t, sizeof t - 1);
			t[k < 0 ? 0 : k] = 0;
			buf_printf(&r->out, "%*s%s@ -> %s\n", ind * 2, "", n, t);
		} else if (S_ISREG(st.st_mode)) {
			buf_printf(&r->out, "%*s%s %lld%s\n", ind * 2, "", n, (long long)st.st_size,
			           st.st_mode & 0111 ? "*" : "");
		} else {
			buf_printf(&r->out, "%*s%s %s\n", ind * 2, "", n,
			           S_ISFIFO(st.st_mode) ? "fifo" : S_ISSOCK(st.st_mode) ? "socket" : "device");
		}
		path[pl] = 0;
	}
	path[pl] = 0;
	buf_free(&nm.names);
	free(nm.idx);
}

int t_ls(const Json *j, uint32_t a, Res *r, const Id *id)
{
	(void)id;
	char path[PATH_MAX];
	if (path_resolve(r, arg_str(j, a, "path", NULL), path, true)) return T_ERR;
	long long depth = arg_int(j, a, "depth", 1);
	if (depth < 1) depth = 1;
	if (depth > 8) depth = 8;
	struct stat st;
	if (stat(path, &st)) return errno_err(r, path);
	if (!S_ISDIR(st.st_mode)) return err(r, "%s: not a directory", path);
	buf_clear(&r->out);
	g_ls_count = 0;
	size_t pl = strlen(path);
	if (pl == 1) pl = 0;                   /* "/" + "/name" */
	ls_dir(r, path, pl, (int)depth, 0, arg_bool(j, a, "hidden", 1));
	if (!r->out.n) buf_puts(&r->out, "(empty)");
	return T_OK;
}

/* ---- stat / mv / mkdir ---- */

int t_stat(const Json *j, uint32_t a, Res *r, const Id *id)
{
	(void)id;
	char path[PATH_MAX];
	const char *in = arg_str(j, a, "path", NULL);
	if (path_resolve(r, in, path, true)) return T_ERR;
	struct stat st;
	if (lstat(path, &st) && stat(path, &st)) return errno_err(r, path);
	const char *ty = S_ISREG(st.st_mode) ? "file" : S_ISDIR(st.st_mode) ? "dir" :
	                 S_ISLNK(st.st_mode) ? "link" : S_ISFIFO(st.st_mode) ? "fifo" :
	                 S_ISSOCK(st.st_mode) ? "socket" : S_ISCHR(st.st_mode) ? "chr" : "blk";
	struct passwd *pw = getpwuid(st.st_uid);
	struct group *gr = getgrgid(st.st_gid);
	char tb[32];
	struct tm tm;
	gmtime_r(&st.st_mtime, &tm);
	strftime(tb, sizeof tb, "%Y-%m-%dT%H:%M:%SZ", &tm);
	buf_clear(&r->out);
	buf_printf(&r->out, "%s %s %lld bytes mode %04o %s:%s mtime %s", path, ty,
	           (long long)st.st_size, (unsigned)(st.st_mode & 07777),
	           pw ? pw->pw_name : "?", gr ? gr->gr_name : "?", tb);
	return T_OK;
}

int t_mv(const Json *j, uint32_t a, Res *r, const Id *id)
{
	(void)id;
	char from[PATH_MAX], to[PATH_MAX];
	if (path_resolve(r, arg_str(j, a, "from", NULL), from, true)) return T_ERR;
	if (path_resolve(r, arg_str(j, a, "to", NULL), to, false)) return T_ERR;
	if (rename(from, to)) return errno_err(r, "rename");
	return T_OK;
}

int t_mkdir(const Json *j, uint32_t a, Res *r, const Id *id)
{
	(void)id;
	char path[PATH_MAX];
	if (path_resolve(r, arg_str(j, a, "path", NULL), path, false)) return T_ERR;
	if (mkdir_p(path, strlen(path))) return errno_err(r, path);
	return T_OK;
}
