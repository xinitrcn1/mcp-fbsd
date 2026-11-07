/* MCP over stdio: one JSON-RPC message per line. Single thread; a poll loop
 * multiplexes stdin, child output and SIGCHLD. Calls that wait on a process
 * are parked (proc.c) and answered later, so other requests keep flowing. */
#include "mcp.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Cfg g_cfg = { .shell = CFG_SHELL };

#define F_WRITE 1   /* hidden with -R */
#define F_EXEC  2   /* hidden with -R or -X */

#define S(x) #x
/* Schema fragments: kept tiny, every byte here lands in the client's context. */
#define P_STR(n, d) "\"" n "\":{\"type\":\"string\"" d "}"
#define P_INT(n, d) "\"" n "\":{\"type\":\"integer\"" d "}"
#define P_BOOL(n, d) "\"" n "\":{\"type\":\"boolean\"" d "}"
#define D(s) ",\"description\":\"" s "\""
#define OBJ(props, req) "{\"type\":\"object\",\"properties\":{" props "},\"required\":[" req "]}"

static const struct Tool {
	const char *name;
	ToolFn fn;
	unsigned flags;
	const char *desc;
	const char *schema;
} tools[] = {
	{ "read", t_read, 0,
	  "Read a text file, or view an image (png/jpg/gif/webp). Partial reads end with [lines A-B of N].",
	  OBJ(P_STR("path", "") "," P_INT("offset", D("first line, 1-based; negative = from end")) ","
	      P_INT("limit", D("max lines, default 2000")) "," P_BOOL("n", D("prefix line numbers")),
	      "\"path\"") },
	{ "write", t_write, F_WRITE,
	  "Write a file (atomic replace), creating parent dirs.",
	  OBJ(P_STR("path", "") "," P_STR("content", "") "," P_BOOL("append", ""), "\"path\",\"content\"") },
	{ "edit", t_edit, F_WRITE,
	  "Replace exact text in a file. old must match `count` times (default 1). Use edits[] for several replacements in one atomic write.",
	  OBJ(P_STR("path", "") "," P_STR("old", "") "," P_STR("new", "") "," P_INT("count", "") ","
	      "\"edits\":{\"type\":\"array\",\"items\":" OBJ(P_STR("old", "") "," P_STR("new", "") "," P_INT("count", ""), "\"old\",\"new\"") "}",
	      "\"path\"") },
	{ "ls", t_ls, 0,
	  "List a directory. dir/  link@ -> target  file size.",
	  OBJ(P_STR("path", "") "," P_INT("depth", D("default 1")) "," P_BOOL("hidden", D("default true")), "\"path\"") },
	{ "stat", t_stat, 0, "File type, size, mode, owner, mtime.", OBJ(P_STR("path", ""), "\"path\"") },
	{ "mv", t_mv, F_WRITE, "Move or rename.", OBJ(P_STR("from", "") "," P_STR("to", ""), "\"from\",\"to\"") },
	{ "mkdir", t_mkdir, F_WRITE, "Create a directory and its parents.", OBJ(P_STR("path", ""), "\"path\"") },
	{ "search", t_search, 0,
	  "Find files by name glob, or lines by regex (POSIX ERE) when re is set. Output: path or path:line:text. Skips hidden and binary files.",
	  OBJ(P_STR("path", "") "," P_STR("glob", D("name filter, e.g. *.c")) "," P_STR("re", D("content pattern")) ","
	      P_BOOL("i", D("ignore case")) "," P_BOOL("fixed", D("re is a literal string")) ","
	      P_BOOL("hidden", "") "," P_INT("max", D("default 200")),
	      "\"path\"") },
	{ "exec", t_exec, F_EXEC,
	  "Run a shell command. Returns output and [exit N], or [running id=N] if still going after timeout_ms; continue with proc_read/proc_write.",
	  OBJ(P_STR("cmd", "") "," P_STR("cwd", "") "," P_INT("timeout_ms", D("default 10000")) ","
	      P_INT("idle_ms", D("return once output pauses this long")) ","
	      P_BOOL("pty", D("use a terminal, for REPLs and prompts")) "," P_STR("stdin", D("written then closed")),
	      "\"cmd\"") },
	{ "proc_write", t_pwrite, F_EXEC,
	  "Send input to a session (newline appended) and return the output that follows.",
	  OBJ(P_INT("id", "") "," P_STR("input", "") "," P_BOOL("nl", D("default true")) "," P_BOOL("eof", D("close stdin after")) ","
	      P_INT("timeout_ms", D("default 5000")) "," P_INT("idle_ms", D("default 300")),
	      "\"id\"") },
	{ "proc_read", t_pread, F_EXEC,
	  "New output of a session. Waits up to timeout_ms (default 0) for some.",
	  OBJ(P_INT("id", "") "," P_INT("timeout_ms", "") "," P_INT("idle_ms", ""), "\"id\"") },
	{ "proc_kill", t_pkill, F_EXEC,
	  "Signal a session's process group (default TERM).",
	  OBJ(P_INT("id", "") "," P_STR("sig", D("TERM, KILL, INT, HUP or a number")), "\"id\"") },
	{ "proc_list", t_plist, F_EXEC, "List sessions: id pid state age cmd.", OBJ("", "") },
	{ "img", t_img, 0,
	  "Transform an image and return it, or save to out (.png/.jpg). Order: crop, rot, flip, resize, gray.",
	  OBJ(P_STR("path", "") "," P_STR("out", "") ","
	      "\"crop\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"},\"description\":\"x,y,w,h\"}," 
	      P_INT("w", "") "," P_INT("h", "") "," P_INT("max", D("fit longest side")) ","
	      P_INT("rot", D("90, 180, 270")) "," P_STR("flip", D("h or v")) "," P_BOOL("gray", "") ","
	      P_INT("q", D("jpeg quality")) "," P_BOOL("info", D("only report size")),
	      "\"path\"") },
};

