// Filters compiled to WebAssembly, run by the JS engine inside a
// libpandoc.wasm conversion, in the browser or Node.
//
// A wasm filter is a pandoc JSON filter built for WASI (`wasm32-wasip1`): a
// command reading the document as pandoc's JSON on stdin and writing the new
// one to stdout, told the output format as its first argument and the rest
// in the environment, as pandoc tells a JSON filter. The same file runs in
// pandocrs (wasmtime) and, through a wasm runtime, under pandoc itself.
//
//   import { wasmFilter } from "./wasm-filter.mjs";
//   const upper = await wasmFilter(await (await fetch("upper.wasm")).arrayBuffer());
//   pandoc.convertWithFilters({ from: "markdown", to: "html" }, "hi", [upper]);
//
// It runs synchronously, as pandoc calls filters: a fresh instance of the
// compiled module per run, with an in-memory stdin and stdout
// (@bjorn3/browser_wasi_shim, pure JS, so the same in Node), and only the
// directories given.
//
// A filter may call pandoc (libpandoc-rs's `libpandoc::read` and the like,
// built for wasm): given `pandoc` (core.mjs's, from node.mjs or
// browser.mjs), its `libpandoc` imports are this libpandoc.wasm's calls,
// marked untrusted (below), so that libpandoc allows them only pandoc's
// sandbox and no options that read or write files or run programs.
//
// Limits: `maxMemory` (bytes; in Node, by default $LIBPANDOC_WASM_MAX_MEMORY,
// bytes or with k, m or g, as the other hosts) caps the filter's memory: the
// module's own maximum is lowered to it before it is compiled, so its memory
// can't grow further. There is no timeout here, as the other hosts have: a
// filter runs synchronously on the conversion's thread, which nothing can
// interrupt; run the conversion in a Worker and terminate it for one.

import { ConsoleStdout, File, OpenFile, WASI } from "@bjorn3/browser_wasi_shim";

const fromUtf8 = new TextDecoder("utf-8");
const toUtf8 = new TextEncoder();

class Refused extends Error {}

const PAGE = 65536;

/** A size in bytes, or with k, m or g ($LIBPANDOC_WASM_MAX_MEMORY): null if
 *  empty or 0. */
export function parseMemory(s) {
  s = (s ?? "").trim();
  if (!s) return null;
  const m = /^([0-9]+)([kKmMgG]?)$/.exec(s);
  if (!m) throw new Error(`LIBPANDOC_WASM_MAX_MEMORY: bytes, or with k, m or g, not ${JSON.stringify(s)}`);
  const n = Number(m[1]) * 2 ** { "": 0, k: 10, m: 20, g: 30 }[m[2].toLowerCase()];
  return n || null;
}

const leb = (b, p) => {
  let n = 0, shift = 0, byte;
  do {
    byte = b[p++];
    n += (byte & 0x7f) * 2 ** shift;
    shift += 7;
  } while (byte & 0x80);
  return [n, p];
};
const uleb = (n) => {
  const out = [];
  do {
    let byte = n % 128;
    n = Math.floor(n / 128);
    if (n) byte |= 0x80;
    out.push(byte);
  } while (n);
  return out;
};

/** The module `bytes` with its memory's maximum at most `max` bytes. */
export function withMaxMemory(bytes, max, name = "filter.wasm") {
  const b = new Uint8Array(bytes.buffer ?? bytes, bytes.byteOffset ?? 0, bytes.byteLength);
  const pages = Math.floor(max / PAGE);
  const out = [b.subarray(0, 8)];
  let found = false;
  for (let p = 8; p < b.length;) {
    const id = b[p];
    const [size, start] = leb(b, p + 1);
    const end = start + size;
    if (id === 5) {
      let [count, q] = leb(b, start);
      const body = [...uleb(count)];
      for (let i = 0; i < count; i++) {
        const flags = b[q++];
        if (flags & ~1) throw new Error(`${name}: can't limit a shared or 64-bit memory`);
        let min, own = null;
        [min, q] = leb(b, q);
        if (flags & 1) [own, q] = leb(b, q);
        if (min > pages) throw new Error(`${name} needs ${min * PAGE} bytes of memory to start, over its limit of ${max}`);
        body.push(1, ...uleb(min), ...uleb(own === null ? pages : Math.min(own, pages)));
      }
      out.push(Uint8Array.of(5, ...uleb(body.length), ...body));
      found = true;
    } else {
      out.push(b.subarray(p, end));
    }
    p = end;
  }
  if (!found) throw new Error(`${name}: no memory of its own to limit`);
  const all = new Uint8Array(out.reduce((n, a) => n + a.length, 0));
  let at = 0;
  for (const a of out) { all.set(a, at); at += a.length; }
  return all;
}

/** What a filter gives pandoc (`options` or a query), marked for libpandoc
 *  to check (`"untrusted": true`, libpandoc 1.7): it accepts only what reads
 *  and writes no files and runs nothing, with pandoc's sandbox on. */
function untrusted(abi, what) {
  if (abi.abiVersion() < 1007) throw new Refused("a wasm filter calling pandoc needs libpandoc 1.7 (\"untrusted\")");
  if (what === null || typeof what !== "object" || Array.isArray(what))
    throw new Refused("not allowed for untrusted code: options that aren't an object");
  return { ...what, untrusted: true };
}

