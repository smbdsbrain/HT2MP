#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
skip_tests=0
if [[ "${1:-}" == "--skip-tests" ]]; then
  skip_tests=1
elif [[ $# -ne 0 ]]; then
  echo "usage: tools/build-server.sh [--skip-tests]" >&2
  exit 2
fi

cd "$repo_root"
cmake --preset server-linux-x64
cmake --build --preset build-server-linux-x64 --parallel
if [[ $skip_tests -eq 0 ]]; then
  ctest --preset test-server-linux-x64
fi
cmake --build --preset package-server-linux-x64

cache="$repo_root/build/server-linux-x64/CMakeCache.txt"
version="$(sed -n 's/^HT2MP_PACKAGE_VERSION:STRING=//p' "$cache" | head -n 1)"
if [[ -z "$version" ]]; then
  echo "HT2MP_PACKAGE_VERSION is missing from $cache" >&2
  exit 1
fi
dist="$repo_root/dist/server"
artifact="$dist/HT2MP-server-$version-linux-x86_64.tar.gz"
if [[ ! -f "$artifact" ]]; then
  echo "expected package was not produced: $artifact" >&2
  exit 1
fi
printf '%s\n' "$artifact"
