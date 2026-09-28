// Benchmark: the same filters as Lua, as panir in JavaScript, and as panir
// in Python (Pyodide), each inside one libpandoc.wasm conversion, in
// browsers (Playwright), all in a Web Worker.
//
//   NODE_MODULES=<playwright, @bjorn3/browser_wasi_shim, pyodide>
//   PANIR=<panir checkout, with ts/dist built> PANIR_WHEEL=<panir wheel>
//   DOC=<markdown file, e.g. pandoc's MANUAL.txt>
//   node wasm/bench-browser.mjs [chromium firefox webkit]
//
// Workloads (the first two are panir's corpus scenarios, whose Lua, TS and
// Python versions are tested to make the same document):
//   upper:  every Str replaced by an upper-cased one
//   modify: headers demoted, link and image targets and code classes changed
//   count:  every Str counted, a paragraph with the count added (stateful)
// Each also runs with an identity filter that doesn't parse the JSON (raw),
// which is the cost of crossing into the language and back.
import { createReadStream, existsSync, readFileSync } from "node:fs";
import { createServer } from "node:http";
import { createRequire } from "node:module";
import { basename, extname, join, normalize } from "node:path";

const nm = process.env.NODE_MODULES;
const panir = process.env.PANIR;
const wheel = process.env.PANIR_WHEEL;
const doc = readFileSync(process.env.DOC, "utf8");
const runs = +(process.env.RUNS ?? 5);
const require = createRequire(join(nm, "playwright", "package.json"));
const playwright = require("playwright");
const here = new URL(".", import.meta.url).pathname;
const wasm = process.env.LIBPANDOC_WASM ?? join(here, "../dist/wasm/libpandoc.wasm");

const lua = {
  upper: `return {{ Str = function(s) return pandoc.Str(s.text:upper()) end }}`,
  modify: `return {{
    Header = function(h) h.level = h.level + 1; return h end,
    Link = function(l) l.target = "https://example.org/" .. l.target; return l end,
    Image = function(i) i.src = "img/" .. i.src; i.attributes.loading = "lazy"; return i end,
    CodeBlock = function(c) c.classes:insert("numbered"); return c end,
  }}`,
  count: `local n = 0
  return {{
    Str = function(s) n = n + 1 end,
    Pandoc = function(doc) doc.blocks:insert(pandoc.Para(pandoc.Str(tostring(n)))); return doc end,
  }}`,
};

// the harness both workers share: timing, and the conversions
const common = `
const doc = await (await fetch("/doc.md")).text();
const workloads = ["upper", "modify", "count"];
const RUNS = ${runs};
function median(xs) { const s = [...xs].sort((a, b) => a - b); return s[Math.floor(s.length / 2)]; }
function time(f) {
  f(); // warm up
  const ts = [];
  for (let i = 0; i < RUNS; i++) { const t = performance.now(); f(); ts.push(performance.now() - t); }
  return median(ts);
}
const opts = { from: "markdown", to: "html" };
`;

const jsWorker = `
import { load } from "/wasm/browser.mjs";
import { applyFilter, parse, serialize, Str, Para } from "/panir/index.js";
${common}
const luaFiles = ${JSON.stringify(Object.fromEntries(Object.entries(lua).map(([k, v]) => [`/tmp/${k}.lua`, v])))};
let t = performance.now();
const pandoc = await load("/libpandoc.wasm", { files: luaFiles });
const loadMs = performance.now() - t;
const filters = {
  upper: () => ({ Str: (s) => Str(s.text.toUpperCase()) }),
  modify: () => ({
    Header: (h) => { h.level += 1; },
    Link: (l) => { l.target.url = "https://example.org/" + l.target.url; },
    Image: (i) => { i.target.url = "img/" + i.target.url; i.attr.attributes.push(["loading", "lazy"]); },
    CodeBlock: (c) => { c.attr.classes.push("numbered"); },
  }),
  count: () => { let n = 0; return { Str: () => { n += 1; }, Pandoc: (d) => { d.blocks.push(Para([Str(String(n))])); return d; } }; },
};
const out = {};
const results = { load: loadMs, none: time(() => pandoc.convert(opts, doc)) };
results.raw = time(() => pandoc.convertWithFilters(opts, doc, [(text) => text], { raw: true }));
for (const w of workloads) {
  results["lua " + w] = time(() => (out["lua " + w] = pandoc.convert({ ...opts, filters: ["/tmp/" + w + ".lua"] }, doc)));
  results["js " + w] = time(() => (out["js " + w] = pandoc.convertWithFilters(opts, doc,
    [(text, ctx) => serialize(applyFilter(parse(text), filters[w](), JSON.parse(ctx).format))], { raw: true })));
}
const json = pandoc.convert({ from: "markdown", to: "json" }, doc);
results["json only"] = time(() => serialize(parse(json)));
for (const w of workloads) results["js alone " + w] = time(() => serialize(applyFilter(parse(json), filters[w](), "html")));
postMessage({ results, out });
`;

