#!/usr/bin/env bash
# Bring up a local 2-instance co-op test session:
#   host (p1, console 27100): launch -> load save -> listen 7777
#   client (p2, console 27101): launch -> load save -> connect 127.0.0.1:7777
# Usage: scripts/coop-session.sh [--host-only] [--no-connect] [--nullrhi-host] [--save NAME] [--kill]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SAVE="ES2__AUTO_2023.04.08-23.33.57"; HOST_ONLY=0; CONNECT=1; NULLRHI_HOST=""; KILL=0
while [[ $# -gt 0 ]]; do case "$1" in
  --host-only) HOST_ONLY=1;; --no-connect) CONNECT=0;; --nullrhi-host) NULLRHI_HOST="--nullrhi";; --save) SAVE="$2"; shift;; --kill) KILL=1;; *) echo "unknown arg $1"; exit 1;; esac; shift; done
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
wait_world 27100 S01ML01 240 || exit 1; sleep 8
echo "### host: listen"; con 27100 listen 7777 | tail -1
[[ $HOST_ONLY == 1 ]] && exit 0

echo "### client: launch"; scripts/launch.sh client --port 27101 --res 1280x720
wait_console 27101 240 || exit 1
sleep 20
echo "### client: load save $SAVE"; con 27101 "call UserFunctionsLib LoadGame world $SAVE 0"
wait_world 27101 S01ML01 240 || exit 1; sleep 8
if [[ $CONNECT == 1 ]]; then
  echo "### client: connect"; con 27101 connect 127.0.0.1:7777 | tail -1
  sleep 20
  echo "### client status"; con 27101 status | head -7
  echo "### host net"; con 27100 net | head -12
fi
echo "### done. host console 27100, client console 27101"
