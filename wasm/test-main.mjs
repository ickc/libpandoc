// node --test wasm/test-main.mjs: pandoc's command (pandoc_main) in
// libpandoc.wasm, each command line in a child Node process of its own, on
// that process's standard input, output and error. With PANDOC (a native
// pandoc of the same version), each is also compared with it: output,
// messages, exit status. With WASM_FILTERS (as test-wasm-filter.mjs), a
// wasm filter on the command line too.
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, test } from "node:test";

const path = process.env.LIBPANDOC_WASM ?? new URL("../dist/wasm/libpandoc.wasm", import.meta.url).pathname;
const native = process.env.PANDOC;
const filters = process.env.WASM_FILTERS;

// pandoc in libpandoc.wasm as a command: the working directory preopened
// at its own path and pandoc's; `-F name.wasm` run as wasm filters (as a
// host would: parse-args tells the filters, the wasm ones become callbacks)
const runner = `
  import { readFileSync } from "node:fs";
  import { load } from ${JSON.stringify(new URL("node.mjs", import.meta.url).href)};
  import { wasmFilter } from ${JSON.stringify(new URL("wasm-filter.mjs", import.meta.url).href)};
  const [wasm, times, ...args] = process.argv.slice(1);
  const cwd = process.cwd();
  const pandoc = await load(wasm, { preopens: { [cwd]: cwd } });
  pandoc.chdir(cwd);
  let parsed = {};
  try {
    parsed = pandoc.query({ query: "parse-args", args });
  } catch {
    // arguments pandoc refuses: pandoc_main says so
  }
  let options = {};
  if (parsed.filters?.some((f) => f.type === "json" && f.path.endsWith(".wasm"))) {
    const fns = [];
    const filtersJson = [];
    for (const f of parsed.filters) {
      if (f.type === "json" && f.path.endsWith(".wasm")) {
        filtersJson.push({ type: "callback", index: fns.length });
        fns.push(await wasmFilter(readFileSync(f.path), { name: f.path, pandoc }));
      } else filtersJson.push(f);
    }
    options = { filtersJson, filters: fns };
  }
  let status;
  for (let i = 0; i < Number(times); i++) status = pandoc.main(args, options);
  pandoc.close();
  process.exitCode = status;
`;

const work = mkdtempSync(join(tmpdir(), "libpandoc-wasm-main-"));
after(() => rmSync(work, { recursive: true, force: true }));
writeFileSync(join(work, "in.md"), "# Hi\n\n*there*\n");
writeFileSync(join(work, "warn.tex"), "\\input{nope}\n\nx\n");
writeFileSync(join(work, "print.lua"), "print('from Lua')\nfunction Str(s) return pandoc.Str(s.text:upper()) end\n");

function wasm(args, { input = "", times = 1, env = {} } = {}) {
  const r = spawnSync(process.execPath, ["--no-warnings", "--input-type=module", "-e", runner, "--", path, String(times), ...args],
                      { cwd: work, input, encoding: "utf8", env: { ...process.env, ...env } });
  return { status: r.status, stdout: r.stdout, stderr: r.stderr };
}

// the command line in libpandoc.wasm, and, with PANDOC, the same from the
// native command
function both(args, opts = {}) {
  const w = wasm(args, opts);
  if (native) {
    const n = spawnSync(native, args, { cwd: work, input: opts.input ?? "", encoding: "utf8" });
    assert.deepEqual(w, { status: n.status, stdout: n.stdout, stderr: n.stderr }, args.join(" "));
  }
  return w;
}

test("a conversion: from a file, from standard input", () => {
  const html = '<h1 id="hi">Hi</h1>\n<p><em>there</em></p>\n';
  assert.deepEqual(both(["in.md", "-t", "html"]), { status: 0, stdout: html, stderr: "" });
  assert.deepEqual(both(["-t", "html"], { input: "# Hi\n\n*there*\n" }), { status: 0, stdout: html, stderr: "" });
});

test("errors: pandoc's message on standard error, its exit status", () => {
  const missing = both(["missing.md"]);
  assert.equal(missing.status, 1);
  assert.match(missing.stderr, /missing\.md: withBinaryFile: does not exist/);
  assert.equal(both(["-f", "nonsense", "in.md"]).status, 21);
  assert.equal(both(["-t", "nonsense", "in.md"]).status, 22);
  assert.equal(both(["--no-such-option"]).status, 6);
});

test("warnings: printed, silenced by --quiet, failing with --fail-if-warnings", () => {
  assert.match(both(["warn.tex", "-t", "plain"]).stderr, /\[WARNING\] Could not load include file nope/);
  assert.deepEqual(both(["--quiet", "warn.tex", "-t", "plain"]), { status: 0, stdout: "x\n", stderr: "" });
  const failed = both(["--fail-if-warnings", "warn.tex", "-t", "plain"]);
  assert.equal(failed.status, 3);
  assert.match(failed.stderr, /Failing because there were warnings/);
});