const pyWorker = `
import { load } from "/wasm/browser.mjs";
import { loadPyodide } from "/pyodide/pyodide.mjs";
${common}
let t = performance.now();
const pandoc = await load("/libpandoc.wasm");
const loadMs = performance.now() - t;
t = performance.now();
const py = await loadPyodide({ indexURL: "/pyodide/" });
await py.loadPackage("micropip");
await py.pyimport("micropip").install(location.origin + "/wheels/${basename(wheel)}");
const pyLoadMs = performance.now() - t;
// what a Pyodide backend of libpandoc-python calls: JSON text both ways
py.registerJsModule("_libpandoc_wasm", {
  convert_filters: (options, input, fns) => {
    const list = fns.toJs();
    try { return pandoc.convertWithFilters(JSON.parse(options), input, list.map((f) => (d, c) => f(d, c)), { raw: true }); }
    finally { list.forEach((f) => f.destroy()); fns.destroy(); }
  },
});
py.runPython(\`
import json
import _libpandoc_wasm as lp
from panir import Filter, Para, Str, Header, Link, Image, CodeBlock, Pandoc

def upper():
    f = Filter()
    @f.on(Str)
    def up(s):
        return Str(s.text.upper())
    return f

def modify():
    f = Filter()
    @f.on(Header)
    def demote(h):
        h.level += 1
    @f.on(Link)
    def link(x):
        x.target.url = "https://example.org/" + x.target.url
    @f.on(Image)
    def image(i):
        i.target.url = "img/" + i.target.url
        i.attr.attributes.append(("loading", "lazy"))
    @f.on(CodeBlock)
    def code(c):
        c.attr.classes.append("numbered")
    return f

def count():
    f = Filter()
    n = 0
    @f.on(Str)
    def s(x):
        nonlocal n
        n += 1
    @f.on(Pandoc)
    def pandoc(doc):
        doc.blocks.append(Para(Str(str(n))))
    return f

FILTERS = {"upper": upper, "modify": modify, "count": count}
OPTS = json.dumps({"from": "markdown", "to": "html"})

import time

def alone(name, text, runs):
    ts = []
    for i in range(runs + 1):
        t = time.perf_counter()
        if name == "json only":
            panir.dumps(panir.loads(text))
        else:
            FILTERS[name]().run_json(text, "html")
        ts.append((time.perf_counter() - t) * 1000)
    ts = sorted(ts[1:])
    return ts[len(ts) // 2]

import panir

def run(name, doc):
    if name == "raw":
        return lp.convert_filters(OPTS, doc, [lambda d, c: d])
    f = FILTERS[name]()
    return lp.convert_filters(OPTS, doc, [lambda d, c: f.run_json(d, json.loads(c)["format"])])
\`);
const run = py.globals.get("run");
const out = {};
const results = { load: loadMs, "pyodide + panir load": pyLoadMs };
results.raw = time(() => run("raw", doc));
for (const w of workloads) results["py " + w] = time(() => (out["py " + w] = run(w, doc)));
const json = pandoc.convert({ from: "markdown", to: "json" }, doc);
const alone = py.globals.get("alone");
results["json only"] = alone("json only", json, RUNS);
for (const w of workloads) results["py alone " + w] = alone(w, json, RUNS);
postMessage({ results, out });
`;

const page = (worker) => `<!doctype html><script type="module">
const w = new Worker("/${worker}.mjs", { type: "module" });
w.onmessage = (e) => { window.result = e.data; };
w.onerror = (e) => { window.result = { error: (e.message || "worker error") + " " + (e.filename ?? "") + ":" + (e.lineno ?? "") }; };
</script>`;

