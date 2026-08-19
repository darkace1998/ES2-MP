#!/usr/bin/env bash
# Screenshot all "Everspace 2" X windows -> run/shots/<name>-<n>.png   Usage: scripts/screenshot.sh [name]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; mkdir -p "$ROOT/run/shots"; NAME="${1:-shot}"; i=0
for w in $(DISPLAY=:0 xprop -root _NET_CLIENT_LIST 2>/dev/null | grep -oE '0x[0-9a-f]+'); do
  t=$(DISPLAY=:0 xprop -id $w _NET_WM_NAME 2>/dev/null | grep -oE '"[^"]*"')
  if [[ "$t" == *Everspace* ]]; then i=$((i+1)); f="$ROOT/run/shots/$NAME-$i.png"; DISPLAY=:0 magick import -window $w "$f" && echo "$f ($w)"; fi
done
[[ $i == 0 ]] && echo "no Everspace window found"
