#!/usr/bin/env bash
# Bundle wasm/pyodide.mjs and what it imports (browser_wasi_shim included)
# into one ES module, $1/libpandoc-pyodide.mjs (default dist/wasm), for
# libpandoc-python's wheel for Pyodide, with browser_wasi_shim's MIT licence
# at the top (it is MIT or Apache-2.0). Needs `npm ci` in wasm/ first.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-dist/wasm}
mkdir -p "$out"
shim=wasm/node_modules/@bjorn3/browser_wasi_shim
{
	echo "/* libpandoc's wasm/pyodide.mjs (GPL-2.0-or-later), bundled with"
	echo "   @bjorn3/browser_wasi_shim $(node -p "require('./$shim/package.json').version"), under its MIT licence:"
	echo
	sed 's/^/   /; s/[[:space:]]*$//' "$shim/LICENSE-MIT"
	echo "*/"
	npx --yes esbuild@0.25.10 wasm/pyodide.mjs --bundle --format=esm --platform=neutral \
		--legal-comments=inline --log-level=warning
} >"$out/libpandoc-pyodide.mjs"
