#!/usr/bin/env bash
# One-step build: claims the next version, checks, builds, packages, tags. Never installs.
#   tools/release.sh [--thread NAME] [--dry-run] [--no-dll] [--allow-dirty]
#   --dry-run     checks + packaging into release/.dry/ (no version claimed, no tag, no commit needed)
#   --no-dll      skip the CMake build, reuse the DLL/exe files of the newest release
#   --allow-dirty build even with uncommitted changes (the version line then says "+dirty")
# Env: PY37 (Python 3.7 for the .ts4script; default python3.7 / python3), BUILD_DIR (default build-release).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
THREAD=""; DRY=0; NODLL=0; DIRTY=0
while [ $# -gt 0 ]; do case "$1" in
  --thread) THREAD="$2"; shift;; --dry-run) DRY=1;; --no-dll) NODLL=1;; --allow-dirty) DIRTY=1;;
  *) echo "unknown option $1" >&2; exit 2;; esac; shift; done

REL="$ROOT/release"; mkdir -p "$REL"
exec 9>"$REL/.lock"; flock 9          # one release at a time; version claimed under this lock

PYBIN="${PY37:-$(command -v python3.7 || command -v python3)}"
COMMIT="$(git rev-parse --short HEAD)"
PATHS="src scriptmod config tools CMakeLists.txt"
if [ -n "$(git status --porcelain -- $PATHS)" ]; then
  if [ $DIRTY = 1 ] || [ $DRY = 1 ]; then COMMIT="$COMMIT+dirty"
  else echo "Uncommitted changes in: $PATHS (commit them or use --allow-dirty):" >&2
       git status --short -- $PATHS >&2; exit 1; fi
fi

# 1. Python syntax check against 3.7 grammar, before anything is built.
"$PYBIN" -I - scriptmod <<'PY'
import ast, os, sys
bad = 0
for d, _, fs in os.walk(sys.argv[1]):
    for f in fs:
        if f.endswith('.py'):
            p = os.path.join(d, f)
            try:
                ast.parse(open(p, encoding='utf-8').read(), p, feature_version=(3, 7))
            except SyntaxError as e:
                print('SYNTAX', p, e, file=sys.stderr); bad += 1
sys.exit(1 if bad else 0)
PY
echo "syntax ok"

# 2. Claim the version: highest of release/ dirs and git tags, +1. mkdir is the claim.
LAST="$( { ls "$REL" | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$'; git tag -l 'v[0-9]*'; } | sort -V | tail -1)"
PREV="$LAST"
if [ $DRY = 1 ]; then VER="${LAST#v}+dry"; OUT="$REL/.dry"; rm -rf "$OUT"; mkdir -p "$OUT"
else
  VER="${LAST#v}"; VER="${VER%.*}.$(( ${VER##*.} + 1 ))"; OUT="$REL/v$VER"
  mkdir "$OUT"                          # fails if somebody already holds it
fi
echo "version $VER commit $COMMIT"
PKG="$OUT/sims4vr"; mkdir -p "$PKG"

# 3. DLL + extras.
if [ $NODLL = 1 ]; then
  for f in openxr_loader.dll sf_hook.dll sf_launch.exe sims4vr.dll sims4vr_nofade.package; do
    cp "$REL/$PREV/sims4vr/$f" "$PKG/" 2>/dev/null || echo "warn: $f not in $PREV"; done
  cp "$REL/$PREV/sims4vr.map" "$OUT/" 2>/dev/null || true
else
  B="${BUILD_DIR:-$ROOT/build-release}"
  cmake -S . -B "$B" -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/mingw-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DS4VR_BUILD_VERSION="$VER" -DS4VR_BUILD_COMMIT="$COMMIT" >/dev/null
  cmake --build "$B" -j"$(nproc)" --target sims4vr
  cp "$B"/package/sims4vr/*.dll "$B"/package/sims4vr/*.exe "$B"/package/sims4vr/*.package "$PKG/" 2>/dev/null || true
  cp "$B/sims4vr.map" "$OUT/"
fi

# 4. Script mod (needs real Python 3.7 for the .pyc) and ini.
if "$PYBIN" -c 'import sys; sys.exit(sys.version_info[:2]!=(3,7))'; then
  S4VR_BUILD_VERSION="$VER" S4VR_BUILD_COMMIT="$COMMIT" "$PYBIN" -I tools/build_scriptmod.py "$PKG" >/dev/null
else
  [ $DRY = 1 ] && echo "dry run: no Python 3.7, .ts4script not packaged" || { echo "Python 3.7 required (set PY37)" >&2; exit 1; }
fi
cp config/sims4vr.ini "$PKG/sims4vr.ini"

# 5. CHANGES.txt from the commits since the previous build, then tag.
{ echo "sims4vr v$VER  commit $COMMIT  thread: ${THREAD:-?}  $(date -u +%FT%TZ)"
  if git rev-parse -q --verify "refs/tags/$PREV" >/dev/null; then git log --oneline "$PREV..HEAD"; else git log --oneline -20; fi
} > "$OUT/CHANGES.txt"
if [ $DRY = 0 ]; then git tag "v$VER" 2>/dev/null || echo "tag v$VER exists"; fi
echo "built $OUT (not installed)"
