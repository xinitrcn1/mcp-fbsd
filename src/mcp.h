#ifndef MCP_H
#define MCP_H

#define _DEFAULT_SOURCE 1
#if defined(__linux__)
#define _GNU_SOURCE 1
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include "config.h"

#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define PRINTF(a, b) __attribute__((format(printf, a, b)))

/* ---- buf.c: growable byte buffer, the only dynamic container ---- */
typedef struct { char *p; size_t n, cap; } Buf;
void buf_reserve(Buf *b, size_t extra);          /* aborts on OOM */
void buf_put(Buf *b, const void *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...) PRINTF(2, 3);
void buf_b64(Buf *b, const unsigned char *s, size_t n);
void buf_free(Buf *b);
static inline void buf_clear(Buf *b) { b->n = 0; }
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
uint64_t now_ms(void);

/* ---- json.c: in-place parser into flat token arrays ---- */
enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct {
	uint8_t  *type;
	uint32_t *off, *len;  /* bytes in src; len = child count for containers */
	uint32_t *next;       /* index just past this token's subtree */
	uint32_t n, cap;
	char *src;            /* strings are unescaped and NUL-terminated in place */
} Json;
int         json_parse(Json *j, char *src, size_t n);   /* 0 ok */
uint32_t    json_get(const Json *j, uint32_t obj, const char *key); /* 0 = absent */
const char *json_str(const Json *j, uint32_t t, size_t *len);  /* NULL unless string */
bool        json_num(const Json *j, uint32_t t, double *v);
void        json_esc(Buf *b, const char *s, size_t n);  /* quoted, UTF-8 repaired */

/* argument helpers on an object token; 0 means "no object" */
const char *arg_str(const Json *j, uint32_t o, const char *k, size_t *len);
long long   arg_int(const Json *j, uint32_t o, const char *k, long long def);
int         arg_bool(const Json *j, uint32_t o, const char *k, int def);

/* ---- tool plumbing ---- */
typedef struct { char raw[128]; uint8_t n; } Id;   /* request id, JSON text */
typedef struct {
	Buf out;              /* result text, raw (escaped on send) */
	Buf img;              /* encoded image bytes, if any */
	const char *mime;
} Res;
enum { T_OK, T_ERR, T_DEFER };
typedef int (*ToolFn)(const Json *j, uint32_t args, Res *r, const Id *id);

int  err(Res *r, const char *fmt, ...) PRINTF(2, 3);   /* sets text, returns T_ERR */
int  errno_err(Res *r, const char *what);              /* "what: strerror" */
void reply_tool(const Id *id, Res *r, bool is_err);    /* main.c */

/* ---- global config (main.c) ---- */
typedef struct {
	char  *root[CFG_MAX_ROOTS];
	size_t rootlen[CFG_MAX_ROOTS];
	int    nroot;
	bool   ro, noexec, verbose;
	const char *shell;
} Cfg;
extern Cfg g_cfg;

/* ---- fs.c ---- */
/* Resolve `in` (~ expanded) to an absolute path in `out` and check roots.
 * must_exist=false allows missing trailing components (for creation). */
int path_resolve(Res *r, const char *in, char out[PATH_MAX], bool must_exist);
int mkdir_p(char *path, size_t upto);
int t_read(const Json *, uint32_t, Res *, const Id *);
int t_write(const Json *, uint32_t, Res *, const Id *);
int t_edit(const Json *, uint32_t, Res *, const Id *);
int t_ls(const Json *, uint32_t, Res *, const Id *);
int t_stat(const Json *, uint32_t, Res *, const Id *);
int t_mv(const Json *, uint32_t, Res *, const Id *);
int t_mkdir(const Json *, uint32_t, Res *, const Id *);

/* ---- search.c ---- */
int t_search(const Json *, uint32_t, Res *, const Id *);

/* ---- proc.c ---- */
#include <poll.h>
void   proc_init(void);
int    proc_pollfds(struct pollfd *pf, int max);   /* appends session fds */
void   proc_pump(const struct pollfd *pf, int n);  /* drains readable fds */
void   proc_reap(void);                             /* after SIGCHLD */
int    proc_timeout(void);                          /* poll timeout ms, -1 none */
void   proc_wake(void);                             /* answer ready waiters */
bool   proc_waiting(void);
void   proc_shutdown(void);                         /* SIGHUP live sessions */
int t_exec(const Json *, uint32_t, Res *, const Id *);
int t_pwrite(const Json *, uint32_t, Res *, const Id *);
int t_pread(const Json *, uint32_t, Res *, const Id *);
int t_pkill(const Json *, uint32_t, Res *, const Id *);
int t_plist(const Json *, uint32_t, Res *, const Id *);

/* ---- img.c ---- */
int t_img(const Json *, uint32_t, Res *, const Id *);
/* read_file hands image files here; returns T_OK with r->img set, or T_ERR */
int img_view(Res *r, const char *path, unsigned maxdim);

#endif
