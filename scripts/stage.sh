#!/usr/bin/env bash
# Stage the built library into a self-contained prefix:
#
#   $1/include/libpandoc.h
#   $1/lib/libpandoc.so            (.dylib on macOS; bin/pandoc.dll and
#                                   lib/pandoc.def on Windows)
#   $1/lib/libpandoc/              (Linux, macOS: the Haskell shared libraries
#                                   libpandoc.so needs; its RPATH points here)
#   $1/share/libpandoc/ast-schema.json
#
# On Linux and macOS, GHC's static libraries are not position-independent
# (Linux) or cabal links foreign libraries dynamically (both), so libpandoc
# depends on the libHS*.so of every Haskell package. They are copied next to
# it with relative RPATHs. On Windows the DLL is standalone.
set -euo pipefail

out=$1
mkdir -p "$out/include" "$out/lib" "$out/share/libpandoc"
out=$(cd "$out" && pwd)
cp include/libpandoc.h "$out/include/"

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
	for so in "$out"/lib/libpandoc/*.so*; do
		patchelf --set-rpath '$ORIGIN' "$so"
	done
	patchelf --set-rpath '$ORIGIN/libpandoc' "$out/lib/libpandoc.so"
	;;
Darwin)
	lib=$(find dist-newstyle -name 'libpandoc.dylib' -path '*/f/pandoc/*' | head -1)
	cp "$lib" "$out/lib/libpandoc.dylib"
	install_name_tool -id @rpath/libpandoc.dylib "$out/lib/libpandoc.dylib"
	mkdir -p "$out/lib/libpandoc"
	# copy the Haskell dylibs it references (transitively), then point every
	# reference at @rpath
	todo=("$out/lib/libpandoc.dylib")
	while ((${#todo[@]})); do
		f=${todo[0]}
		todo=("${todo[@]:1}")
		while read -r dep; do
			base=$(basename "$dep")
			if [[ ! -e $out/lib/libpandoc/$base ]]; then
				src=$dep
				if [[ $dep == @rpath/* ]]; then
					src=$(otool -l "$f" | awk '/LC_RPATH/{getline; getline; print $2}' |
						while read -r d; do [[ -e $d/$base ]] && echo "$d/$base"; done | head -1)
				fi
				cp -L "$src" "$out/lib/libpandoc/$base"
				chmod u+w "$out/lib/libpandoc/$base"
				install_name_tool -id "@rpath/$base" "$out/lib/libpandoc/$base"
				todo+=("$out/lib/libpandoc/$base")
			fi
			install_name_tool -change "$dep" "@rpath/$base" "$f" 2>/dev/null || true
		done < <(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -E 'libHS|libffi')
	done
	for f in "$out/lib/libpandoc.dylib" "$out"/lib/libpandoc/*.dylib; do
		otool -l "$f" | awk '/LC_RPATH/{getline; getline; print $2}' | while read -r r; do
			install_name_tool -delete_rpath "$r" "$f"
		done
	done
	install_name_tool -add_rpath @loader_path/libpandoc "$out/lib/libpandoc.dylib"
	for f in "$out"/lib/libpandoc/*.dylib; do
		install_name_tool -add_rpath @loader_path "$f"
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
