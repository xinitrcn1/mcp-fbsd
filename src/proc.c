/* Process sessions. Each child runs in its own session/process group with one
 * output stream (pipe or pty) drained into a fixed ring. A call that has to
 * wait (exec, proc_write, proc_read with timeout) becomes a waiter: a small
 * record with a deadline, answered from the main loop when its condition is
 * met. No threads. */
#include "mcp.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

_Static_assert((CFG_RING & (CFG_RING - 1)) == 0, "CFG_RING must be a power of two");

#define EXIT_GRACE 200   /* ms to wait for output after exit if the pipe stays open */

typedef struct {
	uint64_t total;      /* bytes ever received */
	uint64_t rd;         /* bytes already returned */
	uint64_t t0, tlast, texit;
	char    *ring;
	pid_t    pid;
	uint32_t id;         /* 0 = free slot */
	int      fd;         /* output (pipe read end or pty master), -1 at EOF */
	int      in;         /* input: pipe write end or pty master, -1 closed */
	int      status;
	bool     exited, pty;
	char     cmd[40];
} Proc;

typedef struct {
	uint64_t deadline, mark;
	uint32_t sid, idle;  /* sid 0 = free */
	Id       id;
} Wait;

static Proc  P[CFG_PROCS];
static Wait  W[CFG_WAITERS];
static uint32_t g_next = 1;
static Buf   g_lin, g_clean;   /* scratch, reused */
static Res   g_wres;

void proc_init(void) { for (int i = 0; i < CFG_PROCS; i++) P[i].fd = P[i].in = -1; }

static Proc *find(uint32_t id)
{
	for (int i = 0; i < CFG_PROCS; i++) if (id && P[i].id == id) return &P[i];
	return NULL;
}

static bool done(const Proc *s, uint64_t now)
{
	return s->exited && (s->fd < 0 || now - s->texit >= EXIT_GRACE);
}

static void release(Proc *s)
{
	if (s->fd >= 0) close(s->fd);
	if (s->in >= 0 && s->in != s->fd) close(s->in);
	free(s->ring);
	memset(s, 0, sizeof *s);
	s->fd = s->in = -1;
}

static void close_out(Proc *s)
{
	if (s->fd < 0) return;
	if (s->in == s->fd) s->in = -1;
	close(s->fd);
	s->fd = -1;
}

/* ---- loop hooks ---- */

int proc_pollfds(struct pollfd *pf, int max)
{
	int n = 0;
	for (int i = 0; i < CFG_PROCS && n < max; i++)
		if (P[i].id && P[i].fd >= 0) pf[n++] = (struct pollfd){ P[i].fd, POLLIN, 0 };
	return n;
}

static void ring_put(Proc *s, const char *p, size_t n)
{
	if (n > CFG_RING) { s->total += n - CFG_RING; p += n - CFG_RING; n = CFG_RING; }
	size_t o = (size_t)(s->total & (CFG_RING - 1)), k = CFG_RING - o;
	if (k > n) k = n;
	memcpy(s->ring + o, p, k);
	memcpy(s->ring, p + k, n - k);
	s->total += n;
}

void proc_pump(const struct pollfd *pf, int n)
{
	char tmp[16384];
	for (int i = 0; i < n; i++) {
		if (!pf[i].revents) continue;
		Proc *s = NULL;
		for (int k = 0; k < CFG_PROCS; k++) if (P[k].id && P[k].fd == pf[i].fd) s = &P[k];
		if (!s) continue;
		for (int rounds = 0; rounds < 16; rounds++) {   /* bounded: stay fair */
			ssize_t r = read(s->fd, tmp, sizeof tmp);
			if (r > 0) { ring_put(s, tmp, (size_t)r); s->tlast = now_ms(); continue; }
			if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
			close_out(s);                 /* EOF, or EIO on a pty whose slave closed */
			break;
		}
	}
}

void proc_reap(void)
{
	int st;
	pid_t pid;
	while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
		for (int i = 0; i < CFG_PROCS; i++) {
			if (P[i].id && P[i].pid == pid) {
				P[i].exited = true;
				P[i].status = st;
				P[i].texit = now_ms();
			}
		}
	}
}

bool proc_waiting(void)
{
	for (int i = 0; i < CFG_WAITERS; i++) if (W[i].sid) return true;
	return false;
}

static uint64_t due(const Wait *w, const Proc *s)
{
	uint64_t t = w->deadline;
	if (s->exited && s->fd >= 0 && s->texit + EXIT_GRACE < t) t = s->texit + EXIT_GRACE;
	if (w->idle && s->total > w->mark && s->tlast + w->idle < t) t = s->tlast + w->idle;
	return t;
}

