// node --test wasm/test.mjs (after scripts/build-wasm.sh)
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, symlinkSync,
         writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { load as loadInMemory } from "./browser.mjs";
import { load, PandocError, StoppedError } from "./node.mjs";
import { readOptions } from "./core.mjs";

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

test("a filter is told whether pandoc's sandbox is on (1.13)", () => {
  const told = [];
  const f = (doc, ctx) => { told.push(ctx.sandbox); return doc; };
  pandoc.convertWithFilters({ to: "html", sandbox: true }, "x", [f]);
  pandoc.convertWithFilters({ to: "html" }, "x", [f]);
  assert.deepEqual(told, [true, false]);
  assert.equal(pandoc.query({ query: "parse-args", args: ["--sandbox"] }).sandbox, true);
});

test("a filter gets the instance it runs in: read and readMany as the conversion reads", () => {
  writeFileSync(join(pandoc.tmp, "h2-secret.tex"), "SECRET");
  const tex = "\\emph{x} \\input{/tmp/h2-secret.tex}";
  const seen = {};
  const f = (doc, ctx, p) => {
    seen.same = p.read === pandoc.read;
    const frag = p.read(tex, ctx); // LaTeX, as the conversion reads, sandboxed if it is
    seen[ctx.sandbox ? "sandboxed" : "free"] = JSON.stringify(frag);
    seen.many = p.readMany(["*a*", "\\textbf{b}"], ctx).map((d) => d.blocks[0].c[0].t);
    seen.override = JSON.stringify(p.read(tex, ctx, { sandbox: false }));
    doc.blocks.push(...frag.blocks);
    return doc;
  };
  const out = pandoc.convertWithFilters({ from: "latex", to: "plain" }, "y", [f]);
  assert.ok(seen.same);
  assert.match(out, /^y\n\nx SECRET/);
  assert.match(seen.free, /"Emph"/);
  assert.match(seen.free, /SECRET/);
  pandoc.convertWithFilters({ from: "latex", to: "plain", sandbox: true }, "y", [f]);
  assert.match(seen.sandboxed, /"Emph"/);
  assert.doesNotMatch(seen.sandboxed, /SECRET/);
  assert.deepEqual(seen.many, ["Str", "Strong"]); // "*a*" is no emphasis in LaTeX
  assert.match(seen.override, /SECRET/); // the filter's own choice
  // a raw filter (JSON text) gets the instance too
  pandoc.convertWithFilters({ to: "html" }, "x", [(d, c, p) => { seen.raw = p.read("*r*", JSON.parse(c)); return d; }],
                            { raw: true });
  assert.equal(seen.raw.blocks[0].c[0].t, "Emph");
});

test("read, readMany: a format, options, a conversion; failing fails", () => {
  assert.equal(pandoc.read("\\textbf{b}", "latex").blocks[0].c[0].t, "Strong");
  assert.equal(pandoc.readMany(["\\textbf{b}"], "latex")[0].blocks[0].c[0].t, "Strong");
  assert.equal(pandoc.readMany(["# x"], { from: "markdown" })[0].blocks[0].t, "Header");
  assert.throws(() => pandoc.read("x", "nonsense"), (e) => e instanceof PandocError);
  assert.throws(() => pandoc.read("{", "json"), (e) => e instanceof PandocError);
  // a conversion that doesn't say whether it is sandboxed (before 1.13): read
  // in pandoc's sandbox (fail closed); panir's Conversion, camelCase
  assert.deepEqual(readOptions({ "input-format": "latex", "reader-options": { "tab-stop": 2 } }),
                   { "tab-stop": 2, sandbox: true, "preserve-tabs": true, from: "latex" });
  assert.equal(readOptions({ inputFormat: "rst", sandbox: false }).sandbox, undefined);
  assert.equal(readOptions({ inputFormat: "rst", options: { sandbox: { read: ["."] } } }).sandbox, true);
});

test("filters calling the instance, nested deep", () => {
  // each level converts with a filter of the next, which reads a fragment
  const level = (n) => (doc, ctx, p) => {
    const frag = p.read(`*${n}*`, ctx);
    doc.blocks.push(...frag.blocks);
    if (n < 20) {
      const inner = JSON.parse(p.convertWithFilters({ from: "markdown", to: "json" }, `level ${n + 1}`, [level(n + 1)]));
      doc.blocks.push(...inner.blocks);
    }
    return doc;
  };
  const out = pandoc.convertWithFilters({ from: "markdown", to: "plain" }, "top", [level(1)]);
  assert.equal(out.split("\n\n").length, 40, out); // top, 1, then level n and n for 2 to 20
  assert.match(out, /level 20\n\n20\n$/);
  assert.equal(pandoc.convert({ to: "plain" }, "*after*"), "after\n");
});

