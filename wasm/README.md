# libpandoc.wasm

libpandoc's C ABI (`include/libpandoc.h`) built by GHC's wasm backend as a
WASI reactor, for JavaScript hosts (browser, Node.js) and Python in Pyodide.
It uses the same pandoc entry points as upstream's `pandoc.wasm` and the
same build recipe (`cabal-wasm.project`, the patches in `wasm/patches`, from
pandoc 3.12's `cabal.project`).

```sh
scripts/build-wasm.sh              # dist/wasm/libpandoc.wasm (needs ~/.ghc-wasm)
node --test wasm/test.mjs          # Node.js: convert, query, read_many, Lua and JS filters
PYODIDE=... PANIR_WHEEL=... node wasm/test-pyodide.mjs   # Python filters in Pyodide
```

- **Exports:** `pandoc_init`, `pandoc_convert`, `pandoc_convert_args`,
  `pandoc_convert_filters`, `pandoc_convert_args_filters`, `pandoc_read_many`,
  `pandoc_query`, `pandoc_result_free`, `pandoc_buffer_set`, `malloc`, `free`.
  Call `_initialize` (WASI's reactor start), then `pandoc_init`.
- **Callback filters:** a `pandoc_filter` whose `fn` is `NULL` is the host's
  import `libpandoc.filter(userdata, doc, doc_len, context, context_len, out)`,
  which answers with `pandoc_buffer_set` as a `pandoc_filter_fn` does. A
  filter may call libpandoc again.
- **Files:** pandoc's temporary files go in `/tmp`, which the host must
  provide (a preopened directory, or an in-memory one in the browser).
- **Not in it:** threads (`read_many` reads one after another), JSON filters
  (WASI can't start processes), `pandoc_main`, `pandoc lua`, PDF, HTTP.
- **Engines** (tested 2026-09-28 with Playwright's browsers,
  `wasm/test-browser.mjs`): it needs wasm's exnref exception handling (the
  `try_table` opcode, 0x1f), which pandoc's Lua is compiled with, as in
  upstream's pandoc.wasm.

  | engine | runs it | rejects it |
  |---|---|---|
  | Chromium | 138, 153 | 127, 131, 136 |
  | Firefox | 132, 155 | 127 |
  | WebKit (Safari) | 18.2, 18.4, 18.5, 26.6 | 17.4 (times out) |
  | Node | 26 | 22 segfaults on some inputs (early exnref), as upstream's pandoc.wasm does through its exports |

  Loading takes 0.2 s in Chromium and Firefox, and 1.4 s (WebKit 26.6) to
  7.7 s (WebKit 18.2) in WebKit. 800 paragraphs of markdown to HTML: 0.3–0.6
  s, against 0.09 s for native pandoc.
- **Hosts:** `wasm/core.mjs` is the ABI over wasm memory: `abi`, byte for
  byte what libpandoc-python's `_core` does (its Pyodide backend calls it),
  and on top `convert`, `convertWithFilters` (JS filters, or `raw` JSON
  text ones), `query`, `readMany`. `wasm/node.mjs` runs it with `node:wasi`
  (real directories preopened), `wasm/browser.mjs` with
  `@bjorn3/browser_wasi_shim` (`/tmp` in memory, or any directories given;
  run it in a Worker).
- **Memory:** wasm32 has 4 GiB of linear memory at most. The runtime has
  a heap limit (`-M3584m`), with which GHC compacts instead of copying its
  oldest generation: about 80 MB of markdown still converts (to HTML, in
  5 minutes). Past that, the instance stops: see "When an instance stops"
  below.
- **Wasm filters:** `wasm/wasm-filter.mjs` runs a filter compiled to
  WebAssembly inside a conversion: a pandoc JSON filter built for WASI
  (`wasm32-wasip1`), such as a Rust one with panir, reading the document
  on stdin and writing it to stdout, told the format as its argument and
  the rest in `PANDOC_*` variables as pandoc tells JSON filters. It is
  compiled once and instantiated per run, synchronously, with an in-memory
  stdin and stdout, seeing only the directories given
  (`browser_wasi_shim`, in the browser and Node alike). The same file runs
  in pandocrs (libpandoc-rs, wasmtime). Tested in Node
  (`test-wasm-filter.mjs`) and in browsers (`WASM_FILTERS=... node
  wasm/test-browser.mjs`: Chromium 153, Firefox 155, WebKit 26.6).
  A filter may call pandoc (libpandoc-rs's `libpandoc::read_many` and the
  like, built for wasm, import them from a `libpandoc` module): given
  `{ pandoc }`, those calls go to this libpandoc.wasm, marked
  `"untrusted": true`: libpandoc then allows pandoc's sandbox only, and no
  options that read or write files, fetch resources or run programs (the
  list every host shares, in `LibPandoc.Untrusted`).
  The same `"untrusted": true` works in `convert`'s options for untrusted
  documents or options in general.
  `{ maxMemory }` (bytes; in Node, by default `$LIBPANDOC_WASM_MAX_MEMORY`,
  as the other hosts) caps a filter's memory: its module's own maximum is
  lowered before it is compiled. There is no timeout, as the other hosts
  have: a filter runs synchronously on the conversion's thread, which
  nothing can interrupt. For one, run the conversion in a Worker and
  terminate it.
- **Pyodide:** `wasm/emscripten-fs.mjs` makes WASI directories out of
  Pyodide's filesystem, so that Python and pandoc see the same files:
  `load(url, { tmp: emscriptenDirectory(py.FS, "/tmp") })`. libpandoc-python
  on Pyodide (its `_wasm` backend) passes 46 of its 49 tests on it; the
  other 3 need threads.
- **Tests** (`npm install` in `wasm/` first): `test.mjs` (Node),
  `test-browser.mjs` (Chromium, Firefox, WebKit with Playwright),
  `test-pyodide.mjs` (Python filters in Pyodide), `test-pyodide-suite.mjs`
  (libpandoc-python's tests in Pyodide; `SHIM=1` through the shared
  filesystem). CI runs them all.
- **Benchmark:** `bench-browser.mjs`: the same filters as Lua, panir JS and
  panir Python (Pyodide), in one conversion, in browsers. On pandoc's
  MANUAL (299 KB, markdown to HTML, about 1 s), a Python filter adds 10–20%,
  a JS one 3–10%; Pyodide costs 1 s to start and 6.3 MB to download.

## When an instance stops

An instance of libpandoc.wasm (what one `load()` returns) can stop for
good in the middle of a call. What happens:

- **When:** GHC's runtime runs out of memory (wasm32 has 4 GiB; about
  80 MB of markdown still converts, a bigger document won't), or the module
  traps (a `WebAssembly.RuntimeError`, such as a stack overflow). Normal
  failures are not this: a document pandoc can't read, a bad option, a JS
  or wasm filter that throws or traps all come back as a `PandocError`, and
  the instance goes on.
- **Why for good:** out of memory, the runtime calls `exit` (WASI's
  `proc_exit`), which the host throws as an exception through the running
  conversion. The runtime is left with its heap full and a conversion half
  done, and isn't meant to run again (tried: the next call fails the same
  way). A wasm memory can't shrink either.
- **What you see:** a `StoppedError` (exported by `node.mjs` and
  `browser.mjs`, next to `PandocError`), whose message says why and whose
  `cause` is what the host threw. **Every later call on the same instance
  throws a `StoppedError` too**, at once, without entering wasm. Other
  instances are unaffected.
- **What to do:** drop the instance and `load()` a new one, from the old
  one's `module` (its compiled `WebAssembly.Module`; `load` takes one as
  well as a path, URL or bytes): 40–60 ms in Node, against 120–140 ms from
  the file, and in a browser no second download. The new instance starts
  a fresh runtime, and the old one's memory is freed once nothing refers
  to it. There's no automatic reload or retry: the same input will most
  likely stop the new instance too, and what the old one held (its `/tmp`)
  is gone, so the application decides.

```js
import { load, StoppedError } from "./node.mjs"; // or browser.mjs

let pandoc = load(wasmPath); // an instance, as a promise

async function convert(options, input) {
  const instance = await pandoc;
  try {
    return instance.convert(options, input);
  } catch (e) {
    if (e instanceof StoppedError) {
      // this instance is gone: a fresh one (no recompiling) for later calls
      pandoc = load(instance.module);
      // and this input most likely can't be converted here: say so, don't retry
      throw new Error("the document is too large to convert here", { cause: e });
    }
    throw e; // a PandocError: about this document; the instance is fine
  }
}
```

In a browser, run conversions in a Worker (worth it anyway: a conversion
can take seconds, and would block the page). Then a stopped instance, or
a conversion running too long, can also be ended from outside with
`worker.terminate()`, and a new Worker started.

- **Pyodide** (libpandoc-python's prototype backend): the call raises
  Pyodide's `JsException` with `e.name == "StoppedError"`. The package
  holds the instance the host registered (`libpandoc_wasm`), so recovering
  means loading and registering a new one and importing the package again;
  for now, restart Pyodide.
- **Native libpandoc** has none of this: no 4 GiB ceiling, and memory
  exhaustion there is the operating system's to handle.

