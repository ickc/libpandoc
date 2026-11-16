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
// pandoc's messages: text, but not to fail on if a byte isn't
const lenientUtf8 = new TextDecoder("utf-8");

// A value given to a call that isn't bytes or a string (or a string with a
// NUL where C reads up to it), refused before anything enters wasm: the
// instance goes on.
class InputError extends TypeError {}

// bytes of a string, a Uint8Array, an ArrayBuffer or another view of one
function toBytes(x, what) {
  if (typeof x === "string") return utf8.encode(x);
  if (x instanceof Uint8Array) return x;
  if (x instanceof ArrayBuffer) return new Uint8Array(x);
  if (ArrayBuffer.isView(x)) return new Uint8Array(x.buffer, x.byteOffset, x.byteLength);
  throw new InputError(`${what}: bytes or a string, not ${x === null ? "null" : typeof x}`);
}
// a C string: no NUL inside
function cBytes(s, what) {
  if (typeof s !== "string") throw new InputError(`${what}: a string, not ${typeof s}`);
  if (s.includes("\0")) throw new InputError(`${what}: a string without NUL`);
  return utf8.encode(s + "\0");
}

// Writers whose output is bytes, not text (as libpandoc-python's): what
// convert returns as bytes unless told otherwise.
const BINARY_WRITERS = new Set(["docx", "odt", "epub", "epub2", "epub3", "pptx", "pdf", "chunkedhtml", "xlsx"]);

function binaryOutput(options) {
  const to = options?.to ?? options?.writer ?? "html";
  if (typeof to !== "string" || to.toLowerCase().includes(".lua")) return false; // a Lua writer
  return BINARY_WRITERS.has(to.split(/[+-]/, 1)[0]);
}

// What libpandoc reads with: the conversion's options that apply to
// reading a fragment, and the reader's options a filter is told that have
// a defaults-file key (as libpandoc-python's read_options).
const READ_OPTIONS = ["abbreviations", "data-dir", "default-image-extension", "indented-code-classes",
  "resource-path", "strip-comments", "tab-stop", "track-changes"];
const READER_OPTIONS = ["default-image-extension", "indented-code-classes", "strip-comments", "tab-stop"];
const TRACK_CHANGES = { "accept-changes": "accept", "reject-changes": "reject", "all-changes": "all" };

/** Whether `x` is a conversion a filter runs in: libpandoc's context (the
 *  filter's second argument) or panir's `Conversion`. */
export function isConversion(x) {
  return x !== null && typeof x === "object" && !Array.isArray(x)
    && ["reader-options", "input-format", "readerOptions", "inputFormat"].some((k) => k in x);
}

/** Options (defaults-file keys) to read a fragment as `conversion` reads
 *  its input: its input format (or `format`), the options that affect
 *  reading, its tabs kept (as Lua's pandoc.read), and pandoc's sandbox when
 *  the conversion is sandboxed or that can't be told (it reads no files: a
 *  LaTeX \input or an RST include in the text). `conversion` is
 *  libpandoc's context (1.13 says "sandbox") or panir's `Conversion`
 *  (`options` when known, `sandbox` when told). */
