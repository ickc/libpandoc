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

Bindings: [libpandoc-python](https://github.com/ickc/libpandoc-python).

## The ABI

| Function | Does |
|---|---|
| `pandoc_convert(options, input)` | a conversion; `options` is a JSON object in [defaults-file](https://pandoc.org/MANUAL.html#defaults-files) format, `input` is stdin |
| `pandoc_convert_args(argc, argv, input)` | the same with command-line arguments, parsed by pandoc itself |
| `pandoc_convert_filters(options, input, filters, n)` | a conversion with filters implemented by the caller, run in process (below) |
| `pandoc_query(query)` | formats, extensions, templates and versions, as JSON |
| `pandoc_result_free(r)` | frees a result: output bytes, typed error, and pandoc's log as JSON |

The Haskell runtime starts on first use, without installing signal
handlers, and every Haskell exception becomes an error result: nothing
exits or crashes the host process. Calls may be made from any thread,
concurrently.

The ABI doesn't change with pandoc. What changes with pandoc crosses it
as formats pandoc itself defines: defaults-file options, and the JSON AST
(`to: json` / `from: json`) versioned by `pandoc-api-version`. The Haskell
side uses only what upstream's own `pandoc.wasm` uses (`Opt`'s JSON
decoder, `defaultOpts`, `convertWithOpts`, the Lua engine), plus
`parseOptionsFromArgs`, so upstream keeps it working. Callback filters add
one more public interface: the Lua engine's `engineApplyFilter`.

### Filters in the caller's language

`pandoc_convert_filters` runs filters the caller implements (a C function
pointer and its data), placed among pandoc's own filters in the options as
`{"type": "callback", "index": i}`. pandoc calls them as it would a JSON
filter, with the document as JSON, the output format and the reader's
options, but in process, on the calling thread, and within one conversion:
what a reader keeps in memory, such as images embedded in a docx, reaches
the writer. A callback may call libpandoc again, for example to parse a
fragment of text.

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
[pandom](https://github.com/ickc/pandom), which needs no
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

On Linux and macOS, cabal links foreign libraries against Haskell shared
libraries, and on x86-64 Linux GHC's static libraries aren't
position-independent, so one self-contained `.so` isn't possible with a
stock GHC. `scripts/stage.sh` therefore copies the ~200 Haskell shared
libraries into `lib/libpandoc/` with relative RPATHs (about 300 MB, 60 MB
compressed). The Windows DLL is standalone.

CI builds and tests linux-64, linux-aarch64, osx-64, osx-arm64 and win-64,
and each push to `main` updates the `continuous` pre-release.

## License

GPL-2.0-or-later, as pandoc.
