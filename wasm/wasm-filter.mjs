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

import { ConsoleStdout, File, OpenFile, WASI } from "@bjorn3/browser_wasi_shim";

const fromUtf8 = new TextDecoder("utf-8");

/** A filter from a wasm module (its bytes, or a compiled
 *  WebAssembly.Module). `name` is its first argument (argv[0]);
 *  `preopens`: directories it may see (browser_wasi_shim
 *  PreopenDirectory); `pandocVersion`: for PANDOC_VERSION; `stderr`: where
 *  its standard error's lines go. The result is a function on bytes, as
 *  core.mjs's convertWithFilters takes. */
export async function wasmFilter(source, { name = "filter.wasm", preopens = [], pandocVersion = null,
  stderr = (line) => console.warn(`[${name}] ${line}`) } = {}) {
  const module = source instanceof WebAssembly.Module ? source : await WebAssembly.compile(source);
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
    const instance = new WebAssembly.Instance(module, { wasi_snapshot_preview1: wasi.wasiImport });
    const status = wasi.start(instance);
    if (status !== 0) throw new Error(`${name} exited with status ${status}`);
    return stdout.data;
  }
  run.bytes = true;
  return run;
}
