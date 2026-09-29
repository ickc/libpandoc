// node --test wasm/test-wasm-filter.mjs: wasm filters (WASI commands, e.g.
// Rust with panir) inside a libpandoc.wasm conversion. WASM_FILTERS is a
// directory with libpandoc-rs's example filters built for wasm32-wasip1
// (upper.wasm, conversion.wasm, include.wasm).
import assert from "node:assert/strict";
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { File, PreopenDirectory } from "@bjorn3/browser_wasi_shim";
import { load, PandocError } from "./node.mjs";
import { wasmFilter } from "./wasm-filter.mjs";

const path = process.env.LIBPANDOC_WASM ?? new URL("../dist/wasm/libpandoc.wasm", import.meta.url).pathname;
const dir = process.env.WASM_FILTERS;
if (!dir) throw new Error("WASM_FILTERS: a directory of the example wasm filters");
const pandoc = await load(path);
const version = pandoc.query({ query: "version" });
const filter = (name, opts = {}) =>
  wasmFilter(readFileSync(join(dir, `${name}.wasm`)), { name: `${name}.wasm`, pandocVersion: version, ...opts });

test("a wasm filter inside the conversion", async () => {
  const upper = await filter("upper");
  assert.equal(pandoc.convertWithFilters({ from: "markdown", to: "plain" }, "hello *world*", [upper]),
               "HELLO WORLD\n");
});

test("between JS and Lua filters, in order", async () => {
  const upper = await filter("upper");
  const addPara = (doc) => { doc.blocks.push({ t: "Para", c: [{ t: "Str", c: "js" }] }); return doc; };
  const out = pandoc.convertWithFilters(
    { from: "markdown", to: "plain", filters: [{ type: "callback", index: 0 }, { type: "callback", index: 1 }] },
    "a", [addPara, upper]);
  assert.equal(out, "A\n\nJS\n");
});

test("told the conversion, as pandoc tells a JSON filter", async () => {
  const conv = await filter("conversion");
  const out = pandoc.convertWithFilters({ from: "commonmark_x", to: "html5" }, "x", [conv]);
  const told = JSON.parse(out.split("<code>")[1].split("</code>")[0].replaceAll("&quot;", '"'));
  assert.equal(told.format, "html5");
  assert.match(told["input-format"], /^commonmark_x/);
  assert.match(told["output-format"], /^html5/);
  assert.equal(told["reader-options"], true);
  assert.equal(told["pandoc-version"], version);
});

test("sees only the directories given", async () => {
  const input = "```include\npart.txt\n```\n";
  const files = new PreopenDirectory(".", new Map([["part.txt", new File(new TextEncoder().encode("included"))]]));
  const include = await filter("include", { preopens: [files] });
  assert.equal(pandoc.convertWithFilters({ from: "markdown", to: "plain" }, input, [include]).trim(), "included");
  const blind = await filter("include", { stderr: () => {} });
  assert.throws(() => pandoc.convertWithFilters({ from: "markdown", to: "plain" }, input, [blind]),
                (e) => e instanceof PandocError && e.kind === "PandocFilterError" && /exited with status 3/.test(e.message));
});

test("compiled once, run many times", async () => {
  const upper = await filter("upper");
  for (let i = 0; i < 20; i++) {
    assert.equal(pandoc.convertWithFilters({ from: "markdown", to: "plain" }, `n${i}`, [upper]), `N${i}\n`);
  }
});


test("a filter calling pandoc: read_many, as the document is read", async () => {
  const parse = await filter("parse", { pandoc });
  const input = "```parse\n*a*\n```\n\n```parse\n# b\n```\n";
  assert.equal(pandoc.convertWithFilters({ from: "markdown", to: "html" }, input, [parse]),
               '<p><em>a</em></p>\n<h1 id="b">b</h1>\n');
});

// the `calls` filter answers each request in a code block with class call
const calls = async (requests) => {
  const f = await filter("calls", { pandoc });
  const input = requests.map((r) => "```call\n" + JSON.stringify(r) + "\n```\n").join("\n");
  const doc = JSON.parse(pandoc.convertWithFilters({ from: "markdown", to: "json" }, input, [f]));
  return doc.blocks.map((b) => JSON.parse(b.c[1]));
};

test("a filter calling pandoc: convert, read_many, query", async () => {
  const [conv, read, query] = await calls([
    { convert: [{ from: "markdown", to: "html" }, "*x*"] },
    { read_many: [["a", "*b*"], { from: "markdown" }] },
    { query: ["version", null] },
  ]);
  assert.deepEqual(conv, { ok: "<p><em>x</em></p>\n" });
  assert.equal(read.ok.length, 2);
  assert.deepEqual(read.ok[1].blocks[0].c[0], { t: "Emph", c: [{ t: "Str", c: "b" }] });
  assert.deepEqual(query, { ok: version });
});

test("a filter's calls get no files, programs or Lua", async () => {
  const answers = await calls([
    { convert: [{ from: "markdown", to: "html", filters: ["/bin/sh"] }, "x"] },
    { convert: [{ from: "markdown", to: "html", "output-file": "out.html" }, "x"] },
    { convert: [{ from: "markdown", to: "writer.lua" }, "x"] },
    { convert: [{ from: "markdown", to: "pdf" }, "x"] },
    { convert: [{ from: "markdown", to: "html" }, null] },
    { read_many: [["x"], { from: "markdown", "data-dir": "/" }] },
    { query: ["parse-args", { args: ["-d", "x.yaml"] }] },
  ]);
  for (const a of answers) {
    assert.equal(a.error?.[0], "PandocOptionError", JSON.stringify(a));
  }
  assert.match(answers[0].error[1], /not allowed in a wasm filter: filters/);
});

test("a filter calling pandoc needs the pandoc", async () => {
  await assert.rejects(filter("parse"), /calls pandoc/);
});

test("a filter's calls are sandboxed (LaTeX's \\input reads no file)", async () => {
  // a file pandoc itself can read: in its /tmp
  writeFileSync(join(pandoc.tmp, "secret.tex"), "SECRET");
  const tex = "\\input{/tmp/secret.tex}";
  assert.match(JSON.stringify(pandoc.readMany([tex], { from: "latex" })), /SECRET/);
  const answers = await calls([
    { read_many: [[tex], { from: "latex" }] },
    { convert: [{ from: "latex", to: "plain" }, tex] },
  ]);
  for (const a of answers) assert.doesNotMatch(JSON.stringify(a), /SECRET/);
});
