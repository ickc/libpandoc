#!/usr/bin/env bash
# The Python 3 the build runs scripts with: python3, else python (Windows'
# runners), else the manylinux images' newest in /opt/python.
set -euo pipefail
for p in python3 python; do
	if command -v "$p" >/dev/null && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null; then
		command -v "$p"
		exit 0
	fi
done
shopt -s nullglob
found=(/opt/python/cp3*/bin/python3)
if ((${#found[@]})); then
	printf '%s\n' "${found[@]}" | sort -V | tail -1
	exit 0
fi
echo "no Python 3 (scripts/notices.py)" >&2
exit 1