test("binary output is bytes by itself (H12); asked for text, an error that says so", () => {
  const docx = pandoc.convert({ to: "docx" }, "# Hi");
  assert.ok(docx instanceof Uint8Array);
  assert.equal(String.fromCharCode(docx[0], docx[1]), "PK");
  assert.ok(pandoc.convertWithFilters({ to: "odt" }, "x", [(d) => d]) instanceof Uint8Array);
  assert.equal(typeof pandoc.convert({ to: "html" }, "x"), "string");
  assert.ok(pandoc.convert({ to: "html" }, "x", { bytes: true }) instanceof Uint8Array);
  assert.throws(() => pandoc.convert({ to: "docx" }, "x", { bytes: false }),
                (e) => e instanceof TypeError && /pass \{ bytes: true \}/.test(e.message));
});

test("a failing filter fails the conversion", () => {
  assert.throws(() => pandoc.convertWithFilters({ to: "html" }, "x", [() => { throw new Error("boom"); }]),
                (e) => e.kind === "PandocFilterError" && /boom/.test(e.message));
});

// Consecutive JS functions are one exchange with pandoc: what one returns is
// what the next is given. A Lua filter between two makes two.
const noop = "/tmp/noop.lua";
writeFileSync(join(pandoc.tmp, "noop.lua"), "return {}");
const para = (text) => ({ t: "Para", c: [{ t: "Str", c: text }] });
// the options running fns[0], fns[1], ... with Lua's no-op between each
const apart = (options, fns) => ({ ...options, filters: fns.flatMap((_, i) => i ? [noop, cb(i)] : [cb(i)]) });
const cb = (index) => ({ type: "callback", index });

test("consecutive JS filters give what they give apart", () => {
  const fns = [
    (doc) => { doc.blocks.push(para("two")); return doc; },
    (doc) => ({ ...doc, blocks: [...doc.blocks, para("three")] }),
    (doc, ctx) => { doc.blocks.push(para(ctx.format)); return doc; },
  ];
  const options = { from: "markdown", to: "plain" };
  const together = pandoc.convertWithFilters(options, "one", fns);
  assert.equal(together, "one\n\ntwo\n\nthree\n\nplain\n");
  assert.equal(pandoc.convertWithFilters(apart(options, fns), "one", fns), together);
});

test("a run of JS filters is one exchange: the next is given what the one before returned", () => {
  const given = [];
  const fns = [0, 1, 2].map(() => (doc) => { given.push(doc); return doc; });
  pandoc.convertWithFilters({ to: "plain" }, "x", fns);
  assert.ok(given[0] === given[1] && given[1] === given[2]);
  given.length = 0;
  pandoc.convertWithFilters(apart({ to: "plain" }, fns), "x", fns);
  assert.ok(given[0] !== given[1] && given[1] !== given[2]);
  // the filters option may name a function twice, in any order
  given.length = 0;
  const order = [];
  const named = [0, 1].map((i) => (doc) => { order.push(i); given.push(doc); return doc; });
  pandoc.convertWithFilters({ to: "plain", filters: [cb(1), cb(0), cb(1)] }, "x", named);
  assert.deepEqual(order, [1, 0, 1]);
  assert.ok(given[0] === given[1] && given[1] === given[2]);
  // each is given a context of its own
  const formats = [];
  const ctxs = [0, 1].map(() => (doc, ctx) => { formats.push(ctx.format); ctx.format = "changed"; return doc; });
  pandoc.convertWithFilters({ to: "plain" }, "x", ctxs);
  assert.deepEqual(formats, ["plain", "plain"]);
});

test("a run ends at a Lua filter: typed, Lua, typed, typed is two callbacks, in order", () => {
  const given = [];
  const fns = ["b", "c", "d"].map((s) => (doc) => { given.push(doc); doc.blocks.push(para(s)); return doc; });
  writeFileSync(join(pandoc.tmp, "upper.lua"), "function Str(e) return pandoc.Str(e.text:upper()) end");
  const out = pandoc.convertWithFilters({ to: "plain", filters: [cb(0), "/tmp/upper.lua", cb(1), cb(2)] }, "a", fns);
  assert.equal(out, "A\n\nB\n\nc\n\nd\n");
  assert.ok(given[0] !== given[1] && given[1] === given[2]);
});

