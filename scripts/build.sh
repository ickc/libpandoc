#!/usr/bin/env bash
# Build libpandoc with the pins in pins.env and stage it into $1 (default
# dist/). Expects ghc and cabal of the pinned versions on PATH.
#
# Extra cabal.project.local lines can be given in $CABAL_PROJECT_EXTRA
# (e.g. extra-lib-dirs), and extra cabal flags in $CABALOPTS.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-dist}

set -a
# shellcheck source=pins.env
. ./pins.env
set +a

{
	echo "index-state: $INDEX_STATE"
	echo "constraints: pandoc ==$PANDOC_VERSION"
	case "$(uname -s)" in
	MINGW* | MSYS* | CYGWIN*) ;; # a standalone DLL, linked statically
	Linux)
		# cabal links foreign libraries against Haskell shared libraries...
		echo "shared: True"
		# ...but scripts/merge-link.sh then links the packages cabal builds
		# into libpandoc.so from their static archives, which must be
		# position-independent: every package built -fPIC, and GHC's
		# reinstallable boot packages rebuilt so (the others, which
		# template-haskell's installed instance needs, stay shared)
		echo "constraints: binary source, bytestring source, containers source,"
		echo "             directory source, exceptions source, filepath source,"
		echo "             mtl source, os-string source, parsec source, process source,"
		echo "             stm source, text source, time source, transformers source,"
		echo "             unix source"
		echo "package *"
		echo "  ghc-options: -fPIC -fexternal-dynamic-refs"
		echo "package libpandoc"
		echo "  ghc-options: -pgml $PWD/scripts/merge-link.sh"
		;;
	*)
		# cabal links foreign libraries against Haskell shared libraries
		echo "shared: True"
		;;
	esac
	echo "${CABAL_PROJECT_EXTRA:-}"
} >cabal.project.local
LIBPANDOC_GHC_LIBDIR=$(ghc --print-libdir)
export LIBPANDOC_GHC_LIBDIR

cabal update
# shellcheck disable=SC2086
cabal build ${CABALOPTS:-} flib:pandoc
bash scripts/stage.sh "$out"
