#!/usr/bin/env bash
# Build libpandoc.wasm with GHC's wasm backend and stage it in $1 (default
# dist/wasm):
#
#   $1/libpandoc.wasm   optimized (wasm-opt -Oz), without debug info
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
source <(grep -E '^[A-Z_]+=' pins.env | sed 's/^/export /')

if [[ ! -f $prefix/env ]] || { [[ -f $prefix/.ghc-wasm-meta-rev ]] &&
	[[ $(cat "$prefix/.ghc-wasm-meta-rev") != "$GHC_WASM_META_REV" ]]; }; then
	tmp=$(mktemp -d)
	curl -fL --retry 5 "https://gitlab.haskell.org/haskell-wasm/ghc-wasm-meta/-/archive/$GHC_WASM_META_REV/ghc-wasm-meta-$GHC_WASM_META_REV.tar.gz" |
		tar xz --strip-components=1 -C "$tmp"
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
