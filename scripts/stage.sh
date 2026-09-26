#!/usr/bin/env bash
# Stage the built library into a self-contained prefix:
#
#   $1/include/libpandoc.h
#   $1/lib/libpandoc.so            (.dylib on macOS; bin/pandoc.dll and
#                                   lib/pandoc.def on Windows)
#   $1/lib/libpandoc/              (Linux, macOS: the Haskell shared libraries
#                                   libpandoc.so needs; its RPATH points here)
#   $1/share/libpandoc/ast-schema.json
#   $1/share/libpandoc/examples/smoke.c
#
# On Linux and macOS, GHC's static libraries are not position-independent
# (Linux) or cabal links foreign libraries dynamically (both), so libpandoc
# depends on the libHS*.so of every Haskell package. They are copied next to
# it with relative RPATHs. On Windows the DLL is standalone.
set -euo pipefail

out=$1
mkdir -p "$out/include" "$out/lib" "$out/share/libpandoc/examples"
out=$(cd "$out" && pwd)
cp include/libpandoc.h "$out/include/"
cp COPYING.md "$out/"
cp test/smoke.c "$out/share/libpandoc/examples/"

case "$(uname -s)" in
Linux)
	lib=$(find dist-newstyle -name 'libpandoc.so.*.*.*' -path '*/f/pandoc/*' | head -1)
	cp "$lib" "$out/lib/libpandoc.so"
	patchelf --set-soname libpandoc.so "$out/lib/libpandoc.so"
	mkdir -p "$out/lib/libpandoc"
	# every Haskell library it loads (and GHC's libffi), resolved by the loader
	ldd "$out/lib/libpandoc.so" | awk '/=> \// {print $3}' |
		grep -E '/(libHS[^/]*|libffi[^/]*)$' | while read -r dep; do
		cp -L "$dep" "$out/lib/libpandoc/"
	done
	# RUNPATH isn't transitive: each library needs its own path to its
	# siblings and to lib/ (gmp, zlib: from conda, or the system)
	for so in "$out"/lib/libpandoc/*.so*; do
		patchelf --set-rpath '$ORIGIN:$ORIGIN/..' "$so"
	done
	patchelf --set-rpath '$ORIGIN/libpandoc:$ORIGIN' "$out/lib/libpandoc.so"
	;;
Darwin)
	lib=$(find dist-newstyle -name 'libpandoc.dylib' -path '*/f/pandoc/*' | head -1)
	cp "$lib" "$out/lib/libpandoc.dylib"
	install_name_tool -id @rpath/libpandoc.dylib "$out/lib/libpandoc.dylib"
	mkdir -p "$out/lib/libpandoc"
	# Every Haskell dylib that could be referenced, by name: GHC's boot
	# libraries use @loader_path-relative rpaths, which stop resolving once
	# copied, so references are looked up here instead. (macOS bash is 3.2:
	# no associative arrays.)
	index=$(mktemp)
	find "$(ghc --print-libdir)" "$(cabal path --store-dir)" dist-newstyle \
		-name 'lib*.dylib' >"$index" 2>/dev/null || true
	# Copy what libpandoc references, transitively (Haskell libraries, GHC's
	# libffi, and gmp), and point every reference at @rpath.
	todo=("$out/lib/libpandoc.dylib")
	while ((${#todo[@]})); do
		f=${todo[0]}
		todo=("${todo[@]:1}")
		for dep in $(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -E 'libHS|libffi|libgmp' || true); do
			base=$(basename "$dep")
			if [[ $base == "$(basename "$f")" ]]; then continue; fi
			if [[ ! -e $out/lib/libpandoc/$base ]]; then
				if [[ $dep == /* ]]; then
					src=$dep
				else
					src=$(grep "/$base\$" "$index" | head -1 || true)
				fi
				if [[ -z $src || ! -e $src ]]; then
					echo "cannot find $dep, needed by $f" >&2
					exit 1
				fi
				cp -L "$src" "$out/lib/libpandoc/$base"
				chmod u+w "$out/lib/libpandoc/$base"
				install_name_tool -id "@rpath/$base" "$out/lib/libpandoc/$base" 2>/dev/null
				todo+=("$out/lib/libpandoc/$base")
			fi
			install_name_tool -change "$dep" "@rpath/$base" "$f" 2>/dev/null
		done
	done
	rm -f "$index"
	for f in "$out/lib/libpandoc.dylib" "$out"/lib/libpandoc/*.dylib; do
		for r in $(otool -l "$f" | awk '/LC_RPATH/{getline; getline; print $2}'); do
			install_name_tool -delete_rpath "$r" "$f" 2>/dev/null
		done
	done
	install_name_tool -add_rpath @loader_path/libpandoc "$out/lib/libpandoc.dylib" 2>/dev/null
	for f in "$out"/lib/libpandoc/*.dylib; do
		install_name_tool -add_rpath @loader_path "$f" 2>/dev/null
	done
	# modified binaries need re-signing on Apple Silicon
	codesign --force -s - "$out/lib/libpandoc.dylib" "$out"/lib/libpandoc/*.dylib
	;;
MINGW* | MSYS* | CYGWIN*)
	mkdir -p "$out/bin"
	dll=$(find dist-newstyle -name 'pandoc.dll' -path '*/f/pandoc/*' | head -1)
	cp "$dll" "$out/bin/pandoc.dll"
	# the export list, for making an import library (MSVC: lib /def:pandoc.def)
	cp libpandoc.def "$out/lib/pandoc.def"
	;;
esac
