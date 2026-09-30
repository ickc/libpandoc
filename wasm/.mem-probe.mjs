import { readFileSync } from "node:fs";
import { load } from "./node.mjs";
const manual = readFileSync(process.argv[2], "utf8");
const path = new URL("../dist/wasm/libpandoc.wasm", import.meta.url).pathname;
for (const n of process.argv.slice(3).map(Number)) {
  const pandoc = await load(path);
  const input = Array(n).fill(manual).join("\n\n");
  const t = performance.now();
  try {
    pandoc.convert({ from: "markdown", to: "html" }, input);
    console.log(`x${n} (${(input.length / 1e6).toFixed(1)} MB): ok, ${((performance.now() - t) / 1000).toFixed(1)} s, linear memory ${(pandoc.exports.memory.buffer.byteLength / 2 ** 20).toFixed(0)} MiB`);
  } catch (e) {
    console.log(`x${n} (${(input.length / 1e6).toFixed(1)} MB): FAILED after ${((performance.now() - t) / 1000).toFixed(1)} s, memory ${(pandoc.exports.memory.buffer.byteLength / 2 ** 20).toFixed(0)} MiB: ${String(e.message ?? e).slice(0, 150)}`);
  }
}
