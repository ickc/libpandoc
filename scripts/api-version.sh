#!/usr/bin/env bash
# Print the pandoc API version (major.minor, e.g. 1.23) of a staged prefix,
# from its AST schema. Versions the pandoc-api dependency, as in
# pandoc-feedstock.
set -euo pipefail
schema=${1:?usage: api-version.sh PREFIX}/share/libpandoc/ast-schema.json
python3 -c 'import json,sys; v=json.load(open(sys.argv[1]))["pandoc-api-version"]; print(f"{v[0]}.{v[1]}")' "$schema"
