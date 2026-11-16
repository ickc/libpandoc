#!/usr/bin/env bash
# Build libpandoc.wasm with GHC's wasm backend and stage it in $1 (default
# dist/wasm):
#
#   $1/libpandoc.wasm   optimized (wasm-opt -Oz), without debug info
#   $1/libpandoc-wasm-THIRD-PARTY-NOTICES.txt
#                       the licences of what it is built from (notices.py)
#
# The toolchain (ghc-wasm-meta at $GHC_WASM_META_REV, from pins.env) goes in
# $GHC_WASM_PREFIX (default ~/.ghc-wasm), installed unless already there,
# as pandoc-forge's build-wasm.sh does. WASM_OPT=0 skips the optimization
# (it takes about 20 s and 3.5 GB of memory).
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-dist/wasm}
prefix=${GHC_WASM_PREFIX:-$HOME/.ghc-wasm}
# shellcheck disable=SC1091
source <(grep -E '^[A-Z0-9_]+=' pins.env | sed 's/^/export /')

if [[ ! -f $prefix/env ]] || { [[ -f $prefix/.ghc-wasm-meta-rev ]] &&
	[[ $(cat "$prefix/.ghc-wasm-meta-rev") != "$GHC_WASM_META_REV" ]]; }; then
	tmp=$(mktemp -d)
	# by git, so that what runs is the commit pinned (an archive is made
	# on demand, and checked by nothing). Its setup.sh downloads the wasm
	# GHC, wasi-sdk, Node, binaryen and wasmtime from the URLs of its
	# autogen.json, which also has their hashes, but doesn't check them.
	git -C "$tmp" init -q
	git -C "$tmp" fetch -q --depth 1 https://gitlab.haskell.org/haskell-wasm/ghc-wasm-meta.git "$GHC_WASM_META_REV"
	git -C "$tmp" checkout -q FETCH_HEAD
	# setup.sh wipes $PREFIX: keep the cabal store (maybe from a CI cache)
	if [[ -d $prefix/.cabal ]]; then mv "$prefix/.cabal" "$tmp/.cabal-keep"; fi
	(cd "$tmp" && PREFIX="$prefix" ./setup.sh)
	if [[ -d $tmp/.cabal-keep ]]; then mv "$tmp/.cabal-keep" "$prefix/.cabal"; fi
	echo "$GHC_WASM_META_REV" >"$prefix/.ghc-wasm-meta-rev"
	rm -rf "$tmp"
fi
# shellcheck disable=SC1091
source "$prefix/env"

opts=(--project-file=cabal-wasm.project --builddir=dist-wasm
	--index-state="$INDEX_STATE" --constraint="pandoc ==$PANDOC_VERSION")
wasm32-wasi-cabal update "hackage.haskell.org,$INDEX_STATE"
wasm32-wasi-cabal build "${opts[@]}" exe:libpandoc-wasm
# list-bin may print the git checkouts first
bin=$(wasm32-wasi-cabal list-bin "${opts[@]}" exe:libpandoc-wasm | tail -1)
mkdir -p "$out"
if [[ ${WASM_OPT:-1} == 0 ]]; then
	cp "$bin" "$out/libpandoc.wasm"
else
	# no --all-features: it writes encodings older engines can't read
	wasm-opt -Oz --strip-debug "$bin" -o "$out/libpandoc.wasm"
fi
ls -l "$out/libpandoc.wasm"
"$(bash scripts/python.sh)" scripts/notices.py --plan dist-wasm/cache/plan.json \
	--component exe:libpandoc-wasm --store "$(wasm32-wasi-cabal path --store-dir | tail -1)" \
	--ghc-libdir "$(wasm32-wasi-ghc --print-libdir)" --target wasm \
	--out "$out/libpandoc-wasm-THIRD-PARTY-NOTICES.txt" \
	--artifact "This is libpandoc.wasm (and so libpandoc-pyodide.mjs, which loads it)."
