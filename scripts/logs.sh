#!/usr/bin/env bash
# Show mod logs. Usage: scripts/logs.sh [-f] [N lines]   (latest log by mtime; -f to follow)
source "$(dirname "$0")/proton-env.sh"
D="$ES2_BIN/ES2Coop/logs"
F=$(ls -t "$D"/es2coop-*.log 2>/dev/null | head -1)
[[ -z "$F" ]] && { echo "no logs in $D"; exit 1; }
echo "== $F"
if [[ "${1:-}" == "-f" ]]; then tail -n "${2:-50}" -f "$F"; else tail -n "${1:-80}" "$F"; fi
