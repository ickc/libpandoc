// libpandoc.wasm from Python in Pyodide (in Node.js): Python calls pandoc
// synchronously through JS, and a panir filter runs inside the conversion.
//
//   PYODIDE=<dir with node_modules/pyodide> PANIR_WHEEL=<panir wheel> node wasm/test-pyodide.mjs
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { basename, join } from "node:path";
import { load } from "./libpandoc.mjs";

const require = createRequire(join(process.env.PYODIDE, "package.json"));
const { loadPyodide } = require("pyodide");
const path = process.env.LIBPANDOC_WASM ?? new URL("../dist/wasm/libpandoc.wasm", import.meta.url).pathname;

let t = performance.now();
const [pandoc, py] = await Promise.all([load(path), loadPyodide()]);
console.log(`libpandoc.wasm and Pyodide ${py.version} loaded in ${(performance.now() - t).toFixed(0)} ms`);

// What a Pyodide backend of libpandoc-python would call: JSON text in and
// out, so that nothing crosses as a proxy.
py.registerJsModule("_libpandoc_wasm", {
  convert: (options, input) => pandoc.convert(JSON.parse(options), input ?? null),
  convert_filters: (options, input, fns) =>
    pandoc.convertWithFilters(JSON.parse(options), input ?? null,
      fns.toJs().map((fn) => (doc, ctx) => JSON.parse(fn(JSON.stringify(doc), JSON.stringify(ctx))))),
  query: (q) => JSON.stringify(pandoc.query(JSON.parse(q))),
});

const wheel = process.env.PANIR_WHEEL;
py.FS.writeFile(`/tmp/${basename(wheel)}`, readFileSync(wheel));
await py.loadPackage("micropip");
await py.pyimport("micropip").install(`emfs:/tmp/${basename(wheel)}`);

t = performance.now();
py.runPython(`
import json
import _libpandoc_wasm as lp
import panir
from panir import Filter, Header, Str

assert lp.convert(json.dumps({"from": "markdown", "to": "html"}), "*hi*") == "<p><em>hi</em></p>\\n"
assert json.loads(lp.query(json.dumps({"query": "version"}))) == "3.11"

# read: markdown to a panir document
doc = panir.loads(lp.convert(json.dumps({"to": "json"}), "# Title\\n\\nhello *world*"))
assert isinstance(doc.blocks[0], Header)

# a panir filter inside one conversion, told the formats
f = Filter()
seen = {}

@f.on(Str)
def upper(s):
    return Str(s.text.upper())

@f.on(Header)
def demote(h):
    h.level += 1

def run(doc_json, ctx_json):
    seen.update(json.loads(ctx_json))
    return f.run_json(doc_json, seen.get("format"))

out = lp.convert_filters(json.dumps({"from": "markdown", "to": "html"}), "# Title\\n\\nhello *world*", [run])
assert out == '<h2 id="title">TITLE</h2>\\n<p>HELLO <em>WORLD</em></p>\\n', out
assert seen["input-format"] == "markdown", seen

# a filter that calls pandoc again (Python -> JS -> wasm -> JS -> Python -> JS -> wasm)
g = Filter()

@g.on(Str)
def parse(s):
    if s.text == "X":
        inner = panir.loads(lp.convert(json.dumps({"to": "json"}), "**inner**"))
        return inner.blocks[0].content

out = lp.convert_filters(json.dumps({"to": "html"}), "a X b", [lambda d, c: g.run_json(d)])
assert out == "<p>a <strong>inner</strong> b</p>\\n", out
print("Pyodide: all checks passed")
`);
console.log(`checks took ${(performance.now() - t).toFixed(0)} ms`);
