# libpandoc.wasm (spike)

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
- **Engines:** it needs a recent V8. Node 26 runs it. Node 22 segfaults on
  some inputs, and so does upstream's `pandoc.wasm` called through its
  `convert` export, which is how its JavaScript uses it (through `_start`
  it's fine).
- `wasm/libpandoc.mjs` is a minimal Node host (`node:wasi`); a browser host
  would use `@bjorn3/browser_wasi_shim`, as npm's `pandoc-wasm` does.
