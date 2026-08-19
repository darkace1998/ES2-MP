#!/usr/bin/env bash
# Build the ES2Coop mod DLL (dwmapi.dll proxy) with zig for x86_64-windows-gnu.
# Usage: scripts/build.sh [debug|release]   (default release)   -> mod/build/dwmapi.dll
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZIG="${ZIG:-$HOME/.local/opt/zig/zig}"
MODE="${1:-release}"
OUT="$ROOT/mod/build"
mkdir -p "$OUT"
CXXFLAGS=(-std=c++20 -target x86_64-windows-gnu -shared -fno-rtti -Wall -Wno-unused-function -Wno-unused-variable -Wno-missing-braces
          -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS
          -I"$ROOT/mod/third_party/minhook/include")
if [[ "$MODE" == "debug" ]]; then CXXFLAGS+=(-O0 -g); else CXXFLAGS+=(-O2 -g); fi
SRCS=("$ROOT"/mod/src/*.cpp)
MH=("$ROOT"/mod/third_party/minhook/src/hook.c "$ROOT"/mod/third_party/minhook/src/buffer.c "$ROOT"/mod/third_party/minhook/src/trampoline.c "$ROOT"/mod/third_party/minhook/src/hde/hde64.c)
# compile C files separately (zig c++ would treat them as C++)
OBJS=()
for c in "${MH[@]}"; do
  o="$OUT/$(basename "${c%.c}").o"
  "$ZIG" cc -target x86_64-windows-gnu -O2 -c "$c" -o "$o" -I"$ROOT/mod/third_party/minhook/include" -I"$ROOT/mod/third_party/minhook/src"
  OBJS+=("$o")
done
rm -f "$OUT/dwmapi.dll"
set +e
"$ZIG" c++ "${CXXFLAGS[@]}" -o "$OUT/dwmapi.dll" "${SRCS[@]}" "${OBJS[@]}" -lws2_32 -luser32 -lkernel32 2> "$OUT/build.log"
rc=$?
set -e
grep -E 'error|warning: (unused|format)' "$OUT/build.log" | grep -vE 'libcxx|_LIBCPP|nullability' | head -40 || true
if [[ $rc -ne 0 || ! -f "$OUT/dwmapi.dll" ]]; then echo "BUILD FAILED (see $OUT/build.log)"; exit 1; fi
ls -la "$OUT/dwmapi.dll"
echo "exports:"; llvm-readobj --coff-exports "$OUT/dwmapi.dll" | grep -E '^\s+Name:' | tr -s ' ' | tr '\n' ' '; echo
