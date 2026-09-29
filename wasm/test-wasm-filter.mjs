// node --test wasm/test-wasm-filter.mjs: wasm filters (WASI commands, e.g.
// Rust with panir) inside a libpandoc.wasm conversion. WASM_FILTERS is a
// directory with libpandoc-rs's example filters built for wasm32-wasip1
// (upper.wasm, conversion.wasm, include.wasm).
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
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

