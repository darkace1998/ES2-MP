#!/usr/bin/env bash
# Load a save game in a running instance: scripts/loadsave.sh <consoleport> [savename]
PORT="${1:?port}"; SAVE="${2:-ES2__AUTO_2023.04.08-23.33.57}"
python3 "$(dirname "$0")/console.py" "$PORT" "call UserFunctionsLib LoadGame world $SAVE 0"
