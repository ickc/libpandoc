#!/usr/bin/env bash
# The linker GHC runs for libpandoc (ghc-options: -pgml, set by build.sh on
# Linux): links the Haskell packages cabal built into libpandoc.so itself,
# from their static archives, instead of depending on a shared library per
# package. Loading libpandoc then takes ~8 ms instead of ~70 ms: the dynamic
# linker otherwise looks symbols up across ~200 libraries.
#
# That needs the archives to be position-independent (build.sh builds every
# package with -fPIC -fexternal-dynamic-refs, rebuilding GHC's reinstallable
# boot packages from source). GHC's own non-reinstallable libraries (base,
# ghc-internal, template-haskell, ...) come only non-PIC, so they stay shared
# (~11 libraries). Their symbols are the only ones libpandoc.so leaves
# unresolved; those of the archives are local to it (--exclude-libs).
#
# Any other link (the library component's own .so, say) passes through.
set -euo pipefail
cc=${LIBPANDOC_CC:-cc}
ghclib=${LIBPANDOC_GHC_LIBDIR:?set by build.sh: ghc --print-libdir}

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
*/libpandoc.so*) ;;
*) exec "$cc" "${args[@]}" ;;
esac

libdirs=()
for a in "${args[@]}"; do
	[[ $a == -L* ]] && libdirs+=("${a#-L}")
done

kept=()
archives=()
for a in "${args[@]}"; do
	if [[ $a =~ ^-l(HS.+)-ghc[0-9.]+$ ]]; then
		unit=${BASH_REMATCH[1]}
		found=""
		for d in "${libdirs[@]}"; do
			if [[ -f $d/lib$unit.a && $d != "$ghclib"* ]]; then
				found=$d/lib$unit.a
				break
			fi
		done
		if [[ -n $found ]]; then
			archives+=("$found")
			continue
		fi
	fi
	kept+=("$a")
done

echo "merge-link: ${#archives[@]} Haskell packages linked in," \
	"$(printf '%s\n' "${kept[@]}" | grep -c '^-lHS') shared" >&2
exec "$cc" "${kept[@]}" -Wl,--exclude-libs,ALL \
	-Wl,--start-group "${archives[@]}" -Wl,--end-group
