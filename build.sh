#!/usr/bin/env bash
# =====================================================================
#  JARVIS SEARCH - build script (MSYS2 UCRT64)
#
#  With the UCRT64 toolchain on the PATH, run it from the project
#  folder:
#
#      cd /c/Users/you/projects/JarvisSearch
#      ./build.sh
#
#  or point it at the project from anywhere:
#
#      ./build.sh /c/Users/you/projects/JarvisSearch
#
#  The compiler can be chosen with CXX, for example:
#
#      CXX=clang++ ./build.sh
# =====================================================================
set -euo pipefail

#  The project folder: the first argument, or the folder this script
#  lives in.
PROJECT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
PROJECT="$(cd "$PROJECT" && pwd)"
OUT="$PROJECT/JarvisSearch.exe"

CXX="${CXX:-g++}"
command -v "$CXX" >/dev/null 2>&1 || {
    echo "error: g++ not found.  Open the MSYS2 UCRT64 shell and retry." >&2
    exit 1
}

SOURCES=(
    "$PROJECT/src/apps.cpp"
    "$PROJECT/src/fuzzy.cpp"
    "$PROJECT/src/main.cpp"
    "$PROJECT/src/providers.cpp"
    "$PROJECT/src/render.cpp"
    "$PROJECT/src/util.cpp"
)

for s in "${SOURCES[@]}"; do
    if [ ! -f "$s" ]; then
        echo "error: $s is missing.  Is '$PROJECT' the project folder?" >&2
        exit 1
    fi
done

echo "compiling with $("$CXX" --version | head -n 1)"

"$CXX" -std=c++17 -O2 -Wall -Wextra \
    -municode -mwindows -static \
    -o "$OUT" \
    "${SOURCES[@]}" \
    -lgdiplus -luser32 -lgdi32 -lshell32 -lshlwapi -lole32 -loleaut32 \
    -luuid -ladvapi32 -lcomctl32

echo "built $OUT"
ls -l "$OUT"