test("--log and -o write their files in a directory the host gave", () => {
  for (const f of ["log.json", "out.html"]) rmSync(join(work, f), { force: true });
  assert.equal(wasm(["warn.tex", "--quiet", "--log=log.json", "-o", "out.html"]).status, 0);
  assert.match(readFileSync(join(work, "out.html"), "utf8"), /<p>x<\/p>/);
  const log = JSON.parse(readFileSync(join(work, "log.json"), "utf8"));
  assert.equal(log[0].type, "CouldNotLoadIncludeFile");
  if (native) {
    const n = spawnSync(native, ["warn.tex", "--quiet", "--log=log-native.json", "-o", "out-native.html"], { cwd: work });
    assert.equal(n.status, 0);
    assert.equal(readFileSync(join(work, "out.html"), "utf8"), readFileSync(join(work, "out-native.html"), "utf8"));
    assert.deepEqual(log, JSON.parse(readFileSync(join(work, "log-native.json"), "utf8")));
  }
});

test("--version and --help: pandoc's, saying what the module has", () => {
  const v = wasm(["--version"]);
  assert.equal(v.status, 0);
  assert.match(v.stdout, /^pandoc 3\.\d+/);
  assert.match(v.stdout, /Features: -server -lua/); // (the command: +server +lua)
  if (native) {
    const n = spawnSync(native, ["--version"], { encoding: "utf8" }).stdout;
    // (and no user data directory: there is no home in the module)
    const home = /User data directory: .*/;
    assert.equal(v.stdout.replace("-server -lua", "+server +lua").replace(home, ""), n.replace(home, ""));
  }
  const h = both(["--help"]);
  assert.match(h.stdout, /^pandoc \[OPTIONS\] \[FILES\]/);
});

test("filters: Lua runs inside (its print on standard output), JSON filters can't", () => {
  const lua = both(["in.md", "-t", "plain", "--lua-filter=print.lua"]);
  assert.deepEqual(lua, { status: 0, stdout: "from Lua\nHI\n\nTHERE\n", stderr: "" });
  const json = wasm(["in.md", "-F", "./some-filter"]);
  assert.equal(json.status, 83);
  assert.match(json.stderr, /libpandoc\.wasm runs no programs/);
});

test("an instance runs commands one after another; standard input is read once", () => {
  const twice = wasm(["in.md", "-t", "html"], { times: 2 });
  assert.equal(twice.status, 0);
  assert.equal(twice.stdout, '<h1 id="hi">Hi</h1>\n<p><em>there</em></p>\n'.repeat(2));
  // pandoc closes standard input once it has read it, as the command
  const stdin = wasm(["-t", "html"], { input: "*x*", times: 2 });
  assert.equal(stdin.stdout, "<p><em>x</em></p>\n");
  assert.equal(stdin.status, 1);
  assert.match(stdin.stderr, /handle is closed/);
});

test("a wasm filter on the command line, as a callback", { skip: !filters && "WASM_FILTERS not set" }, () => {
  const upper = join(filters, "upper.wasm");
  const r = wasm(["in.md", "-t", "plain", "-F", upper, "--lua-filter=print.lua"]);
  assert.deepEqual(r, { status: 0, stdout: "from Lua\nHI\n\nTHERE\n", stderr: "" });
  assert.equal(wasm(["in.md", "-t", "plain", "-F", upper]).stdout, "HI\n\nTHERE\n");
});

test("consecutive wasm filters on the command line: one exchange, unless LIBPANDOC_SEPARATE_FILTERS=1",
     { skip: !filters && "WASM_FILTERS not set" }, () => {
  const args = ["in.md", "-t", "plain", "--verbose", "-F", join(filters, "upper.wasm"), "-F", join(filters, "modify.wasm")];
  const callbacks = (v) => {
    const r = wasm(args, { env: { LIBPANDOC_SEPARATE_FILTERS: v } });
    assert.equal(r.status, 0, r.stderr);
    assert.equal(r.stdout, "HI\n\nTHERE\n");
    return r.stderr.match(/Running filter libpandoc:callback\//g).length;
  };
  assert.equal(callbacks(""), 1);
  assert.equal(callbacks("0"), 1);
  assert.equal(callbacks("1"), 2);
  const bad = wasm(args, { env: { LIBPANDOC_SEPARATE_FILTERS: "yes" } });
  assert.notEqual(bad.status, 0);
  assert.match(bad.stderr, /LIBPANDOC_SEPARATE_FILTERS: 1 .*, or 0 or empty, not "yes"/);
});