/* ---- output ---- */

static void out_write(const char *p, size_t n)
{
	while (n) {
		ssize_t k = write(1, p, n);
		if (k < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN) { struct pollfd pf = { 1, POLLOUT, 0 }; poll(&pf, 1, -1); continue; }
			exit(0);                     /* client went away */
		}
		p += k; n -= (size_t)k;
	}
}

static Buf g_resp;

static void send_resp(void)
{
	buf_putc(&g_resp, '\n');
	out_write(g_resp.p, g_resp.n);
	buf_clear(&g_resp);
}

static void head(const Id *id)
{
	buf_puts(&g_resp, "{\"jsonrpc\":\"2.0\",\"id\":");
	buf_put(&g_resp, id->raw, id->n);
}

static void reply_raw(const Id *id, const char *result)
{
	head(id);
	buf_puts(&g_resp, ",\"result\":");
	buf_puts(&g_resp, result);
	buf_putc(&g_resp, '}');
	send_resp();
}

static void reply_err(const Id *id, int code, const char *msg)
{
	if (id) head(id); else buf_puts(&g_resp, "{\"jsonrpc\":\"2.0\",\"id\":null");
	buf_printf(&g_resp, ",\"error\":{\"code\":%d,\"message\":", code);
	json_esc(&g_resp, msg, strlen(msg));
	buf_puts(&g_resp, "}}");
	send_resp();
}

void reply_tool(const Id *id, Res *r, bool is_err)
{
	head(id);
	buf_puts(&g_resp, ",\"result\":{\"content\":[");
	bool any = false;
	if (r->img.n) {
		buf_puts(&g_resp, "{\"type\":\"image\",\"data\":\"");
		buf_b64(&g_resp, (const unsigned char *)r->img.p, r->img.n);
		buf_printf(&g_resp, "\",\"mimeType\":\"%s\"}", r->mime ? r->mime : "image/png");
		any = true;
	}
	if (r->out.n || !any) {
		if (any) buf_putc(&g_resp, ',');
		buf_puts(&g_resp, "{\"type\":\"text\",\"text\":");
		json_esc(&g_resp, r->out.n ? r->out.p : "ok", r->out.n ? r->out.n : 2);
		buf_putc(&g_resp, '}');
	}
	buf_puts(&g_resp, is_err ? "],\"isError\":true}}" : "]}}");
	send_resp();
	/* keep capacity for the next call, but don't hoard a huge image */
	buf_clear(&r->out);
	if (r->img.cap > (1u << 20)) buf_free(&r->img); else buf_clear(&r->img);
	r->mime = NULL;
	if (g_resp.cap > (4u << 20)) buf_free(&g_resp);
}