// The `libpandoc` imports (libpandoc-rs's guest.rs) on core.mjs's abi;
// `memory()` is the filter's.
function libpandocImports(abi, memory) {
  let result = null; // [status, output, kind, message, log], as abi's
  const bytes = (p, n) => new Uint8Array(memory().buffer, p, n).slice();
  const json = (p, n) => JSON.parse(fromUtf8.decode(bytes(p, n)));
  const answer = (f) => {
    try {
      result = f();
    } catch (e) {
      const kind = e instanceof Refused || e instanceof SyntaxError ? "PandocOptionError" : "Exception";
      result = [1, new Uint8Array(), kind, e.message, "[]"];
    }
    return result[0] === 0 ? 0 : 1;
  };
  const part = (i) => {
    if (!result) return new Uint8Array();
    const [status, output, kind, message, log] = result;
    const v = status === 0 ? [output, log, "", ""][i] : ["", "[]", kind ?? "Exception", message ?? ""][i];
    return typeof v === "string" ? toUtf8.encode(v) : (v ?? new Uint8Array());
  };
  return {
    convert(o, on, i, iN, has) {
      return answer(() => {
        const options = untrusted(abi, json(o, on));
        if (!has) throw new Refused("a wasm filter gives convert its input");
        return abi.convert(JSON.stringify(options), bytes(i, iN));
      });
    },
    read_many(p, n) {
      return answer(() => {
        const req = json(p, n);
        req.options = untrusted(abi, req?.options ?? {});
        return abi.readMany(JSON.stringify(req));
      });
    },
    query(p, n) {
      return answer(() => {
        return abi.query(JSON.stringify(untrusted(abi, json(p, n))));
      });
    },
    result_len: (i) => part(i).length,
    result_read(i, to) {
      const b = part(i);
      new Uint8Array(memory().buffer, to, b.length).set(b);
    },
  };
}

/** A filter from a wasm module (its bytes, or a compiled
 *  WebAssembly.Module, which can't be given a `maxMemory`). `name` is its
 *  first argument (argv[0]); `maxMemory`: the most memory it may have, in
 *  bytes (null: none; by default, in Node, $LIBPANDOC_WASM_MAX_MEMORY);
 *  `preopens`: directories it may see (browser_wasi_shim
 *  PreopenDirectory); `pandocVersion`: for PANDOC_VERSION (by default,
 *  `pandoc`'s); `pandoc`: the libpandoc.wasm (core.mjs's) the filter's
 *  calls to pandoc go to; `stderr`: where its standard error's lines go.
 *  The result is a function on bytes, as core.mjs's convertWithFilters
 *  takes. */
export async function wasmFilter(source, { name = "filter.wasm", preopens = [], pandoc = null,
  pandocVersion = pandoc?.query({ query: "version" }) ?? null,
  stderr = (line) => console.warn(`[${name}] ${line}`),
  maxMemory = parseMemory(globalThis.process?.env?.LIBPANDOC_WASM_MAX_MEMORY) } = {}) {
  if (maxMemory != null && source instanceof WebAssembly.Module)
    throw new Error(`${name}: maxMemory needs the module's bytes, not a compiled module`);
  if (maxMemory != null) source = withMaxMemory(source, maxMemory, name);
  const module = source instanceof WebAssembly.Module ? source : await WebAssembly.compile(source);
  const callsPandoc = WebAssembly.Module.imports(module).some((i) => i.module === "libpandoc");
  if (callsPandoc && !pandoc) throw new Error(`${name} calls pandoc: give wasmFilter the pandoc it calls`);
  function run(doc, context) {
    const ctx = JSON.parse(fromUtf8.decode(context));
    const env = [`PANDOC_READER_OPTIONS=${JSON.stringify(ctx["reader-options"] ?? {})}`];
    if (pandocVersion) env.push(`PANDOC_VERSION=${pandocVersion}`);
    if (ctx["input-format"]) env.push(`PANDOC_INPUT_FORMAT=${ctx["input-format"]}`);
    if (ctx["output-format"]) env.push(`PANDOC_OUTPUT_FORMAT=${ctx["output-format"]}`);
    const stdout = new File([]);
    const fds = [
      new OpenFile(new File(doc)),
      new OpenFile(stdout),
      ConsoleStdout.lineBuffered(stderr),
      ...preopens,
    ];
    // debug: false, or the shim logs every call (its default when omitted)
    const wasi = new WASI([name, ctx.format ?? ""], env, fds, { debug: false });
    const imports = { wasi_snapshot_preview1: wasi.wasiImport };
    let instance;
    if (callsPandoc) imports.libpandoc = libpandocImports(pandoc.abi, () => instance.exports.memory);
    instance = new WebAssembly.Instance(module, imports);
    let status;
    try {
      status = wasi.start(instance);
    } catch (e) {
      if (maxMemory == null || !(e instanceof WebAssembly.RuntimeError)) throw e;
      throw new Error(`${name}: ${e.message} (its memory limit: ${maxMemory} bytes, LIBPANDOC_WASM_MAX_MEMORY)`);
    }
    run.memory = Math.max(run.memory, instance.exports.memory.buffer.byteLength);
    if (status !== 0) {
      const limit = maxMemory == null ? "" : ` (its memory limit: ${maxMemory} bytes, LIBPANDOC_WASM_MAX_MEMORY)`;
      throw new Error(`${name} exited with status ${status}${limit}`);
    }
    return stdout.data;
  }
  run.bytes = true;
  run.memory = 0; // the most memory a run's instance had, in bytes
  return run;
}