int proc_timeout(void)
{
	uint64_t now = now_ms(), best = UINT64_MAX;
	for (int i = 0; i < CFG_WAITERS; i++) {
		if (!W[i].sid) continue;
		Proc *s = find(W[i].sid);
		uint64_t t = s ? due(&W[i], s) : now;
		if (t < best) best = t;
	}
	if (best == UINT64_MAX) return -1;
	return best <= now ? 0 : (int)(best - now);
}

/* ---- output formatting ---- */

/* Strip terminal control sequences; apply \r and \b like a terminal would. */
static void clean(Buf *o, const char *p, size_t n)
{
	size_t ls = o->n;    /* start of current line in o */
	buf_reserve(o, n);
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)p[i];
		if (c == 0x1b) {
			if (i + 1 >= n) break;
			char t = p[++i];
			if (t == '[') {                      /* CSI ... final byte */
				while (++i < n && !((unsigned char)p[i] >= 0x40 && (unsigned char)p[i] <= 0x7e)) {}
			} else if (t == ']' || t == 'P' || t == '_') {   /* OSC/DCS: to BEL or ST */
				while (++i < n && p[i] != 7 && !(p[i] == 0x1b && i + 1 < n && p[i + 1] == '\\')) {}
				if (i < n && p[i] == 0x1b) i++;
			} else if (t == '(' || t == ')' || t == '#') {
				i++;
			}
			continue;
		}
		if (c == '\r') {
			if (i + 1 < n && p[i + 1] == '\n') continue;
			if (i + 1 < n) o->n = ls;           /* overwrite the line */
			continue;
		}
		if (c == '\b') { if (o->n > ls) o->n--; continue; }
		if (c < 0x20 && c != '\n' && c != '\t') continue;
		o->p[o->n++] = (char)c;
		if (c == '\n') ls = o->n;
	}
	o->p[o->n] = 0;
}

static void emit(Proc *s, Buf *out, uint64_t now)
{
	uint64_t from = s->rd, dropped = 0;
	if (s->total - from > CFG_RING) { dropped = s->total - CFG_RING - from; from = s->total - CFG_RING; }
	size_t n = (size_t)(s->total - from);
	buf_clear(&g_lin);
	buf_reserve(&g_lin, n);
	size_t o = (size_t)(from & (CFG_RING - 1)), k = CFG_RING - o;
	if (k > n) k = n;
	memcpy(g_lin.p, s->ring + o, k);
	memcpy(g_lin.p + k, s->ring, n - k);
	g_lin.n = n;
	s->rd = s->total;

	buf_clear(&g_clean);
	clean(&g_clean, g_lin.p, g_lin.n);
	if (dropped) buf_printf(out, "[%llu bytes dropped]\n", (unsigned long long)dropped);
	if (g_clean.n > CFG_PROC_OUT) {
		size_t h = CFG_PROC_OUT / 4, t = CFG_PROC_OUT - h;
		buf_put(out, g_clean.p, h);
		buf_printf(out, "\n[... %zu bytes elided ...]\n", g_clean.n - h - t);
		buf_put(out, g_clean.p + g_clean.n - t, t);
	} else {
		buf_put(out, g_clean.p, g_clean.n);
	}
	if (out->n && out->p[out->n - 1] != '\n') buf_putc(out, '\n');
	if (done(s, now)) {
		if (WIFSIGNALED(s->status)) buf_printf(out, "[signal %d]", WTERMSIG(s->status));
		else buf_printf(out, "[exit %d]", WEXITSTATUS(s->status));
		release(s);
	} else {
		buf_printf(out, "[running id=%u]", s->id);
	}
	if (g_lin.cap > (1u << 20)) buf_free(&g_lin);
	if (g_clean.cap > (1u << 20)) buf_free(&g_clean);
}

void proc_wake(void)
{
	uint64_t now = now_ms();
	for (int i = 0; i < CFG_WAITERS; i++) {
		Wait *w = &W[i];
		if (!w->sid) continue;
		Proc *s = find(w->sid);
		if (!s) {
			err(&g_wres, "session %u gone", w->sid);
			reply_tool(&w->id, &g_wres, true);
		} else if (done(s, now) || now >= due(w, s)) {
			buf_clear(&g_wres.out);
			emit(s, &g_wres.out, now);
			reply_tool(&w->id, &g_wres, false);
		} else {
			continue;
		}
		w->sid = 0;
	}
}

static int park(Res *r, const Id *id, Proc *s, long long timeout, long long idle, uint64_t mark)
{
	if (timeout < 0) timeout = 0;
	if (timeout > CFG_MAX_MS) timeout = CFG_MAX_MS;
	if (idle < 0) idle = 0;
	for (int i = 0; i < CFG_WAITERS; i++) {
		if (W[i].sid) continue;
		W[i] = (Wait){ now_ms() + (uint64_t)timeout, mark, s->id, (uint32_t)idle, *id };
		return T_DEFER;
	}
	return err(r, "too many pending calls");
}