int err(Res *r, const char *fmt, ...)
{
	va_list ap;
	char tmp[512];
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	buf_clear(&r->out);
	buf_puts(&r->out, tmp);
	return T_ERR;
}

int errno_err(Res *r, const char *what)
{
	return err(r, "%s: %s", what, strerror(errno));
}

/* ---- dispatch ---- */

static bool tool_visible(const struct Tool *t)
{
	if ((t->flags & (F_WRITE | F_EXEC)) && g_cfg.ro) return false;
	if ((t->flags & F_EXEC) && g_cfg.noexec) return false;
	return true;
}

static Buf g_list;   /* tools/list result, built once */

static void build_list(void)
{
	buf_puts(&g_list, "{\"tools\":[");
	bool first = true;
	for (size_t i = 0; i < countof(tools); i++) {
		if (!tool_visible(&tools[i])) continue;
		if (!first) buf_putc(&g_list, ',');
		first = false;
		buf_puts(&g_list, "{\"name\":\"");
		buf_puts(&g_list, tools[i].name);
		buf_puts(&g_list, "\",\"description\":");
		json_esc(&g_list, tools[i].desc, strlen(tools[i].desc));
		buf_puts(&g_list, ",\"inputSchema\":");
		buf_puts(&g_list, tools[i].schema);
		buf_putc(&g_list, '}');
	}
	buf_puts(&g_list, "]}");
}

static Json g_json;
static Res g_res;

static int make_id(const Json *j, uint32_t t, Id *id)
{
	Buf b = { 0 };
	if (j->type[t] == J_NUM) buf_put(&b, j->src + j->off[t], j->len[t]);
	else if (j->type[t] == J_STR) json_esc(&b, j->src + j->off[t], j->len[t]);
	else { buf_free(&b); return -1; }
	int ok = b.n < sizeof id->raw;
	if (ok) { memcpy(id->raw, b.p, b.n); id->n = (uint8_t)b.n; }
	buf_free(&b);
	return ok ? 0 : -1;
}

static void handle(char *line, size_t n)
{
	Json *j = &g_json;
	if (json_parse(j, line, n) || j->type[0] != J_OBJ) { reply_err(NULL, -32700, "parse error"); return; }
	const char *method = arg_str(j, 0, "method", NULL);
	uint32_t idt = json_get(j, 0, "id");
	Id id;
	if (!idt || j->type[idt] == J_NULL) return;         /* notification or a response */
	if (make_id(j, idt, &id)) { reply_err(NULL, -32600, "bad id"); return; }
	if (!method) { reply_err(&id, -32600, "no method"); return; }
	uint32_t params = json_get(j, 0, "params");

	if (!strcmp(method, "initialize")) {
		const char *pv = arg_str(j, params, "protocolVersion", NULL);
		Buf b = { 0 };
		buf_puts(&b, "{\"protocolVersion\":");
		pv = pv ? pv : "2025-06-18";
		json_esc(&b, pv, strlen(pv));
		buf_puts(&b, ",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"mcp-fbsd\",\"version\":\"" VERSION "\"}}");
		reply_raw(&id, b.p);
		buf_free(&b);
	} else if (!strcmp(method, "tools/list")) {
		reply_raw(&id, g_list.p);
	} else if (!strcmp(method, "tools/call")) {
		const char *name = arg_str(j, params, "name", NULL);
		uint32_t args = json_get(j, params, "arguments");
		if (args && j->type[args] != J_OBJ) args = 0;
		const struct Tool *t = NULL;
		for (size_t i = 0; name && i < countof(tools); i++)
			if (!strcmp(tools[i].name, name) && tool_visible(&tools[i])) t = &tools[i];
		if (!t) { reply_err(&id, -32602, "unknown tool"); return; }
		int rc = t->fn(j, args, &g_res, &id);
		if (rc != T_DEFER) reply_tool(&id, &g_res, rc == T_ERR);
		else { buf_clear(&g_res.out); buf_clear(&g_res.img); }
	} else if (!strcmp(method, "ping")) {
		reply_raw(&id, "{}");
	} else {
		reply_err(&id, -32601, "method not found");
	}
}

/* ---- main loop ---- */

static int g_sigpipe[2];

