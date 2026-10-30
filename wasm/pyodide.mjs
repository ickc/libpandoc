// libpandoc.wasm for libpandoc-python in Pyodide: loads it with Pyodide's
// filesystem as pandoc's (its / and /tmp, through emscripten-fs.mjs), and
// registers its interface as the JS module libpandoc_wasm, which the
// package's _wasm backend imports. libpandoc-python's wheel for Pyodide
// carries this file bundled (`scripts/bundle-pyodide.sh`, with
// browser_wasi_shim) and calls it from `await libpandoc.load()`:
//
//   import { loadForPyodide } from "./pyodide.mjs";
//   await loadForPyodide(pyodide, wasmUrlOrBytes);

import { load } from "./browser.mjs";
import { emscriptenDirectory } from "./emscripten-fs.mjs";

export { PandocError, StoppedError } from "./core.mjs";

/** `pyodide`: the Pyodide API (`pyodide_js` from Python). `source`: as
 *  browser.mjs's `load` takes it (a URL, a Response, bytes or a compiled
 *  module). Returns the instance, as `load` does. */
export async function loadForPyodide(pyodide, source) {
  const FS = pyodide.FS;
  FS.mkdirTree("/tmp");
  const pandoc = await load(source, {
    tmp: emscriptenDirectory(FS, "/tmp"),
    // the rest of Pyodide's filesystem, at the same paths
    preopens: [emscriptenDirectory(FS, "/")],
  });
  pyodide.registerJsModule("libpandoc_wasm", pandoc.abi);
  return pandoc;
}
