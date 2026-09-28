// libpandoc.wasm in the browser (or any JS engine without node:wasi), with
// @bjorn3/browser_wasi_shim, as npm's pandoc-wasm uses. /tmp is in memory.
//
//   import { load } from "./browser.mjs";
//   const pandoc = await load(new URL("libpandoc.wasm", import.meta.url));
//
// Conversions block: run it in a Web Worker in a page.

import { ConsoleStdout, File, OpenFile, PreopenDirectory, WASI } from "@bjorn3/browser_wasi_shim";
import { start } from "./core.mjs";

export { PandocError } from "./core.mjs";

/** `source`: a URL (fetched and compiled as it streams), a Response, or the
 *  module's bytes. `files`: files pandoc may read, {"/tmp/name": bytes}. */
export async function load(source, { files = {} } = {}) {
  const tmp = new Map();
  for (const [path, data] of Object.entries(files)) {
    if (!path.startsWith("/tmp/")) throw new Error(`files go in /tmp: ${path}`);
    tmp.set(path.slice(5), new File(typeof data === "string" ? new TextEncoder().encode(data) : data));
  }
  const fds = [
    new OpenFile(new File([])), // stdin
    ConsoleStdout.lineBuffered((line) => console.log(`[libpandoc] ${line}`)),
    ConsoleStdout.lineBuffered((line) => console.warn(`[libpandoc] ${line}`)),
    new PreopenDirectory("/tmp", tmp),
  ];
  const wasi = new WASI(["libpandoc"], ["TMPDIR=/tmp"], fds);
  const module = source instanceof URL || typeof source === "string"
    ? await WebAssembly.compileStreaming(fetch(source))
    : source instanceof Response
      ? await WebAssembly.compileStreaming(source)
      : await WebAssembly.compile(source);
  const pandoc = await start(module, {
    imports: { wasi_snapshot_preview1: wasi.wasiImport },
    initialize: (instance) => wasi.initialize(instance),
  });
  return { ...pandoc, tmp };
}