/* ---- tools ---- */

static int write_in(Proc *s, const char *p, size_t n)
{
	uint64_t end = now_ms() + 2000;
	while (n) {
		ssize_t k = write(s->in, p, n);
		if (k > 0) { p += k; n -= (size_t)k; continue; }
		if (k < 0 && errno == EINTR) continue;
		if (k < 0 && errno == EAGAIN && now_ms() < end) {
			struct pollfd pf = { s->in, POLLOUT, 0 };
			poll(&pf, 1, 50);
			continue;
		}
		return -1;
	}
	return 0;
}

static void close_in(Proc *s)
{
	if (s->in < 0) return;
	if (s->pty) { (void)!write(s->in, "\x04", 1); return; }   /* VEOF */
	close(s->in);
	s->in = -1;
}

int t_exec(const Json *j, uint32_t a, Res *r, const Id *id)
{
	size_t cl;
	const char *cmd = arg_str(j, a, "cmd", &cl);
	if (!cmd || !cl) return err(r, "cmd required");
	char cwd[PATH_MAX];
	const char *cwd_in = arg_str(j, a, "cwd", NULL);
	if (cwd_in && path_resolve(r, cwd_in, cwd, true)) return T_ERR;
	bool pty = arg_bool(j, a, "pty", 0);

	Proc *s = NULL;
	for (int i = 0; i < CFG_PROCS && !s; i++) if (!P[i].id) s = &P[i];
	if (!s) {   /* evict the oldest finished session */
		uint64_t now = now_ms();
		for (int i = 0; i < CFG_PROCS; i++)
			if (done(&P[i], now) && (!s || P[i].t0 < s->t0)) s = &P[i];
		if (!s) return err(r, "%d sessions running; proc_kill one", CFG_PROCS);
		for (int i = 0; i < CFG_WAITERS; i++) if (W[i].sid == s->id) return err(r, "sessions busy");
		release(s);
	}

	int out[2] = { -1, -1 }, in[2] = { -1, -1 }, m = -1;
	char *slave = NULL;
	if (pty) {
		m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (m < 0 || grantpt(m) || unlockpt(m) || !(slave = ptsname(m))) {
			if (m >= 0) close(m);
			return errno_err(r, "pty");
		}
	} else if (pipe(out) || pipe(in)) {
		int e = errno;
		for (int i = 0; i < 2; i++) { if (out[i] >= 0) close(out[i]); if (in[i] >= 0) close(in[i]); }
		errno = e;
		return errno_err(r, "pipe");
	}

	pid_t pid = fork();
	if (pid < 0) {
		int e = errno;
		if (pty) close(m); else { close(out[0]); close(out[1]); close(in[0]); close(in[1]); }
		errno = e;
		return errno_err(r, "fork");
	}
	if (pid == 0) {
		setsid();
		if (pty) {
			int sl = open(slave, O_RDWR);
			if (sl < 0) _exit(126);
			ioctl(sl, TIOCSCTTY, 0);
			struct termios tio;
			if (!tcgetattr(sl, &tio)) {
				tio.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
				tio.c_oflag &= ~(tcflag_t)ONLCR;
				tcsetattr(sl, TCSANOW, &tio);
			}
			struct winsize ws = { 50, 160, 0, 0 };
			ioctl(sl, TIOCSWINSZ, &ws);
			dup2(sl, 0); dup2(sl, 1); dup2(sl, 2);
			if (sl > 2) close(sl);
			close(m);
		} else {
			dup2(in[0], 0); dup2(out[1], 1); dup2(out[1], 2);
			close(in[0]); close(in[1]); close(out[0]); close(out[1]);
		}
		if (cwd_in && chdir(cwd)) _exit(126);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		setenv("TERM", "dumb", 1);
		setenv("PAGER", "cat", 1);
		setenv("GIT_PAGER", "cat", 1);
		setenv("NO_COLOR", "1", 1);
		execl(g_cfg.shell, g_cfg.shell, "-c", cmd, (char *)NULL);
		_exit(127);
	}

	memset(s, 0, sizeof *s);
	s->id = g_next++;
	if (!g_next) g_next = 1;
	s->pid = pid;
	s->pty = pty;
	s->t0 = s->tlast = now_ms();
	s->ring = xmalloc(CFG_RING);        /* pages are touched only as output arrives */
	snprintf(s->cmd, sizeof s->cmd, "%s", cmd);
	for (char *c = s->cmd; *c; c++) if (*c == '\n' || *c == '\t') *c = ' ';
	if (pty) {
		s->fd = s->in = m;
	} else {
		close(in[0]); close(out[1]);
		s->fd = out[0]; s->in = in[1];
		fcntl(s->in, F_SETFD, FD_CLOEXEC);
		fcntl(s->fd, F_SETFD, FD_CLOEXEC);
	}
	fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
	if (s->in != s->fd) fcntl(s->in, F_SETFL, fcntl(s->in, F_GETFL) | O_NONBLOCK);

	size_t inl;
	const char *stdin_s = arg_str(j, a, "stdin", &inl);
	if (stdin_s) {
		write_in(s, stdin_s, inl);
		close_in(s);
	}
	return park(r, id, s, arg_int(j, a, "timeout_ms", CFG_EXEC_MS), arg_int(j, a, "idle_ms", 0), 0);
}

