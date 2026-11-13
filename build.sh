#!/usr/bin/env bash
set -u

NAME="${1:-deid}"
if [ $# -gt 0 ]; then shift; fi

PRUNE=( -path ./dist -o -path ./.git -o -path './build*' -o -path './cmake-build*' )

SOURCES=()
if [ $# -gt 0 ]; then
    SOURCES=("$@")
else
    while IFS= read -r -d '' f; do
        SOURCES+=("$f")
    done < <(find . \( "${PRUNE[@]}" \) -prune -o -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) -print0)
fi

if [ ${#SOURCES[@]} -eq 0 ]; then
    echo "Не са намерени .cpp файлове."
    exit 1
fi

MAINS=()
for f in "${SOURCES[@]}"; do
    if grep -Eq '^[[:space:]]*int[[:space:]]+main[[:space:]]*\(' "$f"; then
        MAINS+=("$f")
    fi
done
if [ ${#MAINS[@]} -gt 1 ]; then
    echo "Намерени са няколко файла с main:"
    printf '  %s\n' "${MAINS[@]}"
    echo "Изредете файловете на всяка програма поотделно, например:"
    echo "  bash build.sh $NAME ${MAINS[0]} ..."
    exit 1
fi

INCLUDES=("-I.")
SEEN="|.|"
while IFS= read -r -d '' h; do
    d=$(dirname "$h")
    case "$SEEN" in
        *"|$d|"*) ;;
        *) SEEN="$SEEN$d|"; INCLUDES+=("-I$d") ;;
    esac
done < <(find . \( "${PRUNE[@]}" \) -prune -o -type f \( -name '*.hpp' -o -name '*.h' -o -name '*.hh' \) -print0)

FLAGS=(-std=c++17 -O2)
STATUS=0

build() {
    local label="$1" tool="$2" out="$3" post="$4"
    shift 4
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[$label] пропуснат: липсва $tool"
        return
    fi
    mkdir -p "$(dirname "$out")"
    if "$tool" "$@" "${FLAGS[@]}" ${INCLUDES[@]+"${INCLUDES[@]}"} "${SOURCES[@]}" -o "$out" $post; then
        echo "[$label] готово: $out"
    else
        echo "[$label] ГРЕШКА при компилиране"
        STATUS=1
    fi
}

build "macOS"        clang++                "dist/mac/$NAME"           ""                                  -arch arm64 -arch x86_64
build "Windows"      x86_64-w64-mingw32-g++ "dist/windows/$NAME.exe"   "-lshell32 -Wl,--stack,33554432"    -static
build "Linux x86_64" zig                    "dist/linux/$NAME"         ""                                  c++ -target x86_64-linux-musl
build "Linux ARM64"  zig                    "dist/linux-arm64/$NAME"   ""                                  c++ -target aarch64-linux-musl

exit $STATUS