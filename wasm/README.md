# libpandoc.wasm

libpandoc's C ABI (`include/libpandoc.h`) built by GHC's wasm backend as a
WASI reactor, for JavaScript hosts (browser, Node.js) and Python in Pyodide.
It uses the same pandoc entry points as upstream's `pandoc.wasm` and the
same build recipe (`cabal-wasm.project`, the patches in `wasm/patches`, from
pandoc 3.11's `cabal.project`).

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
