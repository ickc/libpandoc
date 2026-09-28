#!/usr/bin/env bash
# Build libpandoc.wasm with GHC's wasm backend (ghc-wasm-meta, in
# $GHC_WASM_PREFIX, default ~/.ghc-wasm, as pandoc-forge's build-wasm.sh
# installs it) and stage it in $1 (default dist/wasm).
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-dist/wasm}
prefix=${GHC_WASM_PREFIX:-$HOME/.ghc-wasm}
# shellcheck disable=SC1091
source "$prefix/env"
# shellcheck disable=SC1091
source <(grep -v '^#' pins.env | sed 's/^/export /')
opts=(--project-file=cabal-wasm.project --builddir=dist-wasm
      --index-state="$INDEX_STATE" --constraint="pandoc ==$PANDOC_VERSION")
wasm32-wasi-cabal update "hackage.haskell.org,$INDEX_STATE"
wasm32-wasi-cabal build "${opts[@]}" exe:libpandoc-wasm
mkdir -p "$out"
# list-bin may print the git checkouts first
bin=$(wasm32-wasi-cabal list-bin "${opts[@]}" exe:libpandoc-wasm | tail -1)
cp "$bin" "$out/libpandoc.wasm"
ls -l "$out/libpandoc.wasm"
