// libpandoc.wasm in Node.js, with node:wasi.
//
//   import { load } from "./node.mjs";
//   const pandoc = await load("dist/wasm/libpandoc.wasm");
//   pandoc.convert({ from: "markdown", to: "html" }, "*hi*");
//
// pandoc writes its temporary files in /tmp, a real directory preopened for
// it (pandoc.tmp); `preopens` adds others, e.g. { "/work": process.cwd() }.

import { mkdtempSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { WASI } from "node:wasi";
import { start } from "./core.mjs";

export { PandocError } from "./core.mjs";

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
  const pandoc = await start(module, {
    imports: wasi.getImportObject(),
    initialize: (instance) => wasi.initialize(instance),
  });
  return { ...pandoc, tmp };
}
