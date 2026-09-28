// libpandoc.wasm in Node.js: the C ABI of include/libpandoc.h over wasm
// memory. A spike: enough to convert, query, read many, and run JS filters
// inside a conversion.
//
//   import { load } from "./libpandoc.mjs";
//   const pandoc = await load("dist/wasm/libpandoc.wasm");
//   pandoc.convert({ from: "markdown", to: "html" }, "*hi*");
//
// pandoc writes its temporary files in /tmp, which is a real directory
// preopened for it.

import { mkdtempSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { WASI } from "node:wasi";

const utf8 = new TextEncoder();
const fromUtf8 = new TextDecoder("utf-8", { fatal: true });

export class PandocError extends Error {
  constructor(kind, message) {
    super(message);
    this.kind = kind;
  }
}

export async function load(path, { preopens } = {}) {
  const tmp = mkdtempSync(join(tmpdir(), "libpandoc-wasm-"));
  const wasi = new WASI({
    version: "preview1",
    args: ["libpandoc"],
    env: { TMPDIR: "/tmp" },
    preopens: { "/tmp": tmp, ...preopens },
    returnOnExit: true,
  });
  const module = await WebAssembly.compile(await readFile(path));
  const filters = []; // the JS functions of the conversion running now
  let ex; // the instance's exports
  const imports = {
    ...wasi.getImportObject(),
    libpandoc: {
      // int filter(void *userdata, doc, doc_len, context, context_len, out)
      filter(k, doc, docLen, ctx, ctxLen, out) {
        let status = 0, answer;
        try {
          const context = JSON.parse(text(ctx, ctxLen));
          answer = JSON.stringify(filters[k](JSON.parse(text(doc, docLen)), context));
        } catch (e) {
          status = 1;
          answer = String(e?.stack ?? e);
        }
        const [p, n] = bytes(answer);
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
  function text(p, n) {
    return fromUtf8.decode(mem().subarray(p, p + n));
  }
  function cstring(p) {
    if (p === 0) return null;
    const m = mem();
    let end = p;
    while (m[end] !== 0) end++;
    return fromUtf8.decode(m.subarray(p, end));
  }
  // a malloc'd copy; the caller frees it
  function bytes(s) {
    const b = typeof s === "string" ? utf8.encode(s) : s;
    const p = ex.malloc(Math.max(b.length, 1));
    mem().set(b, p);
    return [p, b.length];
  }
  // pandoc_result on wasm32: status, output, output_len, error_kind,
  // error_message, log; 4 bytes each
  function result(r) {
    if (r === 0) throw new Error("libpandoc.wasm: no result");
    const v = view();
    const status = v.getInt32(r, true);
    const out = text(v.getUint32(r + 4, true), v.getUint32(r + 8, true));
    const kind = cstring(v.getUint32(r + 12, true));
    const message = cstring(v.getUint32(r + 16, true));
    const log = JSON.parse(cstring(v.getUint32(r + 20, true)) ?? "[]");
    ex.pandoc_result_free(r);
    if (status !== 0) throw new PandocError(kind, message);
    return { output: out, log };
  }
  function call(fn, json, input) {
    const [op, on] = bytes(JSON.stringify(json));
    let ip = 0, iN = 0;
    if (input != null) [ip, iN] = bytes(input);
    try {
      return result(fn(op, on, ip, iN));
    } finally {
      ex.free(op);
      if (ip) ex.free(ip);
    }
  }

  return {
    exports: ex,
    /** The host directory pandoc sees as /tmp. */
    tmp,
    abiVersion: () => ex.pandoc_abi_version(),
    /** Convert with defaults-file options; `input` is the standard input. */
    convert(options, input = null) {
      return call(ex.pandoc_convert, options, input).output;
    },
    /** Convert, with JS functions as filters: each takes the document as
     *  pandoc's JSON and the context, and returns the new document. Named
     *  in `options.filters` as {type: "callback", index: i}; by default,
     *  all of them after the options' own filters. */
    convertWithFilters(options, input, fns) {
      // a filter may itself convert with filters: its own come after
      const base = filters.length;
      filters.push(...fns);
      const opts = { ...options };
      if (!opts.filters?.some((f) => f?.type === "callback")) {
        opts.filters = [...(opts.filters ?? []), ...fns.map((_, i) => ({ type: "callback", index: i }))];
      }
      // pandoc_filter[]: {fn = NULL, userdata = base + i}: pandoc's filter i is
      // the host's filters[base + i]
      const arr = ex.malloc(8 * fns.length);
      fns.forEach((_, i) => {
        view().setUint32(arr + 8 * i, 0, true);
        view().setUint32(arr + 8 * i + 4, base + i, true);
      });
      const [op, on] = bytes(JSON.stringify(opts));
      let ip = 0, iN = 0;
      if (input != null) [ip, iN] = bytes(input);
      try {
        return result(ex.pandoc_convert_filters(op, on, ip, iN, arr, fns.length)).output;
      } finally {
        ex.free(op);
        if (ip) ex.free(ip);
        ex.free(arr);
        filters.length = base;
      }
    },
    query(q) {
      const [p, n] = bytes(JSON.stringify(q));
      try {
        return JSON.parse(result(ex.pandoc_query(p, n)).output);
      } finally {
        ex.free(p);
      }
    },
    readMany(inputs, options = {}) {
      const [p, n] = bytes(JSON.stringify({ options, inputs }));
      try {
        return JSON.parse(result(ex.pandoc_read_many(p, n)).output);
      } finally {
        ex.free(p);
      }
    },
  };
}
