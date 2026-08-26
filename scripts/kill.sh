#!/usr/bin/env bash
# Kill all ES2 game instances started locally (game processes only, never this shell).
#
# The launcher invokes the exe by its Linux path (/home/darkace/es2game/...), while Proton re-execs it
# with a Z:\ path. Matching only 'Z:.*ES2-Win64-Shipping.exe' therefore missed the proton wrapper and
# any instance launched by path, leaving stragglers that keep holding console ports 27100+ — the next
# session then binds a different port and the harness talks to a stale instance.
PAT='(ES2-Win64-Shipping|Everspace2)\.exe'
me=$$
mine() { # true if pid is this script, its parent, another copy of this script, or tooling that merely names the exe
  [[ "$1" == "$me" || "$1" == "$PPID" ]] && return 0
  local cmd; cmd=$(tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null)
  [[ "$cmd" == *kill.sh* ]] && return 0
  # pgrep -f matches the whole command line: a concurrent `llvm-objdump ... ES2-Win64-Shipping.exe`
  # (tools/disasm.py) or a python harness quoting the path is not a game instance.
  [[ "$cmd" =~ ^([^ ]*/)?(python[0-9.]*|llvm-[A-Za-z0-9-]+|grep|sed|bash|sh)\  ]]
}
for sig in TERM KILL; do
  n=0
  for p in $(pgrep -f "$PAT" 2>/dev/null); do
    mine "$p" && continue
    kill "-$sig" "$p" 2>/dev/null && { echo "${sig}ed pid $p"; n=$((n+1)); }
  done
  (( n == 0 )) && break
  sleep 2
done
left=0
for p in $(pgrep -f "$PAT" 2>/dev/null); do mine "$p" || left=$((left+1)); done
echo "done (${left} still alive)"
