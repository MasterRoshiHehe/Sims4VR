#!/usr/bin/env bash
# Restore the build toolchain in a fresh container (seconds). Run: bash tools/setup-toolchain.sh
# Then: source tools/toolchain/env.sh   (also done automatically by tools/release.sh)
# Rebuild the archives (rare): bash tools/setup-toolchain.sh --rebuild
# Provides: Python 3.7 (PY37), mingw-w64 posix + cmake (apt), cached CMake deps (S4VR_DEPS -> FETCHCONTENT_SOURCE_DIR_*).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TC="$ROOT/tools/toolchain"; LOCAL="${S4VR_TC_LOCAL:-/opt/s4vr-toolchain}"
mkdir -p "$TC"
if [ "${1:-}" = "--rebuild" ]; then
  mkdir -p "$LOCAL"; W="$(mktemp -d -p "$LOCAL")"; cd "$W"
  git clone -q --depth 1 -b v3.7.17 https://github.com/python/cpython src
  (cd src && ./configure -q --prefix="$LOCAL/py37" >/dev/null && make -j"$(nproc)" >/dev/null && make install >/dev/null)
  tar -C "$LOCAL" -czf "$TC/py37.tar.gz" py37
  rm -rf "$W"; echo "rebuilt $TC/py37.tar.gz"; exit 0
fi
if ! command -v x86_64-w64-mingw32-g++-posix >/dev/null || ! command -v cmake >/dev/null; then
  (apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq mingw-w64 cmake) >/dev/null 2>&1 \
    || echo "warn: apt install mingw-w64 cmake failed (no network?)" >&2
fi
for t in gcc g++; do
  [ -e "/usr/bin/x86_64-w64-mingw32-$t-posix" ] && update-alternatives --set "x86_64-w64-mingw32-$t" "/usr/bin/x86_64-w64-mingw32-$t-posix" >/dev/null 2>&1 || true
done
if [ ! -x "$LOCAL/py37/bin/python3.7" ]; then
  mkdir -p "$LOCAL"; tar -C "$LOCAL" -xzf "$TC/py37.tar.gz"
fi
if [ -f "$TC/deps.tar.gz" ] && [ ! -d "$LOCAL/deps" ]; then tar -C "$LOCAL" -xzf "$TC/deps.tar.gz"; fi
"$LOCAL/py37/bin/python3.7" -c 'import sys,ast,py_compile; assert sys.version_info[:2]==(3,7)'
cat > "$TC/env.sh" <<EOT
export PY37="$LOCAL/py37/bin/python3.7"
export S4VR_DEPS="$LOCAL/deps"
EOT
echo "toolchain ok: PY37=$LOCAL/py37/bin/python3.7"
