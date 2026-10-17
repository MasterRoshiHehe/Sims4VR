#!/bin/sh
# Builds the 32-bit Sims Forever parts (Sims 1 is 32-bit): sf_launch.exe and
# sf_hook.dll into $1 (default: build32/). Needs i686-w64-mingw32-g++.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$here/build32}
mkdir -p "$out"
src=$here/src/simsforever
CXX=i686-w64-mingw32-g++
FLAGS="-O2 -std=c++17 -Wall -Wextra -Wno-cast-function-type -Wno-missing-field-initializers -DWIN32_LEAN_AND_MEAN -static -s"
$CXX $FLAGS -shared -o "$out/sf_hook.dll" "$src/sf_hook.cpp" -luser32 -lgdi32
$CXX $FLAGS -municode -mwindows -o "$out/sf_launch.exe" "$src/sf_launch.cpp" -luser32 -lshell32
echo "built $out/sf_hook.dll $out/sf_launch.exe"
