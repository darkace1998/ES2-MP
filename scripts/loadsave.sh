#!/usr/bin/env bash
# Load a save game in a running instance: scripts/loadsave.sh <consoleport> [savename]
source "$(dirname "$0")/proton-env.sh"
# No hardcoded default: ES2 rotates autosaves, so an old name eventually vanishes and the game
# asserts on load. Fall back to the newest autosave present.
PORT="${1:?port}"; SAVE="${2:-$(ls -t "$ES2_SAVED/SaveGames"/ES2__AUTO_*.sav 2>/dev/null | grep -v PREVIEW | head -1 | xargs -r basename | sed "s/\.sav$//")}"
[[ -z "$SAVE" ]] && { echo "no save found in $ES2_SAVED/SaveGames"; exit 1; }
python3 "$(dirname "$0")/console.py" "$PORT" "call UserFunctionsLib LoadGame world $SAVE 0"
