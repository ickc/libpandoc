// libpandoc.wasm in browsers (Chromium, Firefox, WebKit) with Playwright:
// the same checks as test.mjs, in a Web Worker, through browser.mjs.
//
//   npm install playwright @bjorn3/browser_wasi_shim && npx playwright install
//   NODE_MODULES=<its node_modules> node wasm/test-browser.mjs [chromium firefox webkit]
import { createReadStream, existsSync, readFileSync } from "node:fs";
import { createServer } from "node:http";
import { createRequire } from "node:module";
import { extname, join, normalize } from "node:path";

const nm = process.env.NODE_MODULES;
const require = createRequire(join(nm, "playwright", "package.json"));
const playwright = require("playwright");
const here = new URL(".", import.meta.url).pathname;
const wasm = process.env.LIBPANDOC_WASM ?? join(here, "../dist/wasm/libpandoc.wasm");
// libpandoc-rs's example filters built for wasm32-wasip1, if given: a Rust
// filter run as wasm inside the conversion (wasm-filter.mjs)
const wasmFilters = process.env.WASM_FILTERS;

const worker = `
import { load } from "/wasm/browser.mjs";
const md = (n, unit, sep) => Array.from({ length: n }, () => unit).join(sep);
const results = [];
function check(name, f) {
  try { const r = f(); results.push({ name, ok: r === true, detail: r === true ? "" : String(r) }); }
  catch (e) { results.push({ name, ok: false, detail: String(e?.message ?? e) }); }
}
const wasmFilters = ${JSON.stringify(Boolean(wasmFilters))};
const t0 = performance.now();
const pandoc = await load("/libpandoc.wasm", { files: { "/tmp/upper.lua": "function Str(e) return pandoc.Str(e.text:upper()) end" } });
const loadMs = performance.now() - t0;
check("convert", () => pandoc.convert({ from: "markdown", to: "html" }, "# Hi\\n\\n*a*") === '<h1 id="hi">Hi</h1>\\n<p><em>a</em></p>\\n');
check("error kind", () => { try { pandoc.convert({ from: "nonsense" }, "x"); return "no error"; } catch (e) { return e.kind === "PandocUnknownReaderError" || e.kind; } });
check("query", () => pandoc.query({ query: "version" }) === "3.11");
check("read many", () => pandoc.readMany(["*a*"], {})[0].blocks[0].c[0].t === "Emph");
check("Lua filter", () => pandoc.convert({ to: "plain", filters: ["/tmp/upper.lua"] }, "hi") === "HI\\n");
check("JS filter", () => pandoc.convertWithFilters({ to: "plain" }, "hi", [(d) => { d.blocks[0].c[0].c = "yo"; return d; }]) === "yo\\n");
check("2000 plain words (crashes Node 22)", () => pandoc.convert({ to: "html" }, md(2000, "word", " ")).length === 10007);
if (wasmFilters) {
  const { wasmFilter } = await import("/wasm/wasm-filter.mjs");
  const bytes = await (await fetch("/filters/upper.wasm")).arrayBuffer();
  const upper = await wasmFilter(bytes, { name: "upper.wasm" });
  check("wasm filter (Rust)", () => pandoc.convertWithFilters({ to: "plain" }, "hello *world*", [upper]) === "HELLO WORLD\\n");
}
let t = performance.now();
check("800 paragraphs", () => pandoc.convert({ to: "html" }, md(800, "Paragraph with *emphasis* and [a link](u).", "\\n\\n")).length > 0);
const convertMs = performance.now() - t;
postMessage({ results, loadMs, convertMs });
`;

const page = `<!doctype html><script type="module">
const w = new Worker("/worker.mjs", { type: "module" });
w.onmessage = (e) => { window.result = e.data; };
w.onerror = (e) => { window.result = { error: (e.message || "worker error") + " " + (e.filename ?? "") + ":" + (e.lineno ?? "") }; };
</script>`;

const types = { ".mjs": "text/javascript", ".js": "text/javascript", ".wasm": "application/wasm" };
const server = createServer((req, res) => {
  const url = new URL(req.url, "http://x").pathname;
  let file;
  if (url === "/") { res.writeHead(200, { "content-type": "text/html" }); return res.end(page); }
  if (url === "/worker.mjs") { res.writeHead(200, { "content-type": "text/javascript" }); return res.end(worker); }
  if (url === "/wasm/browser.mjs" || url === "/wasm/wasm-filter.mjs") {
    // the bare import, which a page would resolve with an import map or a bundler
    res.writeHead(200, { "content-type": "text/javascript" });
    return res.end(readFileSync(join(here, url.slice(6)), "utf8")
      .replace('"@bjorn3/browser_wasi_shim"', '"/@bjorn3/browser_wasi_shim/dist/index.js"'));
  }
  if (url === "/libpandoc.wasm") file = wasm;
  else if (url.startsWith("/filters/") && wasmFilters) file = join(wasmFilters, normalize(url.slice(9)));
  else if (url.startsWith("/wasm/")) file = join(here, normalize(url.slice(6)));
  else if (url.startsWith("/@bjorn3/")) file = join(nm, normalize(url.slice(1)));
  if (!file || !existsSync(file)) { res.writeHead(404); return res.end(); }
  res.writeHead(200, { "content-type": types[extname(file)] ?? "application/octet-stream" });
  createReadStream(file).pipe(res);
}).listen(0);
const base = `http://localhost:${server.address().port}`;

let failed = false;
for (const name of process.argv.slice(2).length ? process.argv.slice(2) : ["chromium", "firefox", "webkit"]) {
  const browser = await playwright[name].launch();
  const p = await browser.newPage();
  p.on("console", (m) => { if (m.type() === "error") console.log(`  console: ${m.text()}`); });
  p.on("crash", () => console.log(`${name}: page crashed`));
  await p.goto(base);
  let result;
  try {
    await p.waitForFunction(() => window.result, null, { timeout: 180000 });
    result = await p.evaluate(() => window.result);
  } catch (e) {
    result = { error: e.message.split("\n")[0] };
  }
  console.log(`== ${name} ${browser.version()}`);
  if (result.error) { console.log(`  error: ${result.error}`); failed = true; }
  else {
    console.log(`  load ${result.loadMs.toFixed(0)} ms; 800 paragraphs ${result.convertMs.toFixed(0)} ms`);
    for (const r of result.results) {
      console.log(`  ${r.ok ? "ok  " : "FAIL"} ${r.name}${r.detail ? ": " + r.detail : ""}`);
      failed ||= !r.ok;
    }
  }
  await browser.close();
}
server.close();
process.exitCode = failed ? 1 : 0;