export function readOptions(conversion, format = undefined) {
  const c = conversion ?? {};
  const options = c.options ?? undefined;
  const reader = c.readerOptions ?? c["reader-options"];
  const opts = {};
  if (options) {
    for (const k of READ_OPTIONS) if (options[k] !== undefined) opts[k] = options[k];
  } else if (reader) {
    for (const k of READER_OPTIONS) if (reader[k] !== undefined) opts[k] = reader[k];
    if (TRACK_CHANGES[reader["track-changes"]]) opts["track-changes"] = TRACK_CHANGES[reader["track-changes"]];
  }
  // fail closed: sandboxed unless libpandoc said it isn't, or the options
  // (when known) say so
  const told = c.sandbox, asked = options?.sandbox;
  if (told === true || (asked != null && asked !== false) || (told == null && options == null)) opts.sandbox = true;
  opts["preserve-tabs"] = true;
  opts.from = format ?? c.inputFormat ?? c["input-format"] ?? options?.from ?? options?.reader ?? "markdown";
  return opts;
}

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
  let stopped = null; // why the instance stopped (below)
  const imports = {
    ...wasi.imports,
    libpandoc: {
      // int filter(void *userdata, doc, doc_len, context, context_len, out).
      // Nothing may be thrown from here through the conversion's wasm
      // frames, which would leave it unfinished (its temporary files, its
      // memory) and the runtime in no known state: whatever the filter does,
      // the conversion gets an answer, a document or an error message.
      // Unless the instance stopped meanwhile (a call the filter made ran
      // out of memory): then that goes on up.
      filter(k, doc, docLen, ctx, ctxLen, out) {
        let status = 0, answer;
        try {
          answer = filters[k](copy(doc, docLen), copy(ctx, ctxLen));
          if (typeof answer?.then === "function")
            throw new Error("the filter returned a Promise: filters run synchronously");
          answer = toBytes(answer, "the filter's answer");
        } catch (e) {
          status = 1;
          let message;
          try {
            message = String(e?.message ?? e);
          } catch {
            message = "the filter failed";
          }
          answer = utf8.encode(message);
        }
        if (stopped) throw new StoppedError(stopped);
        let p = ex.malloc(Math.max(answer.length, 1));
        if (p === 0) {
          status = 1;
          answer = utf8.encode(`the filter's answer (${answer.length} bytes) doesn't fit in libpandoc.wasm's memory`);
          p = ex.malloc(answer.length);
          if (p === 0) return 1; // an error without a message
        }
        mem().set(answer, p);
        ex.pandoc_buffer_set(out, p, answer.length);
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
    const end = m.indexOf(0, p);
    if (end < 0) throw new Error("libpandoc.wasm: a string without its end");
    return lenientUtf8.decode(m.subarray(p, end));
  }
  // a malloc'd copy of bytes; the caller frees it
  function alloc(b) {
    const p = ex.malloc(Math.max(b.length, 1));
    if (p === 0) throw new InputError(`libpandoc.wasm: no memory for ${b.length} bytes`);
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
  // run f with each of `values` (bytes or strings) malloc'd (null stays
  // NULL), then free them
  function withAlloc(values, f) {
    const bytes = values.map((x, i) => (x == null ? null : toBytes(x, `argument ${i + 1}`)));
    const ptrs = [];
    try {
      for (const b of bytes) ptrs.push(b === null ? [0, 0] : alloc(b));
      return f(...ptrs);
    } finally {
      for (const [p] of ptrs) if (p) ex.free(p);
    }
  }
  // argv: a C array of C strings
  function withArgv(args, f) {
    if (!Array.isArray(args)) throw new InputError("arguments: an array of strings");
    const bytes = args.map((a, i) => cBytes(a, `argument ${i + 1}`));
    return withAlloc([...bytes, new Uint8Array(4 * bytes.length)], (...ptrs) => {
      const argv = ptrs.pop()[0];
      ptrs.forEach(([p], i) => view().setUint32(argv + 4 * i, p, true));
      return f(args.length, argv);
    });
  }
  // pandoc_filter[]: {fn = NULL, userdata = base + i}: pandoc's filter i is
  // filters[base + i]. A filter may itself convert with filters: its own
  // come after.
  function withFilters(fns, f) {
    if (!Array.isArray(fns)) throw new InputError("filters: an array of functions");
    const base = filters.length;
    const arr = new Uint8Array(8 * fns.length);
    fns.forEach((_, i) => new DataView(arr.buffer).setUint32(8 * i + 4, base + i, true));
    return withAlloc([arr], ([p]) => {
      filters.push(...fns);
      try {
        return f(p, fns.length);
      } finally {
        filters.length = base;
      }
    });
  }

  // An exception out of a call leaves the instance unusable: the Haskell
  // runtime exiting (out of memory: WASI's proc_exit, which node:wasi
  // throws as a symbol, browser_wasi_shim as a WASIProcExit), the module
  // trapping, or anything else thrown through its frames (a WASI shim's
  // own error). Say so as a StoppedError, now and on every later call,
  // rather than throwing a bare value. Not a call refused before it
  // entered wasm (InputError). (Also after close().)
  const guard = (f) => (...args) => {
    if (stopped) throw new StoppedError(stopped);
    try {
      return f(...args);
    } catch (e) {
      if (e instanceof InputError) throw e;
      if (e instanceof StoppedError && stopped) throw e;
      const exited = typeof e === "symbol" || e?.constructor?.name === "WASIProcExit";
      const why = exited ? "its runtime exited, most likely out of memory (wasm32's 4 GiB)"
        : e instanceof WebAssembly.RuntimeError ? `${e.message}, most likely out of memory (wasm32's 4 GiB)`
        : `${e?.message ?? String(e)}, in the middle of a call`;
      stopped = `libpandoc.wasm stopped (${why}): load it again`;
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
    // pandoc_main (libpandoc.wasm 1.11): the pandoc command, argv[0] its
    // name, on the host's WASI standard input, output and error; its exit
    // status. filtersJson (or null) and fns as pandoc_main's filters_json
    // and filters.
    main: (args, filtersJson = null, fns = []) => {
      if (!ex.pandoc_main) throw new InputError("libpandoc.wasm before 1.11 has no pandoc_main");
      return withFilters(fns, (arr, n) =>
        withArgv(args, (argc, argv) =>
          withAlloc([filtersJson], ([fp, fN]) => ex.pandoc_main(argc, argv, fp, fN, arr, n))));
    },
    query: (q) => withAlloc([q], ([p, n]) => take(ex.pandoc_query(p, n))),
    // pandoc's working directory (wasi-libc's; "/" at first), against which
    // relative paths resolve: 0, or -1 if there is no such directory (or
    // the module predates the export)
    chdir: (path) =>
      ex.chdir ? withAlloc([cBytes(path, "chdir")], ([p]) => ex.chdir(p)) : -1,
  };
  for (const k of Object.keys(abi)) abi[k] = guard(abi[k]);

  // the output, or the error thrown
  function ok([status, output, kind, message, log]) {
    if (status !== 0) throw new PandocError(kind, message);
    return { output, log: JSON.parse(log) };
  }
  const str = (b) => fromUtf8.decode(b);
  // the output: bytes with `bytes`, text without; by default as the format
  // is (docx, epub, ... bytes)
  function output(out, options, bytes) {
    if (bytes ?? binaryOutput(options)) return out;
    try {
      return str(out);
    } catch (e) {
      throw new TypeError("the output isn't UTF-8 text (a binary format?): pass { bytes: true } for bytes",
                          { cause: e });
    }
  }
  // options to read with: from a format or a conversion, and those given
  function readRequest(from, options) {
    const opts = isConversion(from) ? readOptions(from) : { from: from ?? "markdown" };
    return { ...opts, ...options };
  }

  const self = {
    exports: ex,
    abi,
    /** Stop this instance: every later call throws a StoppedError (the
     *  hosts' close() also remove its files). */
    close() {
      stopped ??= "libpandoc.wasm: this instance was closed";
    },
    abiVersion: abi.abiVersion,
    /** Convert with defaults-file options; `input` (a string or bytes) is
     *  the standard input. The output is a string (bytes with `bytes`,
     *  for docx and the like). */
    convert(options, input = null, { bytes } = {}) {
      return output(ok(abi.convert(JSON.stringify(options), input)).output, options, bytes);
    },
    /** Convert, with JS functions as filters: each takes the document as
     *  pandoc's JSON, the context, and this instance (to read fragments as
     *  the conversion reads: `pandoc.read(text, ctx)`, `pandoc.readMany`;
     *  or `convert`, `query`: nested calls into the same instance), and
     *  returns the new document. Named
     *  in `options.filters` as {type: "callback", index: i}; by default,
     *  all of them after the options' own filters. With `raw`, the
     *  functions take and return JSON text (for a library that parses it
     *  itself, such as panir, or for Python in Pyodide). A function with
     *  `bytes` set (wasm-filter.mjs's) takes and returns bytes. The output
     *  as `convert`'s. */
    convertWithFilters(options, input, fns, { raw = false, bytes } = {}) {
      const opts = { ...options };
      if (!opts.filters?.some((f) => f?.type === "callback")) {
        opts.filters = [...(opts.filters ?? []), ...fns.map((_, i) => ({ type: "callback", index: i }))];
      }
      // a function marked `bytes` (a wasm filter) takes and returns bytes
      const wrapped = fns.map((fn) => fn.bytes ? (doc, ctx) => fn(doc, ctx, self) : raw
        ? (doc, ctx) => fn(str(doc), str(ctx), self)
        : (doc, ctx) => {
          const out = fn(JSON.parse(str(doc)), JSON.parse(str(ctx)), self);
          if (out === undefined) throw new Error("the filter returned no document (a missing return?)");
          if (typeof out?.then === "function")
            throw new Error("the filter returned a Promise: filters run synchronously");
          return JSON.stringify(out);
        });
      return output(ok(abi.convertFilters(JSON.stringify(opts), input, wrapped)).output, options, bytes);
    },
    query(q) {
      return JSON.parse(str(ok(abi.query(JSON.stringify(q))).output));
    },
    /** The pandoc command: what `pandoc ...args` does, on the host's
     *  standard input, output and error (node.mjs: the process's), with
     *  pandoc's exit status as the result. `filters`: JS functions (as
     *  convertWithFilters takes them, bytes only: a wasm filter's) that
     *  replace those of the arguments, given as `filtersJson`, a list as
     *  the "filters" option naming them {type: "callback", index: i}. */
    main(args, { filtersJson = null, filters = [] } = {}) {
      return abi.main(["pandoc", ...args], filtersJson === null ? null : JSON.stringify(filtersJson), filters);
    },
    /** Read many texts, each on its own: their documents (pandoc's JSON),
     *  or {error: {kind, message}} in a text's place. `from`: the options
     *  (with "from"), a format, or a conversion (a filter's context, or
     *  panir's Conversion) to read as it reads (`readOptions`); `options`
     *  win over what that gives. */
    readMany(inputs, from = {}, options = {}) {
      const opts = isConversion(from) ? { ...readOptions(from), ...options }
        : typeof from === "string" ? { from, ...options } : { ...from, ...options };
      return JSON.parse(str(ok(abi.readMany(JSON.stringify({ options: opts, inputs }))).output));
    },
    /** Read one text: its document (pandoc's JSON); a PandocError if it
     *  can't be. `from`: a format (default markdown), or a conversion, as
     *  for readMany; `options` win over what that gives. */
    read(text, from = "markdown", options = {}) {
      if (typeof text !== "string") throw new InputError("read: the text, a string");
      const [doc] = JSON.parse(str(ok(abi.readMany(JSON.stringify({ options: readRequest(from, options),
                                                                    inputs: [text] }))).output));
      if (doc?.error) throw new PandocError(doc.error.kind, doc.error.message);
      return doc;
    },
    /** Set pandoc's working directory (an absolute path in its
     *  filesystem; "/" at first), against which relative paths resolve. */
    chdir(path) {
      if (abi.chdir(path) !== 0) throw new Error(`libpandoc.wasm: can't change to ${path}`);
    },
  };
  return self;
}
