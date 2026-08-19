#!/usr/bin/env bash
# Launch an ES2 instance directly through Proton with the mod enabled.
# Usage: scripts/launch.sh <name> [--nullrhi] [--port N] [--res WxH] [-- extra game args]
#   name: free label (host, client, p1, ...) used for the console port file & log naming
# Examples:
#   scripts/launch.sh host --nullrhi --port 27100
#   scripts/launch.sh client --port 27101 --res 1280x720
set -euo pipefail
source "$(dirname "$0")/proton-env.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:?name}"; shift || true
PORT=27100; NULLRHI=0; RES=""; EXTRA=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --nullrhi) NULLRHI=1; shift;;
    --port) PORT="$2"; shift 2;;
    --res) RES="$2"; shift 2;;
    --) shift; EXTRA=("$@"); break;;
    *) EXTRA+=("$1"); shift;;
  esac
done
ARGS=(-windowed -NoSplash -NoVerifyGC -dx11)   # -dx11: DXVK is much lighter than vkd3d-proton (DX12) on this machine
if [[ $NULLRHI == 1 ]]; then ARGS+=(-nullrhi -nosound -unattended); fi
if [[ -n "$RES" ]]; then ARGS+=(-ResX="${RES%x*}" -ResY="${RES#*x}"); fi
ARGS+=("${EXTRA[@]}")
mkdir -p "$ROOT/run"
export WINEDLLOVERRIDES="dwmapi=n,b"
export ES2COOP_CONSOLE_PORT="$PORT"
export ES2COOP_INSTANCE="$NAME"
export PROTON_LOG="${PROTON_LOG:-0}"
# Separate Saved/ dir per instance would be ideal; UE5 -UserDir is not honored by shipping games reliably, so we share the prefix.
LOG="$ROOT/run/$NAME.proton.log"
echo "launching '$NAME' console-port=$PORT args: ${ARGS[*]}"
nohup "$PROTON_DIR/proton" run "$ES2_EXE" "${ARGS[@]}" > "$LOG" 2>&1 &
echo $! > "$ROOT/run/$NAME.pid"; echo "$PORT" > "$ROOT/run/$NAME.port"
echo "pid $(cat "$ROOT/run/$NAME.pid") (proton wrapper); stdout/err -> $LOG"
