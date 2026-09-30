// node --test wasm/test.mjs (after scripts/build-wasm.sh)
import assert from "node:assert/strict";
import { writeFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { load, PandocError } from "./node.mjs";

const path = process.env.LIBPANDOC_WASM ?? new URL("../dist/wasm/libpandoc.wasm", import.meta.url).pathname;
const t0 = performance.now();
const pandoc = await load(path);
console.log(`# loaded in ${(performance.now() - t0).toFixed(0)} ms`);

test("convert", () => {
  assert.equal(pandoc.convert({ from: "markdown", to: "html" }, "# Hi\n\n*a*"),
               '<h1 id="hi">Hi</h1>\n<p><em>a</em></p>\n');
});

test("errors have a kind", () => {
  assert.throws(() => pandoc.convert({ from: "nonsense" }, "x"),
                (e) => e instanceof PandocError && e.kind === "PandocUnknownReaderError");
});

test("query", () => {
  assert.equal(pandoc.query({ query: "version" }), "3.12");
  assert.ok(pandoc.query({ query: "input-formats" }).includes("docx"));
  assert.match(pandoc.query({ query: "default-template", format: "html" }), /<!DOCTYPE html>/);
});

test("read many", () => {
  const [a, b] = pandoc.readMany(["*a*", "b"], { from: "markdown" });
  assert.equal(a.blocks[0].c[0].t, "Emph");
  assert.equal(b.blocks[0].c[0].c, "b");
});

test("Lua filters", () => {
  // a filter file in /tmp, which the host shares with pandoc
  writeFileSync(join(pandoc.tmp, "upper.lua"), "function Str(e) return pandoc.Str(e.text:upper()) end");
  assert.equal(pandoc.convert({ from: "markdown", to: "plain", filters: ["/tmp/upper.lua"] }, "hi"), "HI\n");
});

test("a JS filter inside the conversion, told the formats", () => {
  let seen;
  const upper = (doc, ctx) => {
    seen = ctx;
    const walk = (x) => {
      if (Array.isArray(x)) return x.forEach(walk);
      if (x && typeof x === "object") {
        if (x.t === "Str") x.c = x.c.toUpperCase();
        Object.values(x).forEach(walk);
      }
    };
    walk(doc.blocks);
    return doc;
  };
  assert.equal(pandoc.convertWithFilters({ from: "markdown+smart", to: "plain" }, "hello *world*", [upper]),
               "HELLO WORLD\n");
  assert.equal(seen.format, "plain");
  assert.equal(seen["input-format"], "markdown+smart");
});

test("a filter may call pandoc again", () => {
  const f = (doc) => {
    const frag = JSON.parse(pandoc.convert({ from: "markdown", to: "json" }, "**inner**"));
    doc.blocks.push(...frag.blocks);
    return doc;
  };
  assert.equal(pandoc.convertWithFilters({ to: "html" }, "outer", [f]),
               "<p>outer</p>\n<p><strong>inner</strong></p>\n");
});

test("a failing filter fails the conversion", () => {
  assert.throws(() => pandoc.convertWithFilters({ to: "html" }, "x", [() => { throw new Error("boom"); }]),
                (e) => e.kind === "PandocFilterError" && /boom/.test(e.message));
});

test("command-line arguments, and binary output", () => {
  const [status, out] = pandoc.abi.convertArgs(["-f", "markdown", "-t", "html"], "*hi*");
  assert.equal(status, 0);
  assert.equal(new TextDecoder().decode(out), "<p><em>hi</em></p>\n");
  const docx = pandoc.convert({ to: "docx" }, "# Hi", { bytes: true });
  assert.equal(String.fromCharCode(docx[0], docx[1]), "PK"); // a zip
  assert.equal(pandoc.convert({ from: "docx", to: "plain" }, docx), "Hi\n");
});

test("timing", () => {
  const md = Array.from({ length: 800 }, (_, i) => `Paragraph ${i} with *emphasis* and [a link](u).`).join("\n\n");
  let t = performance.now();
  pandoc.convert({ from: "markdown", to: "html" }, md);
  const convert = performance.now() - t;
  t = performance.now();
  pandoc.convertWithFilters({ from: "markdown", to: "html" }, md, [(d) => d]);
  console.log(`# 800 paragraphs md->html: ${convert.toFixed(0)} ms; with an identity JS filter: ${(performance.now() - t).toFixed(0)} ms`);
});

test("untrusted: only options that read, write, fetch and run nothing, sandboxed", () => {
  assert.equal(pandoc.convert({ to: "html", untrusted: true }, "*hi*"), "<p><em>hi</em></p>\n");
  for (const extra of [{ citeproc: true }, { filters: ["x.lua"] }, { "output-file": "/tmp/o" },
                       { to: "pdf" }, { "data-dir": "/tmp" }]) {
    assert.throws(() => pandoc.convert({ to: "html", untrusted: true, ...extra }, "x"),
                  (e) => e instanceof PandocError && /not allowed for untrusted code/.test(e.message));
  }
  writeFileSync(join(pandoc.tmp, "untrusted-secret.tex"), "SECRET");
  const tex = "\\input{/tmp/untrusted-secret.tex}";
  assert.match(pandoc.convert({ from: "latex", to: "plain" }, tex), /SECRET/);
  assert.doesNotMatch(pandoc.convert({ from: "latex", to: "plain", untrusted: true }, tex), /SECRET/);
});
