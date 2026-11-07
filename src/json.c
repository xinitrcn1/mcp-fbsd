/* JSON: one pass, in place. Tokens live in four parallel arrays (type, offset,
 * length, subtree end) that are reused across requests. String escapes are
 * decoded into the source buffer itself, so a string token is a pointer into
 * the request line and nothing is copied. */
#include "mcp.h"
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

typedef struct { Json *j; char *s, *e; } P;

static uint32_t tok(P *p, uint8_t t, uint32_t off)
{
	Json *j = p->j;
	if (j->n == j->cap) {
		uint32_t c = j->cap ? j->cap * 2 : 256;
		j->type = xrealloc(j->type, c);
		j->off  = xrealloc(j->off,  c * sizeof *j->off);
		j->len  = xrealloc(j->len,  c * sizeof *j->len);
		j->next = xrealloc(j->next, c * sizeof *j->next);
		j->cap = c;
	}
	uint32_t i = j->n++;
	j->type[i] = t; j->off[i] = off; j->len[i] = 0; j->next[i] = i + 1;
	return i;
}

static void ws(P *p)
{
	while (p->s < p->e && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r'))
		p->s++;
}

static int hex4(const char *s, uint32_t *v)
{
	*v = 0;
	for (int i = 0; i < 4; i++) {
		char c = s[i];
		*v <<= 4;
		if (c >= '0' && c <= '9') *v |= (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f') *v |= (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') *v |= (uint32_t)(c - 'A' + 10);
		else return -1;
	}
	return 0;
}

static char *utf8(char *w, uint32_t c)
{
	if (c < 0x80) { *w++ = (char)c; }
	else if (c < 0x800) { *w++ = (char)(0xC0 | c >> 6); *w++ = (char)(0x80 | (c & 63)); }
	else if (c < 0x10000) {
		*w++ = (char)(0xE0 | c >> 12); *w++ = (char)(0x80 | (c >> 6 & 63));
		*w++ = (char)(0x80 | (c & 63));
	} else {
		*w++ = (char)(0xF0 | c >> 18); *w++ = (char)(0x80 | (c >> 12 & 63));
		*w++ = (char)(0x80 | (c >> 6 & 63)); *w++ = (char)(0x80 | (c & 63));
	}
	return w;
}

/* p->s at opening quote. Decodes into the same bytes. */
static int str(P *p, uint8_t type)
{
	char *r = ++p->s, *w = r;
	uint32_t t = tok(p, type, (uint32_t)(r - p->j->src));
	for (;;) {
		if (r >= p->e) return -1;
		unsigned char c = (unsigned char)*r;
		if (c == '"') break;
		if (c < 0x20) return -1;
		if (c != '\\') { *w++ = *r++; continue; }
		if (r + 1 >= p->e) return -1;
		switch (r[1]) {
		case '"': *w++ = '"'; break;   case '\\': *w++ = '\\'; break;
		case '/': *w++ = '/'; break;   case 'b': *w++ = '\b'; break;
		case 'f': *w++ = '\f'; break;  case 'n': *w++ = '\n'; break;
		case 'r': *w++ = '\r'; break;  case 't': *w++ = '\t'; break;
		case 'u': {
			uint32_t cp, lo;
			if (p->e - r < 6 || hex4(r + 2, &cp)) return -1;
			r += 6;
			if (cp >= 0xD800 && cp < 0xDC00 && p->e - r >= 6 && r[0] == '\\' &&
			    r[1] == 'u' && !hex4(r + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				r += 6;
			} else if (cp >= 0xD800 && cp < 0xE000) {
				cp = 0xFFFD;
			}
			w = utf8(w, cp);
			continue;
		}
		default: return -1;
		}
		r += 2;
	}
	p->j->len[t] = (uint32_t)(w - (p->j->src + p->j->off[t]));
	*w = 0;                 /* w <= r: never past the closing quote */
	p->s = r + 1;
	return 0;
}

static int value(P *p, int depth)
{
	ws(p);
	if (p->s >= p->e) return -1;
	Json *j = p->j;
	char c = *p->s;
	if (c == '"') return str(p, J_STR);
	if (c == '{' || c == '[') {
		if (depth >= MAX_DEPTH) return -1;
		char close = c == '{' ? '}' : ']';
		uint32_t t = tok(p, c == '{' ? J_OBJ : J_ARR, (uint32_t)(p->s - j->src));
		uint32_t cnt = 0;
		p->s++;
		ws(p);
		if (p->s < p->e && *p->s == close) { p->s++; goto done; }
		for (;;) {
			if (close == '}') {
				ws(p);
				if (p->s >= p->e || *p->s != '"' || str(p, J_STR)) return -1;
				ws(p);
				if (p->s >= p->e || *p->s++ != ':') return -1;
			}
			if (value(p, depth + 1)) return -1;
			cnt++;
			ws(p);
			if (p->s >= p->e) return -1;
			if (*p->s == ',') { p->s++; continue; }
			if (*p->s == close) { p->s++; break; }
			return -1;
		}
	done:
		j->len[t] = cnt;
		j->next[t] = j->n;
		return 0;
	}
	const char *lit[] = { "null", "false", "true" };
	for (int i = 0; i < 3; i++) {
		size_t l = strlen(lit[i]);
		if ((size_t)(p->e - p->s) >= l && !memcmp(p->s, lit[i], l)) {
			tok(p, (uint8_t)i, (uint32_t)(p->s - j->src));
			p->s += l;
			return 0;
		}
	}
	char *s = p->s;
	if (*s == '-') s++;
	if (s >= p->e || *s < '0' || *s > '9') return -1;
	while (s < p->e && ((*s >= '0' && *s <= '9') || *s == '.' || *s == 'e' ||
	                    *s == 'E' || *s == '+' || *s == '-'))
		s++;
	uint32_t t = tok(p, J_NUM, (uint32_t)(p->s - j->src));
	j->len[t] = (uint32_t)(s - p->s);
	p->s = s;
	return 0;
}

int json_parse(Json *j, char *src, size_t n)
{
	P p = { j, src, src + n };
	j->n = 0;
	j->src = src;
	if (value(&p, 0)) return -1;
	ws(&p);
	return p.s == p.e ? 0 : -1;
}

uint32_t json_get(const Json *j, uint32_t o, const char *key)
{
	if (o >= j->n || j->type[o] != J_OBJ) return 0;
	size_t kl = strlen(key);
	for (uint32_t i = o + 1; i < j->next[o]; i = j->next[i + 1])
		if (j->len[i] == kl && !memcmp(j->src + j->off[i], key, kl))
			return i + 1;
	return 0;
}

const char *json_str(const Json *j, uint32_t t, size_t *len)
{
	if (!t || t >= j->n || j->type[t] != J_STR) return NULL;
	if (len) *len = j->len[t];
	return j->src + j->off[t];
}

bool json_num(const Json *j, uint32_t t, double *v)
{
	if (!t || t >= j->n || j->type[t] != J_NUM || j->len[t] > 63) return false;
	char tmp[64];
	memcpy(tmp, j->src + j->off[t], j->len[t]);
	tmp[j->len[t]] = 0;
	*v = strtod(tmp, NULL);
	return true;
}

const char *arg_str(const Json *j, uint32_t o, const char *k, size_t *len)
{
	return json_str(j, json_get(j, o, k), len);
}

long long arg_int(const Json *j, uint32_t o, const char *k, long long def)
{
	double v;
	uint32_t t = json_get(j, o, k);
	if (json_num(j, t, &v)) return (long long)v;
	const char *s = json_str(j, t, NULL);     /* tolerate "42" */
	if (s && *s) { char *e; long long x = strtoll(s, &e, 10); if (!*e) return x; }
	return def;
}

int arg_bool(const Json *j, uint32_t o, const char *k, int def)
{
	uint32_t t = json_get(j, o, k);
	if (!t) return def;
	if (j->type[t] == J_TRUE) return 1;
	if (j->type[t] == J_FALSE) return 0;
	const char *s = json_str(j, t, NULL);
	if (s) return !strcmp(s, "true") || !strcmp(s, "1");
	return def;
}

/* Length of a valid UTF-8 sequence at s, or 0. */
static size_t u8len(const unsigned char *s, size_t n)
{
	unsigned char c = s[0];
	size_t l; uint32_t cp, min;
	if (c < 0xC2) return 0;
	if (c < 0xE0) { l = 2; cp = c & 0x1F; min = 0x80; }
	else if (c < 0xF0) { l = 3; cp = c & 0x0F; min = 0x800; }
	else if (c < 0xF5) { l = 4; cp = c & 0x07; min = 0x10000; }
	else return 0;
	if (n < l) return 0;
	for (size_t i = 1; i < l; i++) {
		if ((s[i] & 0xC0) != 0x80) return 0;
		cp = cp << 6 | (s[i] & 0x3F);
	}
	if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return 0;
	return l;
}

void json_esc(Buf *b, const char *s, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	const unsigned char *u = (const unsigned char *)s;
	buf_reserve(b, n + n / 8 + 16);
	buf_putc(b, '"');
	size_t run = 0;                          /* start of a plain-byte run */
	for (size_t i = 0; i < n;) {
		unsigned char c = u[i];
		const char *esc = NULL;
		char ub[6];
		size_t el = 2, adv = 1;
		if (c >= 0x20 && c != '"' && c != '\\' && c < 0x80) { i++; continue; }
		if (c >= 0x80) {
			size_t l = u8len(u + i, n - i);
			if (l) { i += l; continue; }
			esc = "\\ufffd"; el = 6;
		} else if (c == '"') esc = "\\\"";
		else if (c == '\\') esc = "\\\\";
		else if (c == '\n') esc = "\\n";
		else if (c == '\t') esc = "\\t";
		else if (c == '\r') esc = "\\r";
		else {
			memcpy(ub, "\\u00", 4); ub[4] = hx[c >> 4]; ub[5] = hx[c & 15];
			esc = ub; el = 6;
		}
		buf_put(b, s + run, i - run);
		buf_put(b, esc, el);
		i += adv;
		run = i;
	}
	buf_put(b, s + run, n - run);
	buf_putc(b, '"');
}
