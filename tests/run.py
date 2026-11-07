#!/usr/bin/env python3
"""Drive the server over stdio and check each tool. Usage: run.py ./mcp-fbsd"""
import base64, json, os, subprocess, sys, tempfile, time, zlib, struct

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./mcp-fbsd")
fails = 0


class Srv:
    def __init__(self, *args):
        self.p = subprocess.Popen([BIN, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.n = 0
        self.pending = {}

    def send(self, method, params=None):
        self.n += 1
        msg = {"jsonrpc": "2.0", "id": self.n, "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write((json.dumps(msg) + "\n").encode())
        self.p.stdin.flush()
        return self.n

    def wait(self, i):
        while i not in self.pending:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("server closed")
            m = json.loads(line)
            self.pending[m["id"]] = m
        return self.pending.pop(i)

    def call(self, name, **args):
        m = self.wait(self.send("tools/call", {"name": name, "arguments": args}))
        return m

    def text(self, name, **args):
        m = self.call(name, **args)
        if "error" in m:
            return "RPCERR " + m["error"]["message"]
        r = m["result"]
        t = "".join(c.get("text", "") for c in r["content"])
        return ("ERR " if r.get("isError") else "") + t

    def close(self):
        self.p.stdin.close()
        return self.p.wait(5)


def check(name, cond, got=None):
    global fails
    if cond:
        print("ok  ", name)
    else:
        fails += 1
        print("FAIL", name, "->", repr(got)[:400])


def png_bytes(w, h):
    rows = b"".join(b"\0" + b"".join(bytes([x * 255 // max(w - 1, 1), y * 255 // max(h - 1, 1), 128])
                                     for x in range(w)) for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def main():
    d = tempfile.mkdtemp(prefix="mcpt")
    s = Srv()

    m = s.wait(s.send("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                      "clientInfo": {"name": "t", "version": "0"}}))
    check("initialize", m["result"]["serverInfo"]["name"] == "mcp-fbsd", m)
    s.p.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
    m = s.wait(s.send("tools/list"))
    names = [t["name"] for t in m["result"]["tools"]]
    check("tools/list", "exec" in names and "img" in names, names)
    print("     tools/list is", len(json.dumps(m["result"])), "bytes")
    m = s.wait(s.send("nope"))
    check("unknown method", m["error"]["code"] == -32601, m)
    s.p.stdin.write(b'{bad json\n'); s.p.stdin.flush()
    m = json.loads(s.p.stdout.readline())
    check("parse error", m["error"]["code"] == -32700, m)

    f = os.path.join(d, "sub/dir/a.txt")
    check("write creates parents", s.text("write", path=f, content="one\ntwo\nthree\n") == "ok")
    check("read", s.text("read", path=f) == "one\ntwo\nthree\n", s.text("read", path=f))
    t = s.text("read", path=f, offset=2, limit=1, n=True)
    check("read range", t == "2\ttwo\n[lines 2-2 of 3]\n", t)
    t = s.text("read", path=f, offset=-1)
    check("read tail", t == "three\n[lines 3-3 of 3]\n", t)
    check("append", s.text("write", path=f, content="four\n", append=True) == "ok")
    check("edit", s.text("edit", path=f, old="two", new="TWO") == "ok")
    t = s.text("edit", path=f, old="  three", new="x")
    check("edit miss hint", t.startswith("ERR old not found; first line found at line 3"), t)
    check("edit multi", s.text("edit", path=f, edits=[{"old": "one", "new": "1"},
                                                      {"old": "four", "new": "4"}]) == "ok")
    t = s.text("read", path=f)
    check("edit result", t == "1\nTWO\nthree\n4\n", t)
    t = s.text("edit", path=f, edits=[{"old": "1", "new": "x"}, {"old": "zzz", "new": "y"}])
    check("edit atomic", t.startswith("ERR edit 2") and s.text("read", path=f).startswith("1\n"), t)
    u = os.path.join(d, "u.txt")
    s.text("write", path=u, content="héllo 😀 \x01 \"q\" \\ end")
    t = s.text("read", path=u)
    check("unicode round trip", t == "héllo 😀 \x01 \"q\" \\ end\n" or
          t == "héllo 😀 \x01 \"q\" \\ end", t)
    with open(os.path.join(d, "bad.txt"), "wb") as fh:
        fh.write(b"ok \xff\xfe bytes\n")
    t = s.text("read", path=os.path.join(d, "bad.txt"))
    check("invalid utf-8 repaired", t == "ok �� bytes\n", t)
    with open(os.path.join(d, "long.txt"), "w") as fh:
        fh.write("x" * 200000 + "\nnext\n")
    t = s.text("read", path=os.path.join(d, "long.txt"))
    check("long line cut", t.endswith("...[+198000]\nnext\n"), t[-60:])

    t = s.text("ls", path=d, depth=2)
    check("ls", "sub/\n  dir/\n" in t and "u.txt " in t, t)
    t = s.text("stat", path=f)
    check("stat", " file " in t and "mode 0" in t, t)
    check("mv", s.text("mv", **{"from": u, "to": os.path.join(d, "v.txt")}) == "ok")
    check("mkdir", s.text("mkdir", path=os.path.join(d, "m/n/o")) == "ok" and
          os.path.isdir(os.path.join(d, "m/n/o")))
    t = s.text("read", path=os.path.join(d, "nope"))
    check("read missing", t.startswith("ERR") and "No such file" in t, t)

    t = s.text("search", path=d, glob="*.txt")
    check("search glob", "a.txt" in t and "v.txt" in t, t)
    t = s.text("search", path=d, re="TW.", glob="*.txt")
    check("search re", t.strip().endswith("a.txt:2:TWO"), t)
    t = s.text("search", path=d, re="two", i=True, fixed=True)
    check("search fixed icase", ":2:TWO" in t, t)

    t = s.text("exec", cmd="echo hi; echo err >&2; exit 3")
    check("exec", t == "hi\nerr\n[exit 3]", t)
    t = s.text("exec", cmd="printf 'a\\033[31mred\\033[0m\\rb\\n'")
    check("exec strips ansi/cr", t == "b\n[exit 0]", t)   # lone \r restarts the line
    t = s.text("exec", cmd="cat", stdin="fed\n")
    check("exec stdin", t == "fed\n[exit 0]", t)
    t = s.text("exec", cmd="pwd", cwd=d)
    check("exec cwd", os.path.realpath(d) in t, t)
    t = s.text("exec", cmd="echo start; sleep 30", timeout_ms=300)
    check("exec timeout", t.startswith("start\n[running id="), t)
    sid = int(t.split("id=")[1].rstrip("]"))
    check("proc_list", f"{sid} " in s.text("proc_list"))
    check("proc_kill", s.text("proc_kill", id=sid) == "ok")
    t = s.text("proc_read", id=sid, timeout_ms=2000)
    check("killed status", t.endswith("[signal 15]"), t)

    # concurrency: a slow call must not block a fast one
    t0 = time.time()
    a = s.send("tools/call", {"name": "exec", "arguments": {"cmd": "sleep 1; echo slow"}})
    b = s.send("tools/call", {"name": "exec", "arguments": {"cmd": "echo fast"}})
    mb = s.wait(b)
    fast = time.time() - t0
    ma = s.wait(a)
    check("concurrent", fast < 0.5 and "fast" in mb["result"]["content"][0]["text"]
          and "slow" in ma["result"]["content"][0]["text"], fast)

    t = s.text("exec", cmd="python3 -i -q", pty=True, idle_ms=500, timeout_ms=5000)
    check("pty repl start", "[running id=" in t, t)
    sid = int(t.split("id=")[1].rstrip("]"))
    t = s.text("proc_write", id=sid, input="print(6*7)")
    check("pty repl io", "42" in t, t)
    t = s.text("proc_write", id=sid, input="exit()", timeout_ms=3000)
    check("pty repl exit", t.endswith("[exit 0]"), t)

    t = s.text("exec", cmd="head -c 300000 /dev/zero | tr '\\0' x; echo; echo tail")
    check("output elided", "bytes elided" in t and t.endswith("tail\n[exit 0]"), t[-80:])

    # images
    pf = os.path.join(d, "g.png")
    with open(pf, "wb") as fh:
        fh.write(png_bytes(64, 32))
    m = s.call("read", path=pf)
    c = m["result"]["content"]
    check("read png", c[0]["type"] == "image" and c[0]["mimeType"] == "image/png", c[1:])
    t = s.text("img", path=pf, info=True)
    check("img info", t == "png 64x32", t)
    o = os.path.join(d, "o.png")
    t = s.text("img", path=pf, out=o, crop=[0, 0, 32, 32], rot=90, w=16)
    check("img crop/rot/resize", t == "16x16", t)
    t = s.text("img", path=o, info=True)
    check("img out readable", t == "png 16x16", t)
    oj = os.path.join(d, "o.jpg")
    t = s.text("img", path=pf, out=oj, gray=True, q=70)
    check("img jpeg out", t == "64x32" and open(oj, "rb").read(3) == b"\xff\xd8\xff", t)
    m = s.call("img", path=oj, max=16)
    c = m["result"]["content"]
    check("img view jpeg scaled", c[0]["mimeType"] == "image/jpeg" and c[1]["text"] == "16x8", c[1:])
    big = os.path.join(d, "big.png")
    with open(big, "wb") as fh:
        fh.write(png_bytes(2000, 100))
    m = s.call("read", path=big)
    c = m["result"]["content"]
    check("read big png downscaled", c[1]["text"] == "1568x78 (from 2000x100)", c[1:])

    # netpbm
    pp = os.path.join(d, "a.ppm")
    with open(pp, "wb") as fh:
        fh.write(b"P6\n# c\n4 2\n255\n" + bytes(range(24)))
    t = s.text("img", path=pp, info=True)
    check("ppm info", t == "ppm 4x2", t)
    m = s.call("read", path=pp)
    c = m["result"]["content"]
    check("read ppm as image", c[0]["type"] == "image" and c[1]["text"].startswith("4x2"), c[1:])
    pa = os.path.join(d, "a.pgm")
    with open(pa, "w") as fh:
        fh.write("P2\n3 1\n15\n0 15 7\n")
    t = s.text("img", path=pa, out=os.path.join(d, "b.ppm"), w=6)
    data = open(os.path.join(d, "b.ppm"), "rb").read()
    check("pgm ascii -> ppm out", t == "6x2" and data.startswith(b"P5\n6 2\n255\n")   # aspect kept
          and data[-6:] == bytes([0, 0, 255, 255, 119, 119]), (t, data))
    t = s.text("img", path=pf, out=os.path.join(d, "c.ppm"))
    check("png -> ppm", t == "64x32" and open(os.path.join(d, "c.ppm"), "rb").read(2) == b"P6", t)
    with open(os.path.join(d, "bad.ppm"), "wb") as fh:
        fh.write(b"P6\n4 2\n255\n" + bytes(5))
    t = s.text("img", path=os.path.join(d, "bad.ppm"), info=True)
    check("ppm truncated", t == "ERR pnm: truncated", t)

    rc = s.close()
    check("clean exit", rc == 0, rc)

    # read-only mode: write tools hidden, roots enforced
    s = Srv("-R", "-r", d)
    m = s.wait(s.send("tools/list"))
    names = [t["name"] for t in m["result"]["tools"]]
    check("-R hides tools", "write" not in names and "exec" not in names and "read" in names, names)
    t = s.text("read", path="/etc/passwd")
    check("-r enforced", t.endswith("outside allowed roots"), t)
    t = s.text("read", path=os.path.join(d, "../../etc/passwd"))
    check("-r with ..", t.startswith("ERR"), t)
    s.close()

    print("FAILED %d" % fails if fails else "all passed")
    sys.exit(1 if fails else 0)


main()