static void on_chld(int s)
{
	(void)s;
	int e = errno;
	(void)!write(g_sigpipe[1], "c", 1);
	errno = e;
}

static void usage(void)
{
	fputs("usage: mcp-fbsd [-RXv] [-r root]... [-s shell]\n"
	      "  -r DIR  restrict file tools to DIR (repeatable)\n"
	      "  -R      read-only: no write/edit/mv/mkdir/exec\n"
	      "  -X      no process tools\n"
	      "  -s SH   shell for exec (default " CFG_SHELL ")\n", stderr);
	exit(2);
}

int main(int argc, char **argv)
{
	int c;
	while ((c = getopt(argc, argv, "r:RXs:vh")) != -1) {
		switch (c) {
		case 'r': {
			char rp[PATH_MAX];
			if (g_cfg.nroot == CFG_MAX_ROOTS || !realpath(optarg, rp)) {
				fprintf(stderr, "mcp-fbsd: bad root %s\n", optarg);
				return 2;
			}
			size_t l = strlen(rp);
			while (l > 1 && rp[l - 1] == '/') rp[--l] = 0;
			g_cfg.root[g_cfg.nroot] = strdup(rp);
			g_cfg.rootlen[g_cfg.nroot++] = l;
			break;
		}
		case 'R': g_cfg.ro = true; break;
		case 'X': g_cfg.noexec = true; break;
		case 's': g_cfg.shell = optarg; break;
		case 'v': g_cfg.verbose = true; break;
		default: usage();
		}
	}

	signal(SIGPIPE, SIG_IGN);
	if (pipe(g_sigpipe)) return 1;
	for (int i = 0; i < 2; i++) {
		fcntl(g_sigpipe[i], F_SETFL, O_NONBLOCK);
		fcntl(g_sigpipe[i], F_SETFD, FD_CLOEXEC);
	}
	struct sigaction sa = { 0 };
	sa.sa_handler = on_chld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);

	proc_init();
	build_list();

	Buf in = { 0 };
	size_t scan = 0;          /* bytes of `in` already searched for '\n' */
	bool eof = false, skipping = false;
	struct pollfd pf[2 + CFG_PROCS];

	for (;;) {
		int n = 0;
		if (!eof) pf[n++] = (struct pollfd){ 0, POLLIN, 0 };
		pf[n++] = (struct pollfd){ g_sigpipe[0], POLLIN, 0 };
		int base = n;
		n += proc_pollfds(pf + n, (int)countof(pf) - n);
		if (eof && !proc_waiting()) break;
		int k = poll(pf, (nfds_t)n, proc_timeout());
		if (k < 0 && errno != EINTR) break;
		if (k > 0) {
			if (pf[base - 1].revents) {
				char tmp[64];
				while (read(g_sigpipe[0], tmp, sizeof tmp) > 0) {}
			}
			proc_pump(pf + base, n - base);
		}
		proc_reap();               /* cheap: WNOHANG */

		if (!eof && k > 0 && pf[0].revents) {
			buf_reserve(&in, 65536);
			ssize_t r = read(0, in.p + in.n, in.cap - in.n - 1);
			if (r <= 0) {
				if (r < 0 && errno == EINTR) goto wake;
				eof = true;
			} else {
				in.n += (size_t)r;
			}
			size_t start = 0;
			for (;;) {
				char *nl = memchr(in.p + scan, '\n', in.n - scan);
				if (!nl) break;
				size_t len = (size_t)(nl - (in.p + start));
				if (!skipping && len) {
					if (len && in.p[start + len - 1] == '\r') len--;
					handle(in.p + start, len);
				}
				skipping = false;
				start = (size_t)(nl - in.p) + 1;
				scan = start;
			}
			memmove(in.p, in.p + start, in.n - start);
			in.n -= start;
			scan = in.n;
			if (in.n > CFG_MAX_REQ) {            /* drop until newline */
				if (!skipping) reply_err(NULL, -32600, "request too large");
				skipping = true;
				in.n = scan = 0;
			}
			if (in.cap > (1u << 20) && in.n < 4096) {   /* shrink after a big write */
				Buf t = { 0 };
				buf_put(&t, in.p, in.n);
				buf_free(&in);
				in = t;
			}
		}
	wake:
		proc_wake();
	}
	proc_shutdown();
	return 0;
}
