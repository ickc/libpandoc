// libpandoc-python's own test suite, in Pyodide (in Node.js), on
// libpandoc.wasm: how much of the Python API works unchanged.
//
//   PYODIDE=<dir with node_modules/pyodide> PANIR_WHEEL=<panir wheel>
//   LIBPANDOC_PYTHON=<libpandoc-python checkout> node wasm/test-pyodide-suite.mjs [pytest args]
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { basename, join } from "node:path";
import { load as loadNode } from "./node.mjs";
import { load as loadShim } from "./browser.mjs";
import { emscriptenDirectory } from "./emscripten-fs.mjs";

const require = createRequire(join(process.env.PYODIDE, "package.json"));
const { loadPyodide } = require("pyodide");
const path = process.env.LIBPANDOC_WASM ?? new URL("../dist/wasm/libpandoc.wasm", import.meta.url).pathname;
const src = process.env.LIBPANDOC_PYTHON;

// SHIM=1: as in a browser, browser_wasi_shim with pandoc's /tmp and /src
// being Pyodide's (emscripten-fs.mjs); else node:wasi, with one host
// directory as /tmp in both
const shim = process.env.SHIM === "1";
const py = await loadPyodide();
py.FS.mkdirTree("/src");
py.mountNodeFS("/src", src);
const pandoc = shim
  ? await loadShim(readFileSync(path), { tmp: emscriptenDirectory(py.FS, "/tmp"), preopens: [emscriptenDirectory(py.FS, "/src")] })
  : await loadNode(path);
if (!shim) {
  py.FS.mkdirTree("/tmp");
  py.mountNodeFS("/tmp", pandoc.tmp);
}
py.registerJsModule("libpandoc_wasm", pandoc.abi);
const wheel = process.env.PANIR_WHEEL;
py.FS.mkdirTree("/wheels");
py.FS.writeFile(`/wheels/${basename(wheel)}`, readFileSync(wheel));
await py.loadPackage(["micropip"]);
const micropip = py.pyimport("micropip");
await micropip.install(`emfs:/wheels/${basename(wheel)}`);
await micropip.install("pytest");
const args = process.argv.slice(2).length ? process.argv.slice(2) : ["tests/test_api.py", "tests/test_ast.py"];
py.globals.set("ARGS", py.toPy(["-q", "-p", "no:cacheprovider", ...args]));
const code = py.runPython(`
import os, sys
os.chdir("/src")
sys.path.insert(0, "/src/src")
import pytest
pytest.main(list(ARGS))
`);
process.exitCode = Number(code);
