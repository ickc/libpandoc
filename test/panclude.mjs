// PANCLUDE=<panclude's checkout> node --test test/panclude.mjs
//
// panclude's conformance cases (its tests/cases, as its SPEC.md, section
// 7, describes them) run against the include step built into libpandoc,
// as panclude's own runner (spec/cases.lua) runs them against its Lua
// filter: the cases marked `only: lua` skipped, those marked `only:
// builtin` run. pandoc is libpandoc's pandoc_main, one process a case,
// working in the case's directory:
//   - LIBPANDOC_COMMAND set: a native command on libpandoc (test/command.c);
//   - else libpandoc.wasm in Node (LIBPANDOC_WASM, by default
//     dist/wasm/libpandoc.wasm), with the case's directory, and a directory
//     for the log, preopened at their own paths.
// A line `{panclude}` of a case's arguments is the step, --filter=panclude
// (without one, the step goes first); a case's `roots` gives it as
// {"type": "panclude", "roots": [...]} (pandoc_main's filters_json); a
// case's `untrusted` makes it a conversion of its standard input with
// untrusted options, {"filters": ["panclude"], "untrusted": ...}
// (pandoc_convert). What a case expects is compared as panclude's runner
// compares it: the document as this pandoc writes it (-s -t native, the
// expected one read and written by the same), the warnings in its log (a
// ScriptingWarning's message, any other's pretty text), the exit status.
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, test } from "node:test";

const panclude = process.env.PANCLUDE;
const command = process.env.LIBPANDOC_COMMAND;
const wasm = process.env.LIBPANDOC_WASM ?? new URL("../dist/wasm/libpandoc.wasm", import.meta.url).pathname;
const casesDir = panclude && join(panclude, "tests", "cases");
const logs = mkdtempSync(join(tmpdir(), "libpandoc-panclude-"));
after(() => rmSync(logs, { recursive: true, force: true }));

// pandoc_main (or pandoc_convert with --convert, as test/command.c) in
// libpandoc.wasm, as a command
const driver = `
  import { readFileSync, writeFileSync } from "node:fs";
  import { load } from ${JSON.stringify(new URL("../wasm/node.mjs", import.meta.url).href)};
  const [wasm, logs, ...args] = process.argv.slice(1);
  const cwd = process.cwd();
  const pandoc = await load(wasm, { preopens: { [cwd]: cwd, [logs]: logs } });
  pandoc.chdir(cwd);
  if (args[0] === "--convert") {
    const [status, output, kind, message, log] = pandoc.abi.convert(args[1], readFileSync(0));
    writeFileSync(args[2], log);
    if (status === 0) process.stdout.write(output);
    else {
      process.stderr.write(kind + ": " + message + "\\n");
      process.exitCode = 1;
    }
  } else {
    const filters = process.env.LIBPANDOC_TEST_FILTERS;
    process.exitCode = pandoc.main(args, filters ? { filtersJson: JSON.parse(filters) } : {});
  }
  pandoc.close();
`;

function pandoc(args, { cwd, input = "", env = {} }) {
  const [file, argv] = command ? [command, args]
    : [process.execPath, ["--no-warnings", "--input-type=module", "-e", driver, "--", wasm, logs, ...args]];
  const r = spawnSync(file, argv, { cwd, input, encoding: "utf8", env: { ...process.env, ...env } });
  if (r.error) throw r.error;
  return { status: r.status, stdout: r.stdout, stderr: r.stderr };
}

const read = (file) => (existsSync(file) ? readFileSync(file, "utf8") : null);
const lines = (s) => (s ?? "").split("\n").filter((l) => l !== "");

function load(name) {
  const dir = join(casesDir, name);
  const stdin = read(join(dir, "stdin"));
  const args = read(join(dir, "args"));
  return {
    dir,
    stdin,
    args: args !== null ? lines(args) : stdin !== null ? [] : ["doc.md"],
    env: Object.fromEntries(lines(read(join(dir, "env"))).map((l) => {
      const i = l.indexOf("=");
      return [l.slice(0, i), l.slice(i + 1)];
    })),
    only: (read(join(dir, "only")) ?? "").trim(),
    roots: read(join(dir, "roots")),
    untrusted: read(join(dir, "untrusted")),
    expected: read(join(dir, "expected.native")),
    warnings: lines(read(join(dir, "warnings"))),
    status: Number((read(join(dir, "status")) ?? "0").trim()),
  };
}

// a ScriptingWarning's message, any other warning's text, as pandoc
// prints it
function warnings(log) {
  if (!log) return [];
  return JSON.parse(log).filter((m) => m.verbosity === "WARNING")
    .map((m) => (m.type === "ScriptingWarning" ? m.message : m.pretty));
}

function run(c, name) {
  const logFile = join(logs, `${name}.json`);
  const env = { ...c.env };
  let args;
  if (c.untrusted !== null) {
    const options = { filters: ["panclude"], to: "native", standalone: true, untrusted: JSON.parse(c.untrusted) };
    args = ["--convert", JSON.stringify(options), logFile];
  } else {
    const step = c.args.some((a) => a.includes("{panclude}"));
    args = c.args.flatMap((a) => (!a.includes("{panclude}") ? [a] : c.roots !== null ? [] : ["--filter=panclude"]));
    if (c.roots !== null) env.LIBPANDOC_TEST_FILTERS = JSON.stringify([{ type: "panclude", roots: lines(c.roots) }]);
    else if (!step) args.unshift("--filter=panclude");
    args.push("-s", "-t", "native", `--log=${logFile}`);
  }
  const r = pandoc(args, { cwd: c.dir, input: c.stdin ?? "", env });
  return { ...r, warnings: warnings(read(logFile)) };
}

const names = casesDir ? readdirSync(casesDir).filter((n) =>
  ["args", "doc.md", "stdin"].some((f) => existsSync(join(casesDir, n, f)))).sort() : [];

test("panclude's cases are there", { skip: !panclude && "PANCLUDE not set" }, () => {
  assert.ok(names.length > 0, `no cases in ${casesDir}`);
});

for (const name of names) {
  const c = load(name);
  test(name, { skip: c.only === "lua" && "only: lua" }, () => {
    const got = run(c, name);
    assert.equal(got.status, c.status, `exit status; stderr:\n${got.stderr}`);
    if (c.expected !== null) {
      const want = pandoc(["expected.native", "-f", "native", "-s", "-t", "native"], { cwd: c.dir });
      assert.equal(want.status, 0, `${name}/expected.native: ${want.stderr}`);
      assert.equal(got.stdout, want.stdout, "the document");
    }
    assert.deepEqual(got.warnings, c.warnings, "the warnings");
  });
}
