// libpandoc.wasm in Node.js, with node:wasi.
//
//   import { load } from "./node.mjs";
//   const pandoc = await load("dist/wasm/libpandoc.wasm");
//   pandoc.convert({ from: "markdown", to: "html" }, "*hi*");
//   pandoc.close(); // or `using pandoc = await load(...)`
//
// pandoc writes its temporary files in /tmp, a real directory preopened for
// it (pandoc.tmp), made for this instance and removed with it: by close(),
// else when the instance is garbage-collected, else at the process's exit.
// `preopens` adds others, e.g. { "/work": process.cwd() }.

import { mkdtempSync, rmSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { WASI } from "node:wasi";
import { start } from "./core.mjs";

export { PandocError, StoppedError } from "./core.mjs";

// the instances' directories not removed yet
const live = new Set();
const remove = (dir) => {
  if (!live.delete(dir)) return;
  try {
    rmSync(dir, { recursive: true, force: true });
  } catch {
    // at exit, or a directory someone else holds: nothing more to do
  }
};
// an instance dropped without close()
const dropped = new FinalizationRegistry(remove);
process.once("exit", () => [...live].forEach(remove));

/** `source`: the module's path, or a compiled WebAssembly.Module (a
 *  previous instance's `module`: loading again from it skips compiling).
 *  `preopens`: more directories, as node:wasi's (node:wasi follows
 *  symbolic links in them wherever they point: README.md). */
export async function load(source, { preopens } = {}) {
  const module = source instanceof WebAssembly.Module
    ? source
    : await WebAssembly.compile(await readFile(source));
  const tmp = mkdtempSync(join(tmpdir(), "libpandoc-wasm-"));
  live.add(tmp);
  let pandoc;
  try {
    const wasi = new WASI({
      version: "preview1",
      args: ["libpandoc"],
      // PATH: none (pandoc looks for programs, such as a PDF engine, on it;
      // WASI runs none)
      env: { TMPDIR: "/tmp", PATH: "" },
      preopens: { "/tmp": tmp, ...preopens },
      returnOnExit: true,
    });
    pandoc = await start(module, {
      imports: wasi.getImportObject(),
      initialize: (instance) => wasi.initialize(instance),
    });
  } catch (e) {
    remove(tmp);
    throw e;
  }
  // (the exports are what every function of the instance holds)
  dropped.register(pandoc.exports, tmp, pandoc.exports);
  /** Stop this instance and remove its /tmp: every later call throws a
   *  StoppedError. */
  const close = () => {
    pandoc.close();
    dropped.unregister(pandoc.exports);
    remove(tmp);
  };
  return { ...pandoc, tmp, module, close, [Symbol.dispose]: close };
}
