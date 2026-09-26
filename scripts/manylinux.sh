#!/usr/bin/env bash
# Build and stage libpandoc inside a manylinux container, so that it (and the
# Python wheels that bundle it) runs on any Linux with glibc >= 2.28.
#
#   podman run --rm -v "$PWD:/work" -v "$HOME/.cache/libpandoc-manylinux:/cache" \
#     -w /work quay.io/pypa/manylinux_2_28_x86_64 bash scripts/manylinux.sh dist
#
# /cache (optional) keeps ghcup's GHC and the cabal store between runs.
set -euo pipefail
out=${1:-dist}

set -a
# shellcheck source=pins.env
. ./pins.env
set +a

dnf install -y -q gmp-devel zlib-devel ncurses-compat-libs xz >/dev/null

cache=/cache
mkdir -p "$cache"
export GHCUP_INSTALL_BASE_PREFIX=$cache CABAL_DIR=$cache/cabal
export PATH=$cache/.ghcup/bin:$PATH
if [[ ! -x $cache/.ghcup/bin/ghcup ]]; then
	mkdir -p "$cache/.ghcup/bin"
	curl -sSfL -o "$cache/.ghcup/bin/ghcup" \
		"https://downloads.haskell.org/~ghcup/$(uname -m)-linux-ghcup"
	chmod +x "$cache/.ghcup/bin/ghcup"
fi
ghcup install ghc "$GHC_VERSION" --set
ghcup install cabal "$CABAL_VERSION" --set

git config --global --add safe.directory '*'
bash scripts/build.sh "$out"

# the container runs as root; hand the results back to the caller
if [[ -n ${HOST_UID:-} ]]; then
	chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" /work "$cache"
fi
