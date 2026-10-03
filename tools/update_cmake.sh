#!/usr/bin/env bash
# update_cmake.sh — sync a game project's CMakeLists.txt with the scaffold
# template's managed blocks (implementation: tools/update_cmake.py).
# Usage: update_cmake.sh [--project DIR] [--check] [--dry-run]
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${PYTHON:-}"
if [ -z "$PY" ]; then
  for c in python3 python; do command -v "$c" >/dev/null 2>&1 && { PY="$c"; break; }; done
fi
[ -n "$PY" ] || { echo "update_cmake: python not found on PATH" >&2; exit 1; }
exec "$PY" "$ROOT/tools/update_cmake.py" "$@"