// raw functions and functions marked `bytes` (a wasm filter's) run in a
// run too: what one writes is the next one's input, unchecked
const bytesFn = (f) => Object.assign((doc, ctx) => f(new TextDecoder().decode(doc), ctx), { bytes: true });
const sortedMeta = (text) => JSON.stringify({ ...JSON.parse(text), meta: { b: { t: "MetaBool", c: true }, a: { t: "MetaBool", c: true } } });

test("a mixed run (plain, bytes, plain, bytes) keeps the order and gives what it gives apart", () => {
  const order = [];
  const fns = [
    (doc) => { order.push(0); doc.blocks.push(para("p0")); return doc; },
    bytesFn((text) => { order.push(1); return text.replace('"p0"', '"b1"'); }),
    (doc) => { order.push(2); doc.blocks.push(para("p2")); return doc; },
    bytesFn((text) => { order.push(3); return text.replace('"p2"', '"b3"'); }),
  ];
  const together = pandoc.convertWithFilters({ to: "plain" }, "x", fns);
  assert.equal(together, "x\n\nb1\n\nb3\n");
  assert.equal(pandoc.convertWithFilters({ to: "plain" }, "x", fns, { separateFilters: true }), together);
  assert.deepEqual(order, [0, 1, 2, 3, 0, 1, 2, 3]);
});

