// node --test wasm/test.mjs (after scripts/build-wasm.sh)
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync, readdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { load as loadInMemory } from "./browser.mjs";
import { load, PandocError, StoppedError } from "./node.mjs";

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
  assert.equal(pandoc.query({ query: "version" }), "3.14");
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

test("relative paths resolve against chdir's directory", () => {
  writeFileSync(join(pandoc.tmp, "rel.md"), "# rel");
  assert.throws(() => pandoc.convert({ to: "html", "input-files": ["rel.md"] }), PandocError);
  // (for good: "/" is no directory here, only /tmp is preopened)
  pandoc.chdir("/tmp");
  assert.equal(pandoc.convert({ to: "html", "input-files": ["rel.md"] }), '<h1 id="rel">rel</h1>\n');
  assert.throws(() => pandoc.chdir("/no/such/dir"), /can't change/);
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
  assert.equal(seen["writer-options"]["wrap-text"], "wrap-auto");
  assert.equal(seen["writer-options"].template, null);
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
  for (const extra of [{ filters: ["x.lua"] }, { "output-file": "/tmp/o" }, { to: "pdf" },
                       { "data-dir": "/tmp" }, { bibliography: ["/tmp/x.bib"] }, { csl: "/tmp/x.csl" }]) {
    assert.throws(() => pandoc.convert({ to: "html", untrusted: true, ...extra }, "x"),
                  (e) => e instanceof PandocError && /not allowed for untrusted code/.test(e.message));
  }
  writeFileSync(join(pandoc.tmp, "untrusted-secret.tex"), "SECRET");
  const tex = "\\input{/tmp/untrusted-secret.tex}";
  assert.match(pandoc.convert({ from: "latex", to: "plain" }, tex), /SECRET/);
  assert.doesNotMatch(pandoc.convert({ from: "latex", to: "plain", untrusted: true }, tex), /SECRET/);
  // citeproc, with the document's own references
  const refs = "---\nreferences: [{id: x, title: Inline Title, type: book}]\n---\n\n[@x]\n";
  assert.match(pandoc.convert({ to: "plain", citeproc: true, untrusted: true }, refs), /Inline Title/);
});

test("sandbox: citeproc reads only the bibliography the options name", () => {
  writeFileSync(join(pandoc.tmp, "sandbox-out.bib"), "@book{canary, title={CANARY}, year={2001}}\n");
  writeFileSync(join(pandoc.tmp, "sandbox-own.bib"), "@book{mine, title={Own Title}, year={2002}}\n");
  const doc = "---\nbibliography: /tmp/sandbox-out.bib\nnocite: \"@*\"\n---\n\nText.\n";
  const opts = { to: "plain", citeproc: true };
  assert.match(pandoc.convert(opts, doc), /CANARY/);
  assert.throws(() => pandoc.convert({ ...opts, sandbox: true }, doc),
                (e) => e.kind === "PandocResourceNotFound" && !/CANARY/.test(e.message));
  assert.throws(() => pandoc.convert({ ...opts, untrusted: true }, doc),
                (e) => e.kind === "PandocResourceNotFound" && !/CANARY/.test(e.message));
  assert.match(pandoc.convert({ ...opts, sandbox: true, bibliography: ["/tmp/sandbox-own.bib"] }, "[@mine]"),
               /Own Title/);
});

test("timeout: a call past it is a Timeout, an input of read_many in its place", () => {
  const slow = ">".repeat(20000); // nested block quotes: seconds
  const t0 = performance.now();
  assert.throws(() => pandoc.convert({ from: "markdown", to: "html", timeout: 0.3 }, slow),
                (e) => e.kind === "Timeout" && /time limit/.test(e.message));
  assert.ok(performance.now() - t0 < 5000);
  const [a, b] = pandoc.readMany(["*a*", slow], { from: "markdown", timeout: 0.3 });
  assert.equal(a.blocks[0].c[0].t, "Emph");
  assert.equal(b.error.kind, "Timeout");
  assert.equal(pandoc.convert({ to: "html" }, "*y*"), "<p><em>y</em></p>\n");
});

test("another instance from this one's compiled module; StoppedError exported", async () => {
  const again = await load(pandoc.module);
  assert.equal(again.convert({ to: "html" }, "*x*"), "<p><em>x</em></p>\n");
  assert.equal(pandoc.convert({ to: "html" }, "*y*"), "<p><em>y</em></p>\n");
  assert.ok(new StoppedError("x") instanceof Error);
  assert.equal(new StoppedError("x").name, "StoppedError");
});

// each kind of call once, failing ones too: what a call leaves in /tmp
function everyKind(p) {
  p.convert({ to: "html" }, "*x*");
  p.convert({ to: "html", untrusted: true }, "*x*");
  p.convert({ to: "docx" }, "x", { bytes: true });
  p.abi.convertArgs(["-t", "html"], "x");
  p.convertWithFilters({ to: "html" }, "x", [(d) => d]);
  assert.throws(() => p.convert({ from: "nonsense" }, "x"), PandocError);
  assert.throws(() => p.convertWithFilters({ to: "html" }, "x", [() => { throw new Error("boom"); }]),
                PandocError);
  assert.throws(() => p.convert({ to: "html", timeout: 0.2 }, ">".repeat(20000)),
                (e) => e.kind === "Timeout");
}

test("temporary files: a call leaves none, also failed and stopped ones", async () => {
  const calls = () => readdirSync(pandoc.tmp).filter((n) => n.startsWith("libpandoc"));
  everyKind(pandoc);
  for (let i = 0; i < 20; i++) pandoc.convert({ to: "html" }, "x".repeat(10000));
  assert.deepEqual(calls(), []);
  // browser.mjs's /tmp, in memory
  const inMemory = await loadInMemory(pandoc.module);
  everyKind(inMemory);
  assert.deepEqual([...inMemory.tmp.keys()], []);
  inMemory.close();
  assert.throws(() => inMemory.convert({ to: "html" }, "x"), StoppedError);
});

test("close: the instance's directory is removed, and later calls are StoppedErrors", async () => {
  const own = await load(pandoc.module);
  writeFileSync(join(own.tmp, "mine.txt"), "x");
  assert.equal(own.convert({ to: "html" }, "*x*"), "<p><em>x</em></p>\n");
  own.close();
  assert.equal(existsSync(own.tmp), false);
  assert.throws(() => own.convert({ to: "html" }, "x"), StoppedError);
  own.close(); // twice: nothing
  {
    using scoped = await load(pandoc.module);
    var dir = scoped.tmp;
    assert.equal(existsSync(dir), true);
  }
  assert.equal(existsSync(dir), false);
  // not closed: removed at the process's exit
  const child = spawnSync(process.execPath, ["--no-warnings", "--input-type=module", "-e", `
    import { load } from ${JSON.stringify(new URL("node.mjs", import.meta.url).href)};
    const p = await load(${JSON.stringify(path)});
    p.convert({ to: "html" }, "x");
    console.log(p.tmp);`], { encoding: "utf8" });
  assert.equal(child.status, 0, child.stderr);
  assert.equal(existsSync(child.stdout.trim()), false);
});
