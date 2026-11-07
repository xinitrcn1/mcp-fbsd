#include "mcp.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) { fputs("mcp-fbsd: out of memory\n", stderr); abort(); }
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p) { fputs("mcp-fbsd: out of memory\n", stderr); abort(); }
	return p;
}

uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void buf_reserve(Buf *b, size_t extra)
{
	if (b->cap - b->n > extra) return;          /* keep one byte for NUL */
	size_t c = b->cap ? b->cap : 256;
	while (c - b->n <= extra) c *= 2;
	b->p = xrealloc(b->p, c);
	b->cap = c;
}

void buf_put(Buf *b, const void *s, size_t n)
{
	buf_reserve(b, n);
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = 0;
}

void buf_puts(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }

void buf_putc(Buf *b, char c)
{
	buf_reserve(b, 1);
	b->p[b->n++] = c;
	b->p[b->n] = 0;
}

void buf_printf(Buf *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
	va_end(ap);
	if (k < 0) return;
	if (!b->p || (size_t)k >= b->cap - b->n) {
		buf_reserve(b, (size_t)k);
		va_start(ap, fmt);
		vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
		va_end(ap);
	}
	b->n += (size_t)k;
}

void buf_b64(Buf *b, const unsigned char *s, size_t n)
{
	static const char T[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	buf_reserve(b, (n + 2) / 3 * 4);
	char *o = b->p + b->n;
	size_t i = 0;
	for (; i + 2 < n; i += 3) {
		uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8 | s[i + 2];
		*o++ = T[v >> 18]; *o++ = T[v >> 12 & 63];
		*o++ = T[v >> 6 & 63]; *o++ = T[v & 63];
	}
	if (i < n) {
		uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < n ? (uint32_t)s[i + 1] << 8 : 0);
		*o++ = T[v >> 18]; *o++ = T[v >> 12 & 63];
		*o++ = i + 1 < n ? T[v >> 6 & 63] : '=';
		*o++ = '=';
	}
	b->n = (size_t)(o - b->p);
	b->p[b->n] = 0;
}

void buf_free(Buf *b)
{
	free(b->p);
	b->p = NULL;
	b->n = b->cap = 0;
}
