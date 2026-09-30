// libpandoc.wasm in any JavaScript host: the C ABI of include/libpandoc.h
// over wasm memory. The host (node.mjs, browser.mjs) brings WASI and the
// module; this works the same in both.
//
// Two layers:
// - `abi`: the C functions, bytes in and out, each returning
//   [status, output, errorKind, errorMessage, log], as libpandoc-python's
//   `_core` does (a Pyodide backend of it calls this);
// - the rest: JSON options, strings, errors thrown, filters as JS functions.

const utf8 = new TextEncoder();
const fromUtf8 = new TextDecoder("utf-8", { fatal: true });

export class PandocError extends Error {
  constructor(kind, message) {
    super(message);
    this.kind = kind;
  }
}

/** This instance of libpandoc.wasm has stopped and can't be used again:
 *  its Haskell runtime exited (out of memory: wasm32 has 4 GiB) or the
 *  module trapped, in the middle of a call. Every later call on it throws
 *  this too, without entering wasm. `load()` a new instance (which also
 *  frees the memory, once nothing refers to the old one). See README.md,
 *  "When an instance stops". */
export class StoppedError extends Error {
  constructor(message, options) {
    super(message, options);
    this.name = "StoppedError";
  }
}

/** The library over an instance of libpandoc.wasm: `module` is the
 *  compiled module, `wasi` the host's WASI: its import object and how it
 *  starts a reactor. */
