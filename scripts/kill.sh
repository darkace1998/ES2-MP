#!/usr/bin/env bash
# Kill all ES2 game instances started locally (matches the wine process command line, not this shell).
for p in $(pgrep -f 'Z:.*ES2-Win64-Shipping\.exe' ; pgrep -f 'Z:.*Everspace2\.exe'); do
  [[ "$p" == "$$" ]] && continue
  kill "$p" 2>/dev/null && echo "killed pid $p"
done
sleep 1
for p in $(pgrep -f 'Z:.*ES2-Win64-Shipping\.exe'); do kill -9 "$p" 2>/dev/null && echo "force-killed $p"; done
echo "done"
