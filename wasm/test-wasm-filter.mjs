// node --test wasm/test-wasm-filter.mjs: wasm filters (WASI commands, e.g.
// Rust with panir) inside a libpandoc.wasm conversion. WASM_FILTERS is a
// directory with libpandoc-rs's example filters built for wasm32-wasip1
// (upper.wasm, conversion.wasm, include.wasm).
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { delimiter, join } from "node:path";
import { test } from "node:test";
import { File, PreopenDirectory } from "@bjorn3/browser_wasi_shim";
import { load, PandocError } from "./node.mjs";
import { hostPreopens } from "./node-dir.mjs";
import { parseMemory, wasmFilter } from "./wasm-filter.mjs";

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
  const blind = await filter("include", { preopens: [], stderr: () => {} });
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
  assert.match(answers[0].error[1], /not allowed for untrusted code: filters/);
});

test("a filter calling pandoc: by default, the pandoc it runs in", async () => {
  const parse = await filter("parse");
  assert.equal(pandoc.convertWithFilters({ from: "markdown", to: "html" }, "```parse\n*a*\n```\n", [parse]),
               "<p><em>a</em></p>\n");
  // run outside a conversion: it has none
  const ctx = new TextEncoder().encode(JSON.stringify({ format: "html" }));
  assert.throws(() => parse(new TextEncoder().encode("{}"), ctx), /calls pandoc/);
});

// A tree for the directory tests, the working directory its work/:
//   top/{outside.txt, common/part.txt, work/{part.txt, sub/deep.txt, links}}
// and the variable LIBPANDOC_WASM_DIRS as given (undefined: unset), both
// restored after.
async function inTree(dirs, body) {
  const top = mkdtempSync(join(tmpdir(), "wasm-filter-dirs-"));
  const work = join(top, "work");
  mkdirSync(join(work, "sub"), { recursive: true });
  mkdirSync(join(top, "common"));
  writeFileSync(join(work, "part.txt"), "included");
  writeFileSync(join(work, "sub", "deep.txt"), "deeper");
  writeFileSync(join(top, "common", "part.txt"), "common");
  writeFileSync(join(top, "outside.txt"), "OUTSIDE");
  symlinkSync(join(top, "outside.txt"), join(work, "link.txt"));
  symlinkSync(top, join(work, "linkdir"));
  symlinkSync(join(work, "part.txt"), join(work, "inner-link.txt"));
  const before = process.cwd(), saved = process.env.LIBPANDOC_WASM_DIRS;
  if (dirs === undefined) delete process.env.LIBPANDOC_WASM_DIRS;
  else process.env.LIBPANDOC_WASM_DIRS = typeof dirs === "function" ? dirs(top) : dirs;
  process.chdir(work);
  try {
    // (the variable is read when the filter is made)
    const include = await filter("include", { stderr: () => {} });
    const run = (name) => pandoc.convertWithFilters({ from: "markdown", to: "plain" },
                                                    "```include\n" + name + "\n```\n", [include]).trim();
    await body({ top, work, run });
  } finally {
    process.chdir(before);
    if (saved === undefined) delete process.env.LIBPANDOC_WASM_DIRS;
    else process.env.LIBPANDOC_WASM_DIRS = saved;
    rmSync(top, { recursive: true, force: true });
  }
}
const missing = (run, name, hint = false) =>
  assert.throws(() => run(name), (e) => e instanceof PandocError && /exited with status 3/.test(e.message)
                && /given no directory/.test(e.message) === hint, name);

test("in Node, by default, no directory: an include fails, saying so", async () => {
  await inTree(undefined, ({ run }) => {
    assert.throws(() => run("part.txt"), (e) => e instanceof PandocError
      && /exited with status 3 \(it was given no directory: LIBPANDOC_WASM_DIRS\)$/.test(e.message));
  });
  await inTree("", ({ run }) => missing(run, "part.txt", true));
});

test("LIBPANDOC_WASM_DIRS: the working directory, read-only; nothing outside it", async () => {
  await inTree((top) => join(top, "work"), async ({ top, work, run }) => {
    assert.equal(run("part.txt"), "included");
    assert.equal(run("./part.txt"), "included");
    assert.equal(run("sub/deep.txt"), "deeper");
    assert.equal(run("sub/../part.txt"), "included");
    assert.equal(run(join(work, "part.txt")), "included");
    assert.equal(run("inner-link.txt"), "included"); // a link inside it
    for (const out of ["../outside.txt", join(top, "outside.txt"), "../common/part.txt", "link.txt",
                       "linkdir/outside.txt", "nonesuch"]) missing(run, out);
    // read-only: nothing written, not a new file, not over one
    const writer = await filter("misbehave", { stderr: () => {} });
    for (const name of ["new.txt", "part.txt"]) {
      assert.throws(() => pandoc.convertWithFilters({ from: "markdown", to: "plain" },
                                                    "```write\n" + name + "\n```\n", [writer]),
                    (e) => e instanceof PandocError && /exited with status 3/.test(e.message), name);
    }
    assert.equal(readFileSync(join(work, "part.txt"), "utf8"), "included");
    assert.throws(() => readFileSync(join(work, "new.txt")));
  });
  await inTree(".", ({ run }) => assert.equal(run("part.txt"), "included"));
});

