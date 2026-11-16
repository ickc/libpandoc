# A fuzzer of libpandoc's C interface

`fuzz.c` makes random calls to a built libpandoc and checks what the header
promises; `run.sh` runs it in child processes on several cores for as long
as asked and keeps what fails. It runs for hours, so it is not part of CI;
nor is a short run: its children meet pandoc's slow inputs (hangs) and the
memory cap at random, so it would fail at random.

```sh
bash scripts/build.sh dist                       # or any staged prefix
fuzz/run.sh -j 10 -m 60 dist /tmp/fuzz-out       # 10 children at a time, 60 minutes
cat /tmp/fuzz-out/log                            # one line per failing child, and a summary
```

Its options (at the top of `run.sh`): `-n` iterations per child (100),
`-t` a child's time limit (600 s), `-v` its memory cap (`ulimit -v`, 8 GB),
`-T` how often a child runs untrusted under `strace` (every 20th).

## What it does

Each iteration has its own random state (from the seed and its number), so
one can be made again alone: `fuzz SEED 1 K` (`fuzz -D SEED 1 K` writes its
inputs to files instead). An iteration is one call, or 2 to 6 threads making
a few at once. The calls: `pandoc_convert` and `pandoc_convert_filters`,
`pandoc_read_many`, `pandoc_query`, `pandoc_convert_args`, `pandoc_main`,
`pandoc_set_num_threads`.

What pandoc offers is asked of it at start, not listed by hand: its readers
and writers (queries), its options and their types
(`pandoc.cli.default_options`, through a Lua filter; plus the keys whose
default is null, which a Lua table can't show), and a small valid document
per reader (a sample written by the writer of the same name).

- Options: real keys with values of the right type, the wrong one, null,
  huge strings, deep arrays, odd numbers, invalid UTF-8; unknown and
  repeated keys; garbage instead of JSON. `"untrusted"` and `"sandbox"` on,
  off or wrong; `"timeout"` mostly set (always with a hostile document).
- Documents: random bytes, mutations of the valid ones, splices, the
  review's hostile inputs (nested quotes, links, lists, tables, Typst
  loops, deep JSON, ...), documents naming a canary's files (LaTeX
  `\input`, RST `include`, Org `#+INCLUDE`, Typst `#read`, XInclude,
  metadata bibliographies with citeproc).
- Callback filters that answer the document, a truncated or huge one,
  random bytes, deep JSON, nothing, or fail; or call pandoc again.
- Cancel tokens (1.13): a quarter of the conversions and `read_many`
  calls get one, which another thread cancels (or frees, or leaves) after
  up to 0.8 s.
- A canary: files, and a user data directory (`HOME`, `XDG_DATA_HOME`) with
  templates and translations, whose content must never come out of an
  untrusted call.

## What it checks

- Every call returns a result (not NULL); `status` is 0 exactly when
  `error_kind` is NULL; `output` is NUL-terminated at `output_len`; `log`
  is JSON and UTF-8; an error has a non-empty kind and a non-empty UTF-8
  message; a success has no message; the `json` writer's output is JSON;
  `read_many`'s and queries' output is JSON; a callback is given JSON.
- A call with a time limit and no Lua or callback filter returns within
  the limit and 30 s.
- A cancelled call with no Lua or callback filter returns within 30 s of
  the cancel; a call fails with `"Cancelled"` only if its token was
  cancelled; `pandoc_cancel` answers 0 for a token, -1 once it is freed.
- Untrusted calls: no canary content in output, message or log; and in
  the traced children (all calls untrusted), no canary file opened.
- At a child's end: no temporary directory left in `TMPDIR`, no
  descriptors leaking.
- The process neither exits nor crashes; an iteration over 90 s is a hang.

Classes in `OUT/found/`: `violation`, `crash`, `exit-N`, `hang`,
`canary-open`, and `oom`: running out of memory under the cap ends the
process by design (exit 251; or exit 1 when no thread could be made; or
Lua's "not enough memory" panic), kept apart so it buries nothing.

## What it found (2026-11-15, 75 minutes on 10 cores against batch 3)

9,456 children, 1.78 million calls in those that ended normally.

| finding | class | whose | status |
|---|---|---|---|
| a JSON filter exiting without reading its input (`false`, or stopped by the time limit) ended a host that leaves SIGPIPE at its default | crash (141), 52 children | libpandoc | fixed (1.12), `test/edge.c` |
| an error naming a file whose name has a NUL (`"log-file": "\u0000"`) had an empty message | violation | libpandoc | fixed (1.12), `test/edge.c` |
| `<q` as HTML: `PandocParseError` with an empty message | violation | pandoc | upstream: `printf '<q' \| pandoc -f html` |
| `"timeout"` doesn't stop the muse writer on a run of `{` (a loop that doesn't allocate; 40 `{`: 2.5 s, x1.6 each) | hang | pandoc (writer), GHC (interruptible only where code allocates) | upstream: `printf '%*s\n' 40 '' \| tr ' ' '{' \| pandoc -t muse` |
| markdown's `citations`: unclosed `[` are exponential (18 `[`: 2.3 s) | hang | pandoc | upstream: `printf '[%.0s' {1..18} \| pandoc` |
| dokuwiki: unclosed `((`, `<sup>`, `<del>` are exponential (16 `((`: 24 s) | hang | pandoc | upstream: `printf '((%.0s' {1..16} \| pandoc -f dokuwiki` |
| after `pandoc_main` reads standard input, descriptor 0 is closed and taken by the runtime's next file (epoll): `pandoc lua` then reads that and hangs | hang | libpandoc | fixed (1.13: descriptor 0 is /dev/null then), `test/edge.c` |
| out of memory under the cap: Lua's panic (abort), and `hsluaL_newstate` using a NULL `lua_State` (SIGSEGV) | oom, crash (139) | hslua | upstream: check `luaL_newstate`'s result |
| the same file as `"log-file"` and `"output-file"`: the result's log was the output, not JSON (2026-11-16, against 1.13) | violation | libpandoc | fixed (1.13: the log is `[]` then), `test/edge.c` |

Then 20 minutes against the fixes (2,706 children, 517,006 calls): no
SIGPIPE crash, no NUL violation; the `<q` violation again, and one Lua
memory panic (now counted as oom).

Most hangs are calls that can have no time limit (`pandoc_convert_args`,
`pandoc_main`, `read_many` without options) on inputs that pandoc's readers
take minutes on; the fuzzer gives those at most 512 bytes. Another slow
markdown input without `citations` (from 5 kB of random bytes) was not
minimized yet (833 bytes, over 3 s).

Not covered: `pandoc_shutdown`, libpandoc.wasm, Windows and macOS, PDF
output, URLs (nothing is fetched).
