#!/usr/bin/env bash
# Print the pandoc API version (major.minor, e.g. 1.23) of a staged prefix,
# as the smoke test saved it. Versions the pandoc-api dependency, as in
# pandoc-feedstock.
set -euo pipefail
file=${1:?usage: api-version.sh PREFIX}/share/libpandoc/api-version.json
python3 -c 'import json,sys; v=json.load(open(sys.argv[1])); print(f"{v[0]}.{v[1]}")' "$file"
