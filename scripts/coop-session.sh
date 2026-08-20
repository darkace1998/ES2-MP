#!/usr/bin/env bash
# Bring up a local 2-instance co-op test session:
#   host (p1, console 27100): launch -> load save -> listen 7777
#   client (p2, console 27101): launch -> load save -> connect 127.0.0.1:7777
# Usage: scripts/coop-session.sh [--host-only] [--no-connect] [--nullrhi-host] [--save NAME] [--kill]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SAVE=""; HOST_ONLY=0; CONNECT=1; NULLRHI_HOST=""; KILL=0; STEAM=0
while [[ $# -gt 0 ]]; do case "$1" in
  --host-only) HOST_ONLY=1;; --no-connect) CONNECT=0;; --nullrhi-host) NULLRHI_HOST="--nullrhi";; --save) SAVE="$2"; shift;;
  --steam) STEAM=1;; --kill) KILL=1;; *) echo "unknown arg $1"; exit 1;; esac; shift; done
source "$ROOT/scripts/proton-env.sh"
SAVEDIR="$ES2_SAVED/SaveGames"
# ES2 rotates its autosaves, so a hardcoded save name eventually gets deleted and the game asserts
# ("Savegame load failed ... may be missing or corrupt"). Default to the newest autosave present,
# and read the location out of its PREVIEW sidecar so we wait for the right world.
if [[ -z "$SAVE" ]]; then
  SAVE=$(ls -t "$SAVEDIR"/ES2__AUTO_*.sav 2>/dev/null | grep -v PREVIEW | head -1 | xargs -r basename | sed 's/\.sav$//')
  [[ -z "$SAVE" ]] && { echo "no ES2__AUTO_* save in $SAVEDIR (restore one from run/backup/SaveGames)"; exit 1; }
fi
if [[ ! -f "$SAVEDIR/$SAVE.sav" ]]; then
  # Rotation may have eaten a pinned save; restore it from the local backup if we kept one.
  if [[ -f "$ROOT/run/backup/SaveGames/$SAVE.sav" ]]; then
    cp -n "$ROOT/run/backup/SaveGames/$SAVE.sav" "$SAVEDIR/"
    cp -n "$ROOT/run/backup/SaveGames/${SAVE/ES2__/ES2__PREVIEW__}.sav" "$SAVEDIR/" 2>/dev/null
    echo "### restored '$SAVE' from run/backup/SaveGames"
  else
    echo "save '$SAVE' not found in $SAVEDIR (and no copy in run/backup/SaveGames)"; exit 1
  fi
fi
save_world() { # save name -> location id from the PREVIEW sidecar (e.g. S01ML01)
  # The sidecar serialises as key / type / value on consecutive lines:
  #   CurrentLocation \n NameProperty \n S01ML01
  local pv="$SAVEDIR/${1/ES2__/ES2__PREVIEW__}.sav"
  [[ -f "$pv" ]] || return 1
  strings -n 3 "$pv" 2>/dev/null | grep -A3 -x 'CurrentLocation' | grep -m1 -E '^S[0-9]+[A-Z]*L[0-9]+$'
}
WORLD=$(save_world "$SAVE") || true
[[ -z "$WORLD" ]] && { echo "could not read location from $SAVE preview; assuming S01ML01"; WORLD=S01ML01; }
echo "### save '$SAVE' -> world $WORLD"
con() { python3 "$ROOT/scripts/console.py" "$@"; }
wait_world() { # port world timeout
  local port=$1 want=$2 t=${3:-180} i=0
  while (( i < t )); do
    w=$(con "$port" status 2>/dev/null | sed -n 's/^world=\([^ ]*\).*/\1/p')
    [[ "$w" == "$want" ]] && return 0
    sleep 3; i=$((i+3))
  done
  echo "timeout waiting for world=$want on port $port (last: '$w')"; return 1
}
wait_console() { local port=$1 t=${2:-180} i=0; while (( i < t )); do con "$port" ping >/dev/null 2>&1 && return 0; sleep 3; i=$((i+3)); done; echo "console $port not up"; return 1; }
[[ $KILL == 1 ]] && { scripts/kill.sh; sleep 3; }

echo "### host: launch"; scripts/launch.sh host --port 27100 --res 1280x720 $NULLRHI_HOST
wait_console 27100 240 || exit 1
# the game reaches EntryMap/MainMenu; LoadGame works from either
sleep 20
echo "### host: load save $SAVE"; con 27100 "call UserFunctionsLib LoadGame world $SAVE 0"
wait_world 27100 "$WORLD" 240 || exit 1; sleep 8
if [[ $STEAM == 1 ]]; then
  # Steam transport must be selected BEFORE the first listen: EnableListenServer only creates a net
  # driver when the world has none, so a later switch is silently ignored.
  echo "### host: Steam P2P transport"; con 27100 "netdriver steam" | tail -1; con 27100 "steam host 1" >/dev/null
fi
echo "### host: listen"; con 27100 listen 7777 | tail -1
[[ $STEAM == 1 ]] && con 27100 steam | sed -n '7,12p' 
[[ $HOST_ONLY == 1 ]] && exit 0

echo "### client: launch"; scripts/launch.sh client --port 27101 --res 1280x720
wait_console 27101 240 || exit 1
sleep 20
echo "### client: load save $SAVE"; con 27101 "call UserFunctionsLib LoadGame world $SAVE 0"
wait_world 27101 "$WORLD" 240 || exit 1; sleep 8
if [[ $CONNECT == 1 ]]; then
  echo "### client: connect"; con 27101 connect 127.0.0.1:7777 | tail -1
  sleep 20
  echo "### client status"; con 27101 status | head -7
  echo "### host net"; con 27100 net | head -12
fi
echo "### done. host console 27100, client console 27101"
