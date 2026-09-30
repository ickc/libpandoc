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

import { ConsoleStdout, File, OpenFile, WASI } from "@bjorn3/browser_wasi_shim";

const fromUtf8 = new TextDecoder("utf-8");
const toUtf8 = new TextEncoder();

class Refused extends Error {}

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
 *  WebAssembly.Module). `name` is its first argument (argv[0]);
 *  `preopens`: directories it may see (browser_wasi_shim
 *  PreopenDirectory); `pandocVersion`: for PANDOC_VERSION (by default,
 *  `pandoc`'s); `pandoc`: the libpandoc.wasm (core.mjs's) the filter's
 *  calls to pandoc go to; `stderr`: where its standard error's lines go.
 *  The result is a function on bytes, as core.mjs's convertWithFilters
 *  takes. */
export async function wasmFilter(source, { name = "filter.wasm", preopens = [], pandoc = null,
  pandocVersion = pandoc?.query({ query: "version" }) ?? null,
  stderr = (line) => console.warn(`[${name}] ${line}`) } = {}) {
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
    const status = wasi.start(instance);
    run.memory = Math.max(run.memory, instance.exports.memory.buffer.byteLength);
    if (status !== 0) throw new Error(`${name} exited with status ${status}`);
    return stdout.data;
  }
  run.bytes = true;
  run.memory = 0; // the most memory a run's instance had, in bytes
  return run;
}
