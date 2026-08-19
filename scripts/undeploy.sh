#!/usr/bin/env bash
# Remove the mod DLL from the game folder (restores vanilla).
source "$(dirname "$0")/proton-env.sh"
rm -f "$ES2_BIN/dwmapi.dll" "$ES2_BIN/dwmapi.pdb" && echo "removed dwmapi.dll from game folder"
