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
	*)
		# cabal links foreign libraries against Haskell shared libraries
		echo "shared: True"
		;;
	esac
	echo "${CABAL_PROJECT_EXTRA:-}"
} >cabal.project.local

cabal update
# shellcheck disable=SC2086
cabal build ${CABALOPTS:-} flib:pandoc
bash scripts/stage.sh "$out"
