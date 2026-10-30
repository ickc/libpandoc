// libpandoc-python's own test suite, in Pyodide (in Node.js), on
// libpandoc.wasm: how much of the Python API works unchanged.
//
//   PYODIDE=<dir with node_modules/pyodide> PANIR_WHEEL=<panir wheel>
//   LIBPANDOC_PYTHON=<libpandoc-python checkout> node wasm/test-pyodide-suite.mjs [pytest args]
//
// WHEEL=<libpandoc-python's wheel for Pyodide>: that wheel, installed, loads
// libpandoc.wasm itself (`await libpandoc.load()`), instead of the
// checkout's package with a module this script loads
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
const wheel = process.env.WHEEL;
const py = await loadPyodide();
py.FS.mkdirTree("/src");
py.mountNodeFS("/src", src);
const pandoc = wheel ? null : shim
  ? await loadShim(readFileSync(path), { tmp: emscriptenDirectory(py.FS, "/tmp"), preopens: [emscriptenDirectory(py.FS, "/src")] })
  : await loadNode(path);
if (!wheel && !shim) {
  py.FS.mkdirTree("/tmp");
  py.mountNodeFS("/tmp", pandoc.tmp);
}
if (pandoc) py.registerJsModule("libpandoc_wasm", pandoc.abi);
py.FS.mkdirTree("/wheels");
await py.loadPackage(["micropip"]);
const micropip = py.pyimport("micropip");
for (const w of [process.env.PANIR_WHEEL, wheel].filter(Boolean)) {
  py.FS.writeFile(`/wheels/${basename(w)}`, readFileSync(w));
  await micropip.install(`emfs:/wheels/${basename(w)}`);
}
await micropip.install("pytest");
if (wheel) await py.runPythonAsync("import libpandoc; await libpandoc.load()");
const args = process.argv.slice(2).length ? process.argv.slice(2) : ["tests/test_api.py", "tests/test_ast.py"];
py.globals.set("ARGS", py.toPy(["-q", "-p", "no:cacheprovider", ...args]));
py.globals.set("SRC", !wheel);
const code = py.runPython(`
import os, sys
os.chdir("/src")
if SRC:
    sys.path.insert(0, "/src/src")
import pytest
int(pytest.main(list(ARGS)))
`);
process.exitCode = Number(code);