static Proc *get(const Json *j, uint32_t a, Res *r)
{
	long long id = arg_int(j, a, "id", 0);
	Proc *s = find(id > 0 && id <= UINT32_MAX ? (uint32_t)id : 0);
	if (!s) err(r, "no session %lld", id);
	return s;
}

int t_pwrite(const Json *j, uint32_t a, Res *r, const Id *id)
{
	Proc *s = get(j, a, r);
	if (!s) return T_ERR;
	size_t n = 0;
	const char *in = arg_str(j, a, "input", &n);
	if (in && s->in < 0) return err(r, "session %u: stdin closed", s->id);
	if (in) {
		if (write_in(s, in, n) || (arg_bool(j, a, "nl", 1) && write_in(s, "\n", 1)))
			return errno_err(r, "write");
	}
	if (arg_bool(j, a, "eof", 0)) close_in(s);
	return park(r, id, s, arg_int(j, a, "timeout_ms", 5000), arg_int(j, a, "idle_ms", 300), s->total);
}

int t_pread(const Json *j, uint32_t a, Res *r, const Id *id)
{
	Proc *s = get(j, a, r);
	if (!s) return T_ERR;
	long long to = arg_int(j, a, "timeout_ms", 0);
	if (to > 0 && s->total == s->rd && !done(s, now_ms()))
		return park(r, id, s, to, arg_int(j, a, "idle_ms", 300), s->rd);
	buf_clear(&r->out);
	emit(s, &r->out, now_ms());
	return T_OK;
}

int t_pkill(const Json *j, uint32_t a, Res *r, const Id *id)
{
	(void)id;
	Proc *s = get(j, a, r);
	if (!s) return T_ERR;
	static const struct { const char *n; int s; } sigs[] = {
		{ "TERM", SIGTERM }, { "KILL", SIGKILL }, { "INT", SIGINT }, { "HUP", SIGHUP },
		{ "QUIT", SIGQUIT }, { "STOP", SIGSTOP }, { "CONT", SIGCONT }, { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 },
	};
	int sig = SIGTERM;
	const char *sn = arg_str(j, a, "sig", NULL);
	if (sn) {
		if (!strncmp(sn, "SIG", 3)) sn += 3;
		sig = 0;
		for (size_t i = 0; i < countof(sigs); i++) if (!strcmp(sn, sigs[i].n)) sig = sigs[i].s;
		if (!sig) sig = atoi(sn);
		if (sig <= 0) return err(r, "bad signal");
	} else {
		long long v = arg_int(j, a, "sig", SIGTERM);
		sig = (int)v;
	}
	if (s->exited) return err(r, "session %u already exited", s->id);
	if (kill(-s->pid, sig) && kill(s->pid, sig)) return errno_err(r, "kill");
	return T_OK;
}

int t_plist(const Json *j, uint32_t a, Res *r, const Id *id)
{
	(void)j; (void)a; (void)id;
	uint64_t now = now_ms();
	buf_clear(&r->out);
	for (int i = 0; i < CFG_PROCS; i++) {
		Proc *s = &P[i];
		if (!s->id) continue;
		char st[24];
		if (!s->exited) snprintf(st, sizeof st, "running");
		else if (WIFSIGNALED(s->status)) snprintf(st, sizeof st, "signal %d", WTERMSIG(s->status));
		else snprintf(st, sizeof st, "exit %d", WEXITSTATUS(s->status));
		buf_printf(&r->out, "%u %d %s %llus%s %s\n", s->id, (int)s->pid, st,
		           (unsigned long long)((now - s->t0) / 1000),
		           s->total > s->rd ? " +out" : "", s->cmd);
	}
	if (!r->out.n) buf_puts(&r->out, "(none)");
	return T_OK;
}

void proc_shutdown(void)
{
	for (int i = 0; i < CFG_PROCS; i++)
		if (P[i].id && !P[i].exited) kill(-P[i].pid, SIGHUP);
}