const types = { ".mjs": "text/javascript", ".js": "text/javascript", ".wasm": "application/wasm", ".json": "application/json", ".whl": "application/zip", ".zip": "application/zip" };
const server = createServer((req, res) => {
  const url = decodeURIComponent(new URL(req.url, "http://x").pathname);
  const send = (type, body) => { res.writeHead(200, { "content-type": type }); res.end(body); };
  if (url === "/js" || url === "/py") return send("text/html", page(url.slice(1)));
  if (url === "/js.mjs") return send("text/javascript", jsWorker);
  if (url === "/py.mjs") return send("text/javascript", pyWorker);
  if (url === "/doc.md") return send("text/plain", doc);
  if (url === "/wasm/browser.mjs") {
    return send("text/javascript", readFileSync(join(here, "browser.mjs"), "utf8")
      .replace('"@bjorn3/browser_wasi_shim"', '"/@bjorn3/browser_wasi_shim/dist/index.js"'));
  }
  let file;
  if (url === "/libpandoc.wasm") file = wasm;
  else if (url.startsWith("/wasm/")) file = join(here, normalize(url.slice(6)));
  else if (url.startsWith("/panir/")) file = join(panir, "ts/dist", normalize(url.slice(7)));
  else if (url.startsWith("/pyodide/")) file = join(nm, "pyodide", normalize(url.slice(9)));
  else if (url.startsWith("/wheels/")) file = wheel;
  else if (url.startsWith("/@bjorn3/")) file = join(nm, normalize(url.slice(1)));
  if (!file || !existsSync(file)) { res.writeHead(404); return res.end(); }
  res.writeHead(200, { "content-type": types[extname(file)] ?? "application/octet-stream" });
  createReadStream(file).pipe(res);
}).listen(0);
const base = `http://localhost:${server.address().port}`;

async function measure(browser, which) {
  const p = await browser.newPage();
  p.on("console", (m) => { if (m.type() === "error") console.log(`  console: ${m.text()}`); });
  await p.goto(`${base}/${which}`);
  try {
    await p.waitForFunction(() => window.result, null, { timeout: 900000, polling: 1000 });
    return await p.evaluate(() => window.result);
  } catch (e) {
    return { error: e.message.split("\n")[0] };
  } finally {
    await p.close();
  }
}

const kb = (Buffer.byteLength(doc) / 1024).toFixed(0);
for (const name of process.argv.slice(2).length ? process.argv.slice(2) : ["chromium", "firefox", "webkit"]) {
  const browser = await playwright[name].launch();
  const js = await measure(browser, "js");
  const py = await measure(browser, "py");
  console.log(`== ${name} ${browser.version()} (${kb} KB of markdown to HTML, median of ${runs} runs, ms)`);
  for (const r of [js, py]) if (r.error) console.log(`  error: ${r.error}`);
  if (!js.error && !py.error) {
    const J = js.results, P = py.results;
    console.log(`  load: libpandoc.wasm ${J.load.toFixed(0)}; Pyodide + micropip + panir ${P["pyodide + panir load"].toFixed(0)}`);
    console.log(`  no filter ${J.none.toFixed(0)}; identity filter (raw JSON): JS ${J.raw.toFixed(0)}, Python ${P.raw.toFixed(0)}`);
    console.log(`  ${"workload".padEnd(8)} ${"Lua".padStart(6)} ${"JS".padStart(6)} ${"Python".padStart(7)}  same output`);
    for (const w of ["upper", "modify", "count"]) {
      const same = js.out["lua " + w] === js.out["js " + w] && js.out["js " + w] === py.out["py " + w];
      console.log(`  ${w.padEnd(8)} ${J["lua " + w].toFixed(0).padStart(6)} ${J["js " + w].toFixed(0).padStart(6)} ${P["py " + w].toFixed(0).padStart(7)}  ${same ? "yes" : "NO"}`);
    }
    console.log(`  the filter alone, on the document's JSON (parse, filter, serialize):`);
    console.log(`  ${"json only".padEnd(8)} ${"".padStart(6)} ${J["json only"].toFixed(0).padStart(6)} ${P["json only"].toFixed(0).padStart(7)}`);
    for (const w of ["upper", "modify", "count"]) {
      console.log(`  ${w.padEnd(8)} ${"".padStart(6)} ${J["js alone " + w].toFixed(0).padStart(6)} ${P["py alone " + w].toFixed(0).padStart(7)}`);
    }
  }
  await browser.close();
}
server.close();
