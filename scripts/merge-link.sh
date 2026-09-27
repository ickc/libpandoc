#!/usr/bin/env bash
# The linker GHC runs for libpandoc (ghc-options: -pgml, set by build.sh on
# Linux and macOS): links the Haskell packages into libpandoc itself, from
# their static archives, instead of depending on a shared library per
# package. Loading libpandoc then takes ~8 ms instead of ~70 ms: the dynamic
# linker otherwise looks symbols up across ~200 libraries.
#
# That needs the archives to be position-independent.
# - Linux: build.sh builds every package with -fPIC -fexternal-dynamic-refs,
#   rebuilding GHC's reinstallable boot packages from source. GHC's own
#   non-reinstallable libraries (base, ghc-internal, template-haskell, ...)
#   come only non-PIC, so they stay shared (~11 libraries). Their symbols are
#   the only ones libpandoc.so leaves unresolved; those of the archives are
#   local to it (--exclude-libs).
# - macOS: all code is position-independent, GHC's libraries and the runtime
#   included, so every Haskell package is linked in. Only the symbols of
#   libpandoc.def are exported.
#
# Any other link (the library component's own shared library, say) passes
# through.
set -euo pipefail
cc=${LIBPANDOC_CC:-cc}
ghclib=${LIBPANDOC_GHC_LIBDIR:?set by build.sh: ghc --print-libdir}
here=$(cd "$(dirname "$0")" && pwd)

# GHC may pass a long command line as a response file (@file)
args=()
for a in "$@"; do
	if [[ $a == @* && -f ${a#@} ]]; then
		while IFS= read -r line; do
			# one argument per line, as GHC writes them, quoted and escaped
			line=${line#\"}
			line=${line%\"}
			args+=("$(printf '%b' "${line//\\\"/\"}")")
		done <"${a#@}"
	else
		args+=("$a")
	fi
done

out=""
prev=""
for a in "${args[@]}"; do
	[[ $prev == -o ]] && out=$a
	prev=$a
done
case $out in
*/libpandoc.so* | */libpandoc.dylib*) ;;
*) exec "$cc" "${args[@]}" ;;
esac
darwin=""
[[ $(uname -s) == Darwin ]] && darwin=1

# the static archives there are: those of the packages cabal built (in the
# -L directories on Linux, next to their shared libraries; in a directory per
# package in cabal's store on macOS, which has the shared libraries of all in
# one), and on macOS GHC's own (in a directory per package under its libdir)
index=$(mktemp)
trap 'rm -f "$index"' EXIT
for a in "${args[@]}"; do
	if [[ $a == -L* && -d ${a#-L} ]]; then
		d=${a#-L}
		if [[ $d == "$ghclib"* ]]; then
			continue
		fi
		find "$d" -maxdepth 1 -name 'libHS*.a' >>"$index"
	fi
done
if [[ -n $darwin ]]; then
	find "$ghclib" -name 'libHS*.a' >>"$index"
	find "${LIBPANDOC_STORE_DIR:?set by build.sh: cabal path --store-dir}" \
		-maxdepth 4 -name 'libHS*.a' >>"$index"
fi

kept=()
archives=()
for a in "${args[@]}"; do
	if [[ $a =~ ^-l(HS.+)-ghc[0-9.]+$ ]]; then
		# exact basename only (libHSfoo-1.0.a, not libHSfoo-1.0_p.a)
		found=$(awk -v n="lib${BASH_REMATCH[1]}.a" -F/ '$NF == n {print; exit}' "$index")
		if [[ -n $found ]]; then
			archives+=("$found")
			continue
		fi
	fi
	kept+=("$a")
done

echo "merge-link: ${#archives[@]} Haskell packages linked in," \
	"$( (printf '%s\n' "${kept[@]}" | grep -c '^-lHS') || true) shared" >&2
if [[ -n $darwin ]]; then
	# ld64 searches archives repeatedly, so no grouping is needed
	exports=$(mktemp)
	trap 'rm -f "$index" "$exports"' EXIT
	awk '/^EXPORTS/ {e = 1; next} e && NF {print "_" $1}' \
		"$here/../libpandoc.def" >"$exports"
	"$cc" "${kept[@]}" ${archives[@]+"${archives[@]}"} -Wl,-exported_symbols_list,"$exports"
else
	exec "$cc" "${kept[@]}" -Wl,--exclude-libs,ALL \
		-Wl,--start-group ${archives[@]+"${archives[@]}"} -Wl,--end-group
fi