test("in a run, what a raw or bytes function wrote is the next one's input, unchecked", () => {
  // as written: the metadata's keys in the filter's order (pandoc writes
  // them sorted)
  let keys;
  const raw = [sortedMeta, (text) => { keys = Object.keys(JSON.parse(text).meta); return text; }];
  pandoc.convertWithFilters({ to: "plain" }, "x", raw, { raw: true });
  assert.deepEqual(keys, ["b", "a"]);
  pandoc.convertWithFilters({ to: "plain" }, "x", raw, { raw: true, separateFilters: true });
  assert.deepEqual(keys, ["a", "b"]);
  // a document pandoc would refuse, repaired by the next: chained it goes
  // through; apart pandoc refuses it after the first
  const repaired = [
    (text) => text.replace(/"blocks":\[.*\]/, '"blocks":[{"t":"Nonsense"}]'),
    (text) => text.replace('{"t":"Nonsense"}', '{"t":"Para","c":[{"t":"Str","c":"fine"}]}'),
  ];
  assert.equal(pandoc.convertWithFilters({ to: "plain" }, "x", repaired, { raw: true }), "fine\n");
  assert.throws(() => pandoc.convertWithFilters({ to: "plain" }, "x", repaired, { raw: true, separateFilters: true }),
                (e) => e.kind === "PandocFilterError" && /Nonsense/.test(e.message));
  // not JSON, then a function that parses it: the error names both
  const notJson = [bytesFn(() => "not JSON"), (doc) => doc];
  assert.throws(() => pandoc.convertWithFilters({ to: "plain" }, "x", notJson),
                (e) => e.kind === "PandocFilterError" && /fns\[1\]: .*JSON/.test(e.message)
                  && /\(its input is what fns\[0\] wrote, unchecked: LIBPANDOC_SEPARATE_FILTERS=1 has pandoc check it\)/.test(e.message));
  // apart, pandoc refuses it after the first, naming no other
  assert.throws(() => pandoc.convertWithFilters({ to: "plain" }, "x", notJson, { separateFilters: true }),
                (e) => e.kind === "PandocFilterError" && !/its input is what/.test(e.message));
  // a raw or bytes function failing on such input: the same hint
  const fails = [() => "not JSON", (text) => JSON.parse(text)];
  assert.throws(() => pandoc.convertWithFilters({ to: "plain" }, "x", fails, { raw: true }),
                (e) => /fns\[1\]: .*\(its input is what fns\[0\] wrote, unchecked/.test(e.message));
  // the final document is pandoc's to check, as always
  assert.throws(() => pandoc.convertWithFilters({ to: "plain" }, "x", [() => "not JSON", (t) => t], { raw: true }),
                (e) => e.kind === "PandocFilterError");
});

test("separateFilters, and LIBPANDOC_SEPARATE_FILTERS: each filter its own exchange", () => {
  const given = [];
  const fns = [0, 1].map(() => (doc) => { given.push(doc); return doc; });
  const same = (opts) => {
    given.length = 0;
    pandoc.convertWithFilters({ to: "plain" }, "x", fns, opts);
    return given[0] === given[1];
  };
  const env = process.env.LIBPANDOC_SEPARATE_FILTERS;
  try {
    for (const v of [undefined, "", "0"]) {
      if (v === undefined) delete process.env.LIBPANDOC_SEPARATE_FILTERS;
      else process.env.LIBPANDOC_SEPARATE_FILTERS = v;
      assert.equal(same(), true, `LIBPANDOC_SEPARATE_FILTERS=${v}`);
      assert.equal(same({ separateFilters: true }), false);
    }
    process.env.LIBPANDOC_SEPARATE_FILTERS = "1";
    assert.equal(same(), false);
    assert.equal(same({ separateFilters: false }), true); // the API's value wins
    for (const v of ["yes", "true", " 1"]) {
      process.env.LIBPANDOC_SEPARATE_FILTERS = v;
      assert.throws(() => same(), (e) => e instanceof TypeError && e.message.includes("LIBPANDOC_SEPARATE_FILTERS")
                    && e.message.includes(JSON.stringify(v)));
      assert.equal(same({ separateFilters: false }), true);
    }
  } finally {
    if (env === undefined) delete process.env.LIBPANDOC_SEPARATE_FILTERS;
    else process.env.LIBPANDOC_SEPARATE_FILTERS = env;
  }
  assert.throws(() => same({ separateFilters: "yes" }), (e) => e instanceof TypeError && /separateFilters/.test(e.message));
  // an index naming no function is refused before the call
  assert.throws(() => pandoc.convertWithFilters({ to: "plain", filters: [cb(2)] }, "x", fns),
                (e) => e instanceof TypeError && /index" below 2/.test(e.message));
});

test("in a run, the next filter sees what the one before left, not what pandoc would make of it", () => {
  // the metadata's keys in the order the filter put them (pandoc writes
  // them sorted)
  const keys = [];
  const fns = [
    (doc) => { doc.meta = { b: { t: "MetaBool", c: true }, a: { t: "MetaBool", c: true } }; return doc; },
    (doc) => { keys.push(Object.keys(doc.meta)); return doc; },
  ];
  pandoc.convertWithFilters({ to: "plain" }, "x", fns);
  pandoc.convertWithFilters(apart({ to: "plain" }, fns), "x", fns);
  assert.deepEqual(keys, [["b", "a"], ["a", "b"]]);
  // a node put in twice is one object: changed in place, it changes in both
  // places (apart, pandoc wrote two copies)
  const shared = [
    (doc) => { const p = para("x"); doc.blocks = [p, p]; return doc; },
    (doc) => { doc.blocks[0].c[0].c = "y"; return doc; },
  ];
  assert.equal(pandoc.convertWithFilters({ to: "plain" }, "", shared), "y\n\ny\n");
  assert.equal(pandoc.convertWithFilters(apart({ to: "plain" }, shared), "", shared), "y\n\nx\n");
  // a document pandoc would refuse is refused at the end of the run, not
  // after the filter that made it: a later filter can repair it
  const repaired = [
    (doc) => { doc.blocks = [{ t: "Nonsense" }]; return doc; },
    (doc) => { doc.blocks = [para("fine")]; return doc; },
  ];
  assert.equal(pandoc.convertWithFilters({ to: "plain" }, "x", repaired), "fine\n");
  assert.throws(() => pandoc.convertWithFilters(apart({ to: "plain" }, repaired), "x", repaired),
                (e) => e.kind === "PandocFilterError" && /Nonsense/.test(e.message));
});

test("the filter of a run that failed is named, and those after it don't run", () => {
  let after = false;
  const fns = [(doc) => doc, () => { throw new Error("boom"); }, (doc) => { after = true; return doc; }];
  assert.throws(() => pandoc.convertWithFilters({ to: "html" }, "x", fns),
                (e) => e.kind === "PandocFilterError" && /fns\[1\]: boom/.test(e.message));
  assert.equal(after, false);
  for (const [fn, message] of [[() => undefined, /fns\[1\]: the filter returned no document/],
                               [async (d) => d, /fns\[1\]: the filter returned a Promise/]]) {
    assert.throws(() => pandoc.convertWithFilters({ to: "html" }, "x", [(doc) => doc, fn]),
                  (e) => e.kind === "PandocFilterError" && message.test(e.message));
  }
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

test("a filter's answer that isn't bytes or a string is an error of that conversion", () => {
  const raw = (answer) => Object.assign(() => answer, { bytes: true });
  const doc = (d) => d;
  for (const [fn, message] of [
    [raw(undefined), /bytes or a string, not undefined/],
    [raw(42), /bytes or a string, not number/],
    [raw({ blocks: [] }), /bytes or a string, not object/],
    [raw(Promise.resolve("{}")), /Promise: filters run synchronously/],
    [() => undefined, /no document \(a missing return\?\)/],
    [async (d) => d, /Promise: filters run synchronously/],
    [() => { throw Symbol("odd"); }, /Symbol\(odd\)/],
  ]) {
    assert.throws(() => pandoc.convertWithFilters({ to: "html" }, "x", [fn]),
                  (e) => e instanceof PandocError && e.kind === "PandocFilterError" && message.test(e.message));
  }
  // the instance goes on, and its files are gone
  assert.equal(pandoc.convertWithFilters({ to: "html" }, "*x*", [doc]), "<p><em>x</em></p>\n");
  assert.deepEqual(readdirSync(pandoc.tmp).filter((n) => n.startsWith("libpandoc")), []);
});

test("arguments of the wrong type are refused before the call; the instance goes on", () => {
  for (const f of [() => pandoc.abi.convert("{}", 42), () => pandoc.abi.convert({}, "x"),
                   () => pandoc.abi.convertArgs("-t html", "x"), () => pandoc.abi.convertArgs(["-t", 1], "x"),
                   () => pandoc.abi.convertArgs(["-t", "ht\0ml"], "x"), () => pandoc.chdir("/tmp\0/x"),
                   () => pandoc.abi.convertFilters("{}", "x", "not an array")]) {
    assert.throws(f, (e) => e instanceof TypeError && !(e instanceof StoppedError));
  }
  // bytes in any view
  assert.equal(pandoc.convert({ to: "html" }, new TextEncoder().encode("*a*").buffer), "<p><em>a</em></p>\n");
  assert.equal(pandoc.convert({ to: "html" }, new DataView(new TextEncoder().encode("*b*").buffer)),
               "<p><em>b</em></p>\n");
});

test("an exception thrown through a call stops the instance: a StoppedError, now and later", async () => {
  // a Lua filter's os.exit: WASI's proc_exit, as when the runtime runs out of memory
  const exits = await load(pandoc.module);
  writeFileSync(join(exits.tmp, "exit.lua"), "os.exit(3)");
  assert.throws(() => exits.convert({ to: "html", filters: ["/tmp/exit.lua"] }, "x"), StoppedError);
  assert.throws(() => exits.convert({ to: "html" }, "x"), (e) => e instanceof StoppedError && /exited/.test(e.message));
  // so too from a call a filter makes: the outer call stops, it doesn't go on
  const nested = await load(pandoc.module);
  writeFileSync(join(nested.tmp, "exit.lua"), "os.exit(3)");
  const calls = (d) => { nested.convert({ to: "html", filters: ["/tmp/exit.lua"] }, "inner"); return d; };
  assert.throws(() => nested.convertWithFilters({ to: "html" }, "outer", [calls]), StoppedError);
  assert.throws(() => nested.query({ query: "version" }), StoppedError);
  exits.close();
  nested.close();
});

test("a document reaches only the directories given: not through .., nor by an absolute path", async () => {
  const outside = mkdtempSync(join(tmpdir(), "libpandoc-wasm-test-"));
  try {
    writeFileSync(join(outside, "canary.tex"), "CANARY");
    mkdirSync(join(outside, "work"));
    writeFileSync(join(outside, "work", "in.tex"), "INSIDE");
    symlinkSync(join(outside, "canary.tex"), join(outside, "work", "link.tex"));
    const mem = await loadInMemory(pandoc.module, { files: { "/tmp/in.tex": "INSIDE" } });
    const node = await load(pandoc.module, { preopens: { "/work": join(outside, "work") } });
    const read = (p, path) => {
      try {
        return p.convert({ from: "latex", to: "plain", "input-files": [path] });
      } catch (e) {
        return e.message;
      }
    };
    assert.match(read(node, "/work/in.tex"), /INSIDE/);
    assert.match(read(mem, "/tmp/in.tex"), /INSIDE/);
    for (const p of [node, mem]) {
      for (const path of ["/work/../canary.tex", "/tmp/../" + outside.slice(1) + "/canary.tex",
                          join(outside, "canary.tex"), "/work/link.tex"]) {
        assert.doesNotMatch(read(p, path), /CANARY/, path);
      }
      // nor through LaTeX's \input
      assert.doesNotMatch(p.convert({ from: "latex", to: "plain" }, "\\input{/work/../canary.tex}"), /CANARY/);
    }
    node.close();
    mem.close();
  } finally {
    rmSync(outside, { recursive: true, force: true });
  }
});

test("panclude: inside its roots only, also past a link to a directory outside (1.15)", async () => {
  const outside = mkdtempSync(join(tmpdir(), "libpandoc-wasm-test-"));
  try {
    writeFileSync(join(outside, "canary.txt"), "CANARY");
    mkdirSync(join(outside, "work"));
    mkdirSync(join(outside, "work", "shared"));
    writeFileSync(join(outside, "work", "in.txt"), "INSIDE");
    writeFileSync(join(outside, "work", "shared", "s.txt"), "SHARED");
    // (node:wasi follows a link to a directory wherever it points: README)
    symlinkSync(outside, join(outside, "work", "up"));
    const p = await load(pandoc.module, { preopens: { "/work": join(outside, "work") } });
    p.chdir("/work");
    const doc = ["in.txt", "shared/s.txt", "up/canary.txt", "../canary.txt"]
      .map((f) => `\`\`\`{include="${f}"}\n\`\`\`\n`).join("\n");
    const run = (options) => {
      const [status, out, , message, log] = p.abi.convert(JSON.stringify({ to: "plain", ...options }), doc);
      assert.equal(status, 0, message);
      return { out: new TextDecoder().decode(out), warnings: JSON.parse(log).map((m) => m.message) };
    };
    const reason = (f, why) => `panclude: cannot include "${f}" in standard input: ${why}`;
    let r = run({ filters: ["panclude"] });
    assert.match(r.out, /INSIDE/);
    assert.match(r.out, /SHARED/);
    assert.doesNotMatch(r.out, /CANARY/);
    assert.deepEqual(r.warnings, [reason("up/canary.txt", "a symbolic link leads outside the roots"),
                                  reason("../canary.txt", "outside the roots")]);
    r = run({ filters: [{ type: "panclude", roots: ["shared"] }] });
    assert.doesNotMatch(r.out, /INSIDE|CANARY/);
    assert.match(r.out, /SHARED/);
    r = run({ filters: ["panclude"], untrusted: true });
    assert.doesNotMatch(r.out, /INSIDE|SHARED|CANARY/);
    assert.equal(r.warnings.length, 4);
    assert.ok(r.warnings.every((w) => w.endsWith("there are no roots, so no file is included")), r.warnings);
    r = run({ filters: ["panclude"], untrusted: { "panclude-roots": ["/work/shared"] } });
    assert.match(r.out, /SHARED/);
    assert.doesNotMatch(r.out, /INSIDE|CANARY/);
    p.close();
  } finally {
    rmSync(outside, { recursive: true, force: true });
  }
});

test("panclude in browser.mjs's in-memory /tmp: its roots as the module names them (1.15)", async () => {
  const p = await loadInMemory(pandoc.module, { files: { "/tmp/part.md": "hello *from the part*\n" } });
  const doc = '```{include="/tmp/part.md"}\nPLACEHOLDER\n```\n';
  const plain = (filters) => p.convert({ to: "plain", filters }, doc);
  assert.match(plain([{ type: "panclude", roots: ["/tmp"] }]), /hello \*from the part\*/);
  // the default for input given: the working directory, / until chdir, so
  // every directory the host opened
  assert.match(plain(["panclude"]), /hello \*from the part\*/);
  // a root the module has no directory for: an error
  assert.throws(() => plain([{ type: "panclude", roots: ["/elsewhere"] }]),
                (e) => e instanceof PandocError && e.kind === "PandocOptionError");
  assert.match(p.convert({ to: "plain", filters: ["panclude"], untrusted: true }, doc), /PLACEHOLDER/);
  assert.match(p.convert({ to: "plain", filters: ["panclude"], untrusted: { "panclude-roots": ["/tmp"] } }, doc),
               /hello \*from the part\*/);
  p.close();
});
