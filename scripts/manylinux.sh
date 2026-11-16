#!/usr/bin/env bash
# Build and stage libpandoc inside a manylinux container, so that it (and the
# Python wheels that bundle it) runs on any Linux with glibc >= 2.28; or in a
# musllinux one (Alpine: GHC's Alpine bindist, through ghcup), for any Linux
# with musl >= 1.2.
#
#   podman run --rm -v "$PWD:/work" -v "$HOME/.cache/libpandoc-manylinux:/cache" \
#     -w /work quay.io/pypa/manylinux_2_28_x86_64 bash scripts/manylinux.sh dist
#   podman run --rm -v "$PWD:/work" -v "$HOME/.cache/libpandoc-musllinux:/cache" \
#     -w /work quay.io/pypa/musllinux_1_2_x86_64 bash scripts/manylinux.sh dist
#
# /cache (optional) keeps ghcup's GHC and the cabal store between runs (one
# per libc).
set -euo pipefail
out=${1:-dist}

set -a
# shellcheck source=pins.env
. ./pins.env
set +a

if command -v apk >/dev/null; then
	apk add -q gmp-dev zlib-dev ncurses-dev libffi-dev xz patchelf curl git
else
	dnf install -y -q gmp-devel zlib-devel ncurses-compat-libs xz >/dev/null
fi

cache=/cache
mkdir -p "$cache"
export GHCUP_INSTALL_BASE_PREFIX=$cache CABAL_DIR=$cache/cabal GHCUP_SKIP_UPDATE_CHECK=1
export PATH=$cache/.ghcup/bin:$PATH
arch=$(uname -m)
# ghcup itself, at the version and SHA-256 of pins.env (also when it comes
# from the cache)
ghcup_sha256=GHCUP_SHA256_${arch^^}
if ! echo "${!ghcup_sha256}  $cache/.ghcup/bin/ghcup" | sha256sum -c --status 2>/dev/null; then
	mkdir -p "$cache/.ghcup/bin"
	curl -sSfL -o "$cache/.ghcup/bin/ghcup.new" \
		"https://downloads.haskell.org/~ghcup/$GHCUP_VERSION/$arch-linux-ghcup-$GHCUP_VERSION"
	echo "${!ghcup_sha256}  $cache/.ghcup/bin/ghcup.new" | sha256sum -c
	chmod +x "$cache/.ghcup/bin/ghcup.new"
	mv "$cache/.ghcup/bin/ghcup.new" "$cache/.ghcup/bin/ghcup"
fi
# ghcup checks what it downloads against its metadata: the file of the
# commit pins.env names, not the moving one
metadata=https://raw.githubusercontent.com/haskell/ghcup-metadata/$GHCUP_METADATA_REV/ghcup-0.1.0.yaml
ghcup -s "$metadata" install ghc "$GHC_VERSION" --set
if [[ $arch == aarch64 ]]; then
	# ghcup's aarch64 glibc cabal needs a newer glibc than manylinux_2_28's
	# (2.34 for 3.18); its Alpine build is static and runs on either libc,
	# so it is taken by hand, checked against pins.env
	if ! { cabal --numeric-version | grep -qx "$CABAL_VERSION"; } 2>/dev/null; then
		curl -sSfL -o /tmp/cabal.tar.xz \
			"https://downloads.haskell.org/~ghcup/unofficial-bindists/cabal/$CABAL_VERSION/cabal-install-$CABAL_VERSION-aarch64-linux-alpine.tar.xz"
		echo "$CABAL_ALPINE_AARCH64_SHA256  /tmp/cabal.tar.xz" | sha256sum -c
		rm -f "$cache/.ghcup/bin/cabal"
		tar -xJf /tmp/cabal.tar.xz -C "$cache/.ghcup/bin" cabal
		rm /tmp/cabal.tar.xz
	fi
else
	ghcup -s "$metadata" install cabal "$CABAL_VERSION" --set
fi

git config --global --add safe.directory '*'
bash scripts/build.sh "$out"

# the container runs as root; hand the results back to the caller
if [[ -n ${HOST_UID:-} ]]; then
	chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" /work "$cache"
fi