test("LIBPANDOC_WASM_DIRS: the parent, reached through .. as natively; not normalised", async () => {
  await inTree((top) => top, ({ top, run }) => {
    assert.equal(run("../common/part.txt"), "common");
    assert.equal(run(join(top, "common", "part.txt")), "common");
    assert.equal(run("../outside.txt"), "OUTSIDE");
    assert.equal(run("part.txt"), "included"); // the working directory inside it, as .
    assert.equal(run("../work/part.txt"), "included");
    // wasi-libc matches names as written: these match "." (the working
    // directory), which they would leave
    missing(run, "./../common/part.txt");
    missing(run, "sub/../../common/part.txt");
  });
});

test("LIBPANDOC_WASM_DIRS: a sibling, and a list", async () => {
  await inTree("../common", ({ top, run }) => {
    assert.equal(run("../common/part.txt"), "common");
    assert.equal(run(join(top, "common", "part.txt")), "common");
    missing(run, "part.txt");
    missing(run, "../outside.txt");
    missing(run, "../common/../outside.txt");
  });
  await inTree(["../common", "."].join(delimiter), ({ run }) => {
    assert.equal(run("../common/part.txt"), "common");
    assert.equal(run("part.txt"), "included");
  });
});

test("LIBPANDOC_WASM_DIRS: a missing directory, or a file, is an error naming it", async () => {
  await inTree((top) => [join(top, "work"), join(top, "nonesuch")].join(delimiter), ({ top, run }) => {
    assert.throws(() => run("part.txt"), (e) => e instanceof PandocError
      && e.message.includes(`include.wasm: LIBPANDOC_WASM_DIRS: ${join(top, "nonesuch")} does not exist`));
  });
  await inTree("part.txt", ({ run }) => {
    assert.throws(() => run("part.txt"), /LIBPANDOC_WASM_DIRS: part.txt is not a directory/);
  });
});

test("preopens given, also [], replace LIBPANDOC_WASM_DIRS", async () => {
  await inTree(".", async ({ run }) => {
    assert.equal(run("part.txt"), "included");
    const blind = await filter("include", { preopens: [], stderr: () => {} });
    assert.throws(() => pandoc.convertWithFilters({ from: "markdown", to: "plain" }, "```include\npart.txt\n```\n", [blind]),
                  (e) => e instanceof PandocError && /exited with status 3 \(it was given no directory\)$/.test(e.message));
    const sub = await filter("include", { preopens: hostPreopens(["sub"]), stderr: () => {} });
    const runSub = (name) => pandoc.convertWithFilters({ from: "markdown", to: "plain" },
                                                       "```include\n" + name + "\n```\n", [sub]).trim();
    assert.equal(runSub("sub/deep.txt"), "deeper");
    missing(runSub, "part.txt");
  });
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

test("a memory limit: its memory can't grow past it", async () => {
  const hog = await filter("misbehave", { maxMemory: 64 << 20, stderr: () => {} });
  assert.throws(() => pandoc.convertWithFilters({ from: "markdown", to: "plain" }, "```hog\n```\n", [hog]),
                (e) => e instanceof PandocError && /memory limit: 67108864 bytes/.test(e.message));
  assert.ok(hog.memory <= 64 << 20, `${hog.memory}`);
  // the filter failed, not libpandoc.wasm: the instance goes on
  assert.equal(pandoc.convert({ from: "markdown", to: "plain" }, "*still here*"), "still here\n");
  // within it, as without
  const upper = await filter("upper", { maxMemory: 64 << 20 });
  assert.equal(pandoc.convertWithFilters({ from: "markdown", to: "plain" }, "hi", [upper]), "HI\n");
  await assert.rejects(filter("upper", { maxMemory: 1 }), /over its limit/);
  assert.equal(parseMemory("64m"), 64 << 20);
  assert.equal(parseMemory(""), null);
  assert.throws(() => parseMemory("12x"));
});

// Consecutive filters, wasm ones too, are one exchange with pandoc: what one
// writes the next is given, unchecked, unless separateFilters.
test("wasm filters in a run give what they give apart, mixed with JS ones, in order", async () => {
  const [upper, modify, identity] = await Promise.all(["upper", "modify", "identity"].map((n) => filter(n)));
  const input = "# A head\n\nhello [link](x.md) `code`{.c}\n";
  const run = (fns, separateFilters) => pandoc.convertWithFilters({ from: "markdown", to: "plain" }, input, fns,
                                                                  { separateFilters });
  assert.equal(run([upper, modify], false), run([upper, modify], true));
  const push = (s) => (doc) => { doc.blocks.push({ t: "Para", c: [{ t: "Str", c: s }] }); return doc; };
  const mixed = [push("b"), upper, push("c"), identity, push("d")];
  assert.equal(run(mixed, false), run(mixed, true));
  assert.match(run(mixed, false), /HELLO.*\n\nB\n\nc\n\nd\n$/s);
});

test("what a filter on bytes wrote a wasm filter is given unchecked; failing on it, it says whose it was", async () => {
  const upper = await filter("upper", { stderr: () => {} });
  // (a function marked bytes, as a wasm filter's is, writing what isn't a document)
  const garble = Object.assign(() => "not a document", { bytes: true });
  const run = (separateFilters) => () => pandoc.convertWithFilters({ from: "markdown", to: "plain" }, "x",
                                                                     [garble, upper], { separateFilters });
  assert.throws(run(false), (e) => e instanceof PandocError
    && /fns\[1\]: upper\.wasm exited with status/.test(e.message)
    && /\(its input is what fns\[0\] wrote, unchecked: LIBPANDOC_SEPARATE_FILTERS=1 has pandoc check it\)/.test(e.message));
  // apart, pandoc refuses it after the first
  assert.throws(run(true), (e) => e instanceof PandocError && /invalid document/.test(e.message)
    && !/its input is what/.test(e.message));
});
