# shellcheck shell=bash
# Local development environment: ghcup's GHC/cabal and the pixi env's gmp.
# Source it: . scripts/env.sh
HASKELL_HOME=${HASKELL_HOME:-$HOME/.local/opt/Linux-x86_64/haskell}
export PATH=$HASKELL_HOME/.ghcup/bin:$PATH
export CABAL_DIR=${CABAL_DIR:-$HASKELL_HOME/cabal}
PIXI_PREFIX=$(pixi info --json | python3 -c "import json,sys; print(json.load(sys.stdin)['environments_info'][0]['prefix'])")
export LIBRARY_PATH=$PIXI_PREFIX/lib${LIBRARY_PATH:+:$LIBRARY_PATH}
export C_INCLUDE_PATH=$PIXI_PREFIX/include${C_INCLUDE_PATH:+:$C_INCLUDE_PATH}
