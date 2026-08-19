#!/usr/bin/env bash
# Copy the built mod DLL into the game's Win64 folder (as dwmapi.dll proxy).
set -euo pipefail
source "$(dirname "$0")/proton-env.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# copy to a temp name then mv: running instances keep their old (mapped) inode intact
cp -f "$ROOT/mod/build/dwmapi.dll" "$ES2_BIN/dwmapi.dll.new" && mv -f "$ES2_BIN/dwmapi.dll.new" "$ES2_BIN/dwmapi.dll"
[[ -f "$ROOT/mod/build/dwmapi.pdb" ]] && cp -f "$ROOT/mod/build/dwmapi.pdb" "$ES2_BIN/dwmapi.pdb.new" && mv -f "$ES2_BIN/dwmapi.pdb.new" "$ES2_BIN/dwmapi.pdb" || true
mkdir -p "$ES2_BIN/ES2Coop/logs"
echo "deployed -> $ES2_BIN/dwmapi.dll ($(stat -c %s "$ES2_BIN/dwmapi.dll") bytes)"
