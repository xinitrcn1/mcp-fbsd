# mcp-fbsd

A small MCP server in C11: files, search, shell sessions and images. One
58 KB binary (stripped, with png+jpeg), about 3 MB RSS when idle, one thread.

```
make && make check
./mcp-fbsd [-RXv] [-r root]... [-s shell]
```

| Flag | Effect |
| --- | --- |
| `-r DIR` | file tools only see paths under DIR (repeatable, symlinks resolved) |
| `-R` | read-only: write/edit/mv/mkdir/exec are not even listed |
| `-X` | no process tools |
| `-s SH` | shell for `exec` (default `/bin/sh`) |

`-r` limits the file tools. It is not a sandbox while `exec` is enabled, so
use `-X` or `-R` when that matters.

## Tools

| Tool | Does |
| --- | --- |
| `read` | text with offset/limit (negative = tail), or an image (png, jpg, gif, webp, ppm/pgm) |
| `write` | atomic replace, or `append`; creates parent dirs |
| `edit` | exact replace; `count` must match (0 = all); `edits[]` applied atomically |
| `ls` `stat` `mv` `mkdir` | the obvious |
| `search` | `glob` on names, `re` (POSIX ERE, or `fixed`) on content |
| `exec` | shell command; returns `[exit N]`, or `[running id=N]` after `timeout_ms` |
| `proc_write` `proc_read` `proc_kill` `proc_list` | drive a running session (`pty:true` for REPLs) |
| `img` | crop, rot, flip, resize/max, gray; returns the image or writes `.png/.jpg/.ppm/.pgm` |

## What it avoids

The things that make Desktop Commander expensive to use from an agent:

- **No tips, banners or onboarding text.** A successful write says `ok`.
  Partial reads end with one footer, `[lines A-B of N]`. Errors are one line.
- **Short tool list.** Descriptions are one sentence. The whole `tools/list`
  is about 5 KB, and tools turned off by `-R`/`-X` are not listed.
- **Process output is cleaned.** ANSI sequences are removed and `\r`
  progress lines collapse to the last one. When output is over 32 KB, the
  middle is cut and the head and tail are kept. Children get `TERM=dumb`,
  `NO_COLOR=1` and `PAGER=cat`, so nothing waits on a pager.
- **No telemetry, no network, no config file written at runtime.**

## Design

Data oriented, sized for small systems:

- **Event loop, no threads.** `poll()` covers stdin, the session pipes and
  a SIGCHLD self-pipe. A call that waits on a process (exec, proc_write,
  proc_read with a timeout) is parked as a 160-byte waiter and answered
  later, so a `sleep 60` never blocks other calls.
- **JSON in place.** A request is parsed into four parallel token arrays
  (type, offset, length, subtree end) that are reused across requests.
  Escapes are decoded in the request buffer itself, so string arguments,
  including a whole file for `write`, are never copied. Output is escaped
  straight into one response buffer, and invalid UTF-8 becomes U+FFFD.
- **Fixed budgets.** There are 16 session slots, each with a 256 KB output
  ring that is allocated once, and pages are only touched as output
  arrives. `read` streams through one static 64 KB chunk. `search` reuses
  one file buffer. Scratch buffers that grow past 1 MB are released after
  the call.
- **Images.** Each image is decoded to one RGBA8 buffer. JPEG is decoded at
  1/2, 1/4 or 1/8 scale when only a small view is needed. Downscaling uses
  an alpha-weighted area average. PNG output uses the fewest channels that
  lose nothing. Images are returned as JPEG when the source was JPEG.
  Netpbm needs no library, so `make IMG= IMGLIBS=` (64 KB) still handles
  ppm/pgm.

Limits are in `src/config.h`, which is copied from `config.def.h` on the
first build.

## Layout

```
src/main.c    protocol, tool table, event loop
src/json.c    parser and escaper
src/buf.c     growable buffer, base64
src/fs.c      read write edit ls stat mv mkdir, path checks
src/search.c  fts walk, fnmatch, regex
src/proc.c    sessions, rings, waiters, pty
src/img.c     png/jpeg/pnm codecs and transforms
tests/run.py  end-to-end tests over stdio
```