export async function start(module, wasi) {
  // the filters of the conversions running now: a function taking and
  // returning bytes (the document, and the context, as pandoc's JSON)
  const filters = [];
  let ex; // the instance's exports
  const imports = {
    ...wasi.imports,
    libpandoc: {
      // int filter(void *userdata, doc, doc_len, context, context_len, out)
      filter(k, doc, docLen, ctx, ctxLen, out) {
        let status = 0, answer;
        try {
          answer = filters[k](copy(doc, docLen), copy(ctx, ctxLen));
        } catch (e) {
          status = 1;
          answer = utf8.encode(String(e?.message ?? e));
        }
        const [p, n] = alloc(answer);
        ex.pandoc_buffer_set(out, p, n);
        ex.free(p);
        return status;
      },
    },
  };
  const instance = await WebAssembly.instantiate(module, imports);
  ex = instance.exports;
  wasi.initialize(instance);
  if (ex.pandoc_init() !== 0) throw new Error("libpandoc.wasm: the runtime didn't start");

  const mem = () => new Uint8Array(ex.memory.buffer);
  const view = () => new DataView(ex.memory.buffer);
  const copy = (p, n) => mem().slice(p, p + n);
  function cstring(p) {
    if (p === 0) return null;
    const m = mem();
    let end = p;
    while (m[end] !== 0) end++;
    return fromUtf8.decode(m.subarray(p, end));
  }
  // a malloc'd copy of bytes or a string; the caller frees it
  function alloc(s) {
    const b = typeof s === "string" ? utf8.encode(s) : s;
    const p = ex.malloc(Math.max(b.length, 1));
    mem().set(b, p);
    return [p, b.length];
  }
  // pandoc_result on wasm32: status, output, output_len, error_kind,
  // error_message, log; 4 bytes each
  function take(r) {
    if (r === 0) throw new Error("libpandoc.wasm: no result");
    const v = view();
    const res = [
      v.getInt32(r, true),
      copy(v.getUint32(r + 4, true), v.getUint32(r + 8, true)),
      cstring(v.getUint32(r + 12, true)),
      cstring(v.getUint32(r + 16, true)),
      cstring(v.getUint32(r + 20, true)) ?? "[]",
    ];
    ex.pandoc_result_free(r);
    return res;
  }
  // run f with each of `values` malloc'd (null stays NULL), then free them
  function withAlloc(values, f) {
    const ptrs = values.map((x) => (x == null ? [0, 0] : alloc(x)));
    try {
      return f(...ptrs);
    } finally {
      for (const [p] of ptrs) if (p) ex.free(p);
    }
  }
  function withArgv(args, f) {
    const ptrs = args.map((a) => alloc(utf8.encode(a + "\0"))[0]);
    const argv = ex.malloc(4 * Math.max(args.length, 1));
    ptrs.forEach((p, i) => view().setUint32(argv + 4 * i, p, true));
    try {
      return f(args.length, argv);
    } finally {
      ptrs.forEach((p) => ex.free(p));
      ex.free(argv);
    }
  }
  // pandoc_filter[]: {fn = NULL, userdata = base + i}: pandoc's filter i is
  // filters[base + i]. A filter may itself convert with filters: its own
  // come after.
  function withFilters(fns, f) {
    const base = filters.length;
    filters.push(...fns);
    const arr = ex.malloc(8 * Math.max(fns.length, 1));
    fns.forEach((_, i) => {
      view().setUint32(arr + 8 * i, 0, true);
      view().setUint32(arr + 8 * i + 4, base + i, true);
    });
    try {
      return f(arr, fns.length);
    } finally {
      ex.free(arr);
      filters.length = base;
    }
  }

  // The Haskell runtime exiting (out of memory: WASI's proc_exit, which
  // node:wasi throws as a symbol, browser_wasi_shim as a WASIProcExit) or
  // trapping leaves the instance unusable: say so as a StoppedError, now
  // and on every later call, rather than throwing a bare value.
  let stopped = null;
  const guard = (f) => (...args) => {
    if (stopped) throw new StoppedError(stopped);
    try {
      return f(...args);
    } catch (e) {
      const exited = typeof e === "symbol" || e?.constructor?.name === "WASIProcExit";
      if (!exited && !(e instanceof WebAssembly.RuntimeError)) throw e;
      stopped = `libpandoc.wasm stopped (${exited ? "its runtime exited" : e.message}), ` +
        "most likely out of memory (wasm32's 4 GiB): load it again";
      throw new StoppedError(stopped, { cause: e });
    }
  };

  const abi = {
    abiVersion: () => ex.pandoc_abi_version(),
    convert: (options, input) =>
      withAlloc([options, input], ([op, on], [ip, iN]) => take(ex.pandoc_convert(op, on, ip, iN))),
    convertArgs: (args, input) =>
      withArgv(args, (argc, argv) =>
        withAlloc([input], ([ip, iN]) => take(ex.pandoc_convert_args(argc, argv, ip, iN)))),
    convertFilters: (options, input, fns) =>
      withFilters(fns, (arr, n) =>
        withAlloc([options, input], ([op, on], [ip, iN]) =>
          take(ex.pandoc_convert_filters(op, on, ip, iN, arr, n)))),
    convertArgsFilters: (args, input, fns) =>
      withFilters(fns, (arr, n) =>
        withArgv(args, (argc, argv) =>
          withAlloc([input], ([ip, iN]) =>
            take(ex.pandoc_convert_args_filters(argc, argv, ip, iN, arr, n))))),
    readMany: (request) => withAlloc([request], ([p, n]) => take(ex.pandoc_read_many(p, n))),
    query: (q) => withAlloc([q], ([p, n]) => take(ex.pandoc_query(p, n))),
  };
  for (const k of Object.keys(abi)) abi[k] = guard(abi[k]);

  // the output, or the error thrown
  function ok([status, output, kind, message, log]) {
    if (status !== 0) throw new PandocError(kind, message);
    return { output, log: JSON.parse(log) };
  }
  const str = (b) => fromUtf8.decode(b);

  return {
    exports: ex,
    abi,
    abiVersion: abi.abiVersion,
    /** Convert with defaults-file options; `input` (a string or bytes) is
     *  the standard input. The output is a string (bytes with `bytes`,
     *  for docx and the like). */
    convert(options, input = null, { bytes = false } = {}) {
      const { output } = ok(abi.convert(JSON.stringify(options), input));
      return bytes ? output : str(output);
    },
    /** Convert, with JS functions as filters: each takes the document as
     *  pandoc's JSON and the context, and returns the new document. Named
     *  in `options.filters` as {type: "callback", index: i}; by default,
     *  all of them after the options' own filters. With `raw`, the
     *  functions take and return JSON text (for a library that parses it
     *  itself, such as panir, or for Python in Pyodide). A function with
     *  `bytes` set (wasm-filter.mjs's) takes and returns bytes. */
    convertWithFilters(options, input, fns, { raw = false, bytes = false } = {}) {
      const opts = { ...options };
      if (!opts.filters?.some((f) => f?.type === "callback")) {
        opts.filters = [...(opts.filters ?? []), ...fns.map((_, i) => ({ type: "callback", index: i }))];
      }
      // a function marked `bytes` (a wasm filter) takes and returns bytes
      const wrapped = fns.map((fn) => fn.bytes ? fn : raw
        ? (doc, ctx) => fn(str(doc), str(ctx))
        : (doc, ctx) => JSON.stringify(fn(JSON.parse(str(doc)), JSON.parse(str(ctx)))));
      const { output } = ok(abi.convertFilters(JSON.stringify(opts), input, wrapped));
      return bytes ? output : str(output);
    },
    query(q) {
      return JSON.parse(str(ok(abi.query(JSON.stringify(q))).output));
    },
    readMany(inputs, options = {}) {
      return JSON.parse(str(ok(abi.readMany(JSON.stringify({ options, inputs }))).output));
    },
  };
}
