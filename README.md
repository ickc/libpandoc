# libpandoc

[pandoc](https://pandoc.org) as a C library: a shared library with a small,
stable C ABI (`include/libpandoc.h`) for converting documents in process,
without running the `pandoc` executable.

```c
#include <libpandoc.h>

const char *opts = "{\"from\": \"markdown\", \"to\": \"html\"}";
pandoc_result *r = pandoc_convert(opts, strlen(opts), "*hi*", 4);
if (r->status == 0)
    fwrite(r->output, 1, r->output_len, stdout);   /* <p><em>hi</em></p> */
else
    fprintf(stderr, "%s: %s\n", r->error_kind, r->error_message);
pandoc_result_free(r);
```

Bindings: [libpandoc-python](https://github.com/ickc/libpandoc-python)
(with pandocpy), [libpandoc-rs](https://github.com/ickc/libpandoc-rs)
(with pandocrs), [LibPandoc.jl](https://github.com/ickc/LibPandoc.jl), and
JavaScript on libpandoc.wasm ([`wasm/`](wasm/README.md)).

## The ABI

| Function | Does |
|---|---|
| `pandoc_convert(options, input)` | a conversion; `options` is a JSON object in [defaults-file](https://pandoc.org/MANUAL.html#defaults-files) format, `input` is stdin |
| `pandoc_convert_args(argc, argv, input)` | the same with command-line arguments, parsed by pandoc itself |
| `pandoc_convert_filters(options, input, filters, n)` | a conversion with filters implemented by the caller, run in process (below) |
| `pandoc_convert_args_filters(argc, argv, input, filters, n)` | the same with command-line arguments |
| `pandoc_buffer_set(out, data, len)` | a filter's answer |
| `pandoc_read_many(request)` | many texts read at once, each on its own, in parallel: for filters parsing many fragments |
| `pandoc_query(query)` | formats, extensions, templates, versions, what command-line arguments ask for, as JSON |
| `pandoc_main(argc, argv, filters_json, filters, n)` | the `pandoc` command in process (standard input and output, pandoc's exit status), `pandoc lua` too |
| `pandoc_result_free(r)` | frees a result: output bytes, typed error, and pandoc's log as JSON |
| `pandoc_set_num_threads(n)` | the number of threads pandoc grows to (below) |
| `pandoc_init()`, `pandoc_shutdown()` | start the runtime (implicit), stop it (at the end, optional) |
| `pandoc_abi_version()` | the loaded library's interface version |

The Haskell runtime starts on first use, without installing signal
handlers, and every Haskell exception becomes an error result (or
`pandoc_main`'s status): a pandoc error, a bad option or argument, or a
broken pipe doesn't end the host process (SIGPIPE, from a JSON filter or
another program pandoc writes to that exits early, is blocked in the
threads that run pandoc, 1.12: at `pandoc_init`). What can still end it: running out
of memory (the runtime can't recover from that inside a library, and there
is no memory limit per call: below), and a Lua filter's or script's
`os.exit`; and what stays the host's (Lua's `print` writes to standard
output, standard output and error must be open, a callback must return
rather than unwind) is listed at the top of the header. Calls may be made
from any thread, concurrently, and run in parallel: pandoc runs on one
thread per logical core the process may use (its CPU affinity), or
`LIBPANDOC_NUM_THREADS` (like `OMP_NUM_THREADS`), or what
`pandoc_set_num_threads` says, never more than one per logical core the
process may use (more help nobody). Threads beyond the first start once,
when pandoc first runs something in parallel (a call while another
thread's call runs, or `pandoc_read_many` with several inputs), so a
program converting one document at a time doesn't pay for them. The number
is fixed from then on: `pandoc_set_num_threads` sets it only before (from
any thread, also from several at once), and afterwards returns the number
in effect (1.9: changing it while conversions ran now and then hung GHC's
runtime). `{"query": "num-threads"}` tells it. On a 16-core, 32-thread
CPU, 16 threads were as fast as 32 or faster: set it to the number of
physical cores for throughput. Several first calls at once in a process
now and then fail with `Exception: <<loop>>` although nothing loops (GHC
9.10's runtime loses a wakeup: GHC #26324, fixed in 9.12.5 and 9.14.2), so
libpandoc makes such a call again, up to three times (1.10), unless it was
given no input; a callback filter may then run twice.

Versions: `LIBPANDOC_ABI_VERSION_MAJOR`/`_MINOR` (and `pandoc_abi_version()`)
are this C interface's own, 1.12 now; the embedded pandoc's version and its
AST's are `pandoc_query` "version" and "api-version". Packages of libpandoc
are versioned as the pandoc they embed. The rule: a binding built for
MAJOR.MINOR works with a library of the same MAJOR and a MINOR at least as
large, and checks it when it loads the library (or libpandoc.wasm), against
the MINOR it relies on; MINOR also marks changes of meaning, listed in the
header. `pandoc_shutdown` is optional and has hazards (the header): most
programs never call it.

The ABI doesn't change with pandoc. What changes with pandoc crosses it
as formats pandoc itself defines: defaults-file options, and the JSON AST
(`to: json` / `from: json`) versioned by `pandoc-api-version`. The Haskell
side uses only what upstream's own `pandoc.wasm` uses (`Opt`'s JSON
decoder, `defaultOpts`, `convertWithOpts`, the Lua engine), plus
`parseOptionsFromArgs`, so upstream keeps it working. Callback filters add
one more public interface: the Lua engine's `engineApplyFilter`.

### Untrusted input

`"untrusted": true` (libpandoc's key, not pandoc's) in `pandoc_convert`'s
options, `pandoc_read_many`'s `"options"` or a `pandoc_query` is for
options from code you don't trust (a wasm filter calling pandoc): about
what the options may do, not about documents (below). libpandoc then accepts
only options that read no files, write none, fetch nothing and run
nothing (the reading ones, and those that only shape the output: `to`,
`standalone`, `toc`, `variables`, ...), formats by name (no Lua readers or
writers, no `pdf`), input given rather than files, and pandoc's sandbox
on. Anything else fails with a `PandocOptionError` naming it: filters,
templates, output files, `data-dir`, `bibliography`, `csl`, ... So only
pandoc's own code runs, and it reads nothing but the input and pandoc's
own data: not the user data directory's templates, reference documents,
abbreviations, translations or styles (1.10). (`PANDOC_DATA_DIRS`'s
directories are left out only in part: the header says which.) `citeproc` is
allowed (1.8), sandboxed as below, with the references in the document's
metadata. Wasm filters' calls to pandoc are always untrusted. The list is
`src/LibPandoc/Untrusted.hs`.

`"untrusted"` limits what the options may do, not how long or how much
memory pandoc takes over a document: a few kilobytes of markdown can keep
it busy for minutes, and a Typst loop or a zip bomb can take all memory.
`"timeout": seconds` (1.10; `pandoc_convert`, `pandoc_read_many`) stops a
call past its time with the error kind `"Timeout"`, and the process goes
on. It stops pandoc's own code and JSON filters (terminated and reaped),
but not code in a foreign call: a callback filter runs to its end first,
and Lua (filters, custom readers and writers) isn't stopped at all. There
is no memory limit per call, and can't be in a native library: running
out of memory ends the process, and so does a heap limit (`+RTS -M`)
inside a library. So for documents from someone you don't trust, give the
conversion memory of its own (libpandoc.wasm, whose heap is capped and
whose instance alone ends; a separate process; a container with a memory
limit) and a `"timeout"`. A host running a wasm filter passes the
filter's remaining time as the `"timeout"` of the filter's calls to
pandoc.

So: for **untrusted options**, `"untrusted": true`; for **untrusted
documents**, the conversion in libpandoc.wasm (its memory capped, the
directories it sees chosen: the bindings' directory sandbox), or a
separate process or container, with a `"timeout"`, and `"sandbox": true`
(or `"untrusted"`) so that pandoc reads nothing the document names.

"Sandbox" means three things, by where it is (the header lists them):
`"sandbox": true` in `pandoc_convert`'s options is pandoc's `--sandbox`
plus citeproc (below); in `pandoc_read_many`'s, readers that read no
files; and `"sandbox": {"read": [...], "write": [...]}`, the bindings'
directory sandbox, is the whole conversion in libpandoc.wasm seeing only
those directories (libpandoc itself refuses an object there, so a host
without it fails rather than ignores it).

`"sandbox": true` alone is pandoc's `--sandbox`, plus citeproc (1.8):
pandoc runs its readers and writers in a sandbox, reading only the files
the options name, but runs citeproc outside it, so a document's metadata
(or LaTeX's `\bibliography`) could make it read any bibliography, CSL
style or abbreviations file, or fetch a URL (`UPSTREAM.md` in the
project). libpandoc runs citeproc in pandoc's sandbox too, with the
`bibliography`, `csl` and `citation-abbreviations` the options name: any
other file is missing, with a missing file's message, and nothing is
fetched. So a bibliography given as metadata (`-M`, a metadata file, a
filter) isn't read either, nor the data directory's `csl/`; name them
in those options.

In libpandoc.wasm under a host that opens only some directories to it
(the bindings' directory sandbox), a path leaving them through `..` or a
symbolic link fails with "permission denied" where a missing file fails
with "does not exist", and pandoc prints some such errors as they are
(AsciiDoc and Typst includes, images): a document can thus learn that a
path it names lies outside those directories, though nothing of what is
there. Where even that must stay hidden, run the conversion where only
those directories exist: a container, or a virtual machine.

### Filters in the caller's language

`pandoc_convert_filters` runs filters the caller implements (a C function
pointer and its data), placed among pandoc's own filters in the options as
`{"type": "callback", "index": i}`. pandoc calls them as it would a JSON
filter, with the document as JSON, the output format and the reader's
and writer's options, but in process, on the calling thread, and within
one conversion: what a reader keeps in memory, such as images embedded in
a docx, reaches the writer. A callback may call libpandoc again, for example to parse a
fragment of text.

JSON filters, and callbacks, are also told the input and output formats
with extensions (`PANDOC_INPUT_FORMAT=commonmark_x-smart`,
`PANDOC_OUTPUT_FORMAT=html5+smart`), which pandoc doesn't tell JSON filters
yet; this follows a proposal to upstream (jgm/pandoc#11016), and will follow
upstream's names if they differ. And the writer's options (1.10:
`PANDOC_WRITER_OPTIONS`, a callback's `"writer-options"`), which pandoc
gives Lua filters only: Lua's fields and values, keyed as pandoc's JSON
keys the reader's options (`number-sections`, `top-level-division`,
`math-method`), so that a filter ported from Lua (`-N`,
`--top-level-division`) finds what it read there.

```c
static int shout(void *data, const char *doc, size_t len,
                 const char *context, size_t context_len, pandoc_buffer *out)
{
    /* ... change the JSON document ... */
    pandoc_buffer_set(out, new_doc, new_len);
    return 0;  /* nonzero: out holds an error message */
}

pandoc_filter filters[] = {{shout, NULL}};
const char *opts = "{\"to\": \"html\", \"filters\": [\"a.lua\", {\"type\": \"callback\", \"index\": 0}]}";
pandoc_result *r = pandoc_convert_filters(opts, strlen(opts), md, strlen(md), filters, 1);
```

### The AST

Documents cross the ABI as pandoc's JSON (`to: json`, `from: json`),
versioned by `pandoc-api-version` (`{"query": "api-version"}`). For the AST
as types in other languages, generated from pandoc-types, see
[panir](https://github.com/ickc/panir), which needs no
libpandoc.

## Building

`pins.env` fixes the pandoc version, the Hackage snapshot (upstream's
release time) and the GHC and cabal versions, as in
[pandoc-feedstock](https://github.com/pandoc-forge/pandoc-feedstock).

```sh
# ghc and cabal of the pinned versions on PATH (e.g. from ghcup), and gmp
bash scripts/build.sh dist
cc test/smoke.c -Idist/include -Ldist/lib -lpandoc -Wl,-rpath,'$ORIGIN/lib' -o dist/smoke && dist/smoke
```

`dist/` is then a relocatable prefix: `include/libpandoc.h`,
`lib/libpandoc.so` (`.dylib`; `bin/pandoc.dll` on Windows) and
`share/libpandoc/api-version.json`.

On Linux, `scripts/merge-link.sh` (GHC's linker in the build) links the
Haskell packages into `libpandoc.so` from their static archives, built
position-independent, but GHC's 11 libraries that come only shared
(base, ghc-internal, template-haskell, ...): `scripts/stage.sh` copies
those and GHC's libffi into `lib/libpandoc/` (12 files, 32 MB, beside
176 MB of `libpandoc.so`, stripped) with relative RPATHs. On macOS all of
them are linked in, leaving GHC's libffi; the Windows DLL is standalone. A
release's tarball is 36 to 40 MB. `lib/pkgconfig/libpandoc.pc`'s version
is the ABI's (`pkg-config --atleast-version=1.12 libpandoc`).

CI builds and tests linux-64, linux-aarch64, osx-64, osx-arm64 and win-64,
and linux-64-musl and linux-aarch64-musl (Alpine and the like: built in the
musllinux_1_2 container with GHC's Alpine bindist, smoke-tested on plain
Alpine), and each push to `main` updates the `continuous` pre-release.
What the build takes from outside (actions, container images, GHC, cabal,
ghcup, ghc-wasm-meta, npm packages) is pinned by commit, digest or
SHA-256: the list is at the top of `.github/workflows/ci.yml`, and what a
pandoc bump must update at the top of `pins.env`.


## License

GPL-2.0-or-later, as pandoc.
