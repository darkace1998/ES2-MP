---
name: es2-console
description: Drive a running Everspace 2 instance over its in-game TCP debug console — inspect objects/actors/properties, call UFunctions, exec UE console commands, and use the co-op/net commands. Use to observe or poke live game state while testing. Triggers: "check status in game", "list actors", "dump properties", "call a function", "run a console command", "net status", "coop status".
---

# ES2 in-game console

Each modded instance runs a line-based TCP console. Talk to it with:
`python3 scripts/console.py <port|name> <command...>`   (or `-i` interactive, `-f file`)
Ports: host 27100, client 27101 (or the name from `run/<name>.port`).

## Core commands (run `help` for the live list)
- `status` — world, netmode, gamemode, local PC + pawn (+loc/role), levels.
- `net` — net driver, client connections (addr/loginstate), all PlayerControllers.
- `coop [hz N|verbose 0/1|sweep|say <text>|nopause]` — co-op role + tx/rx of the ship channel.
- `objects <Class> [max] [filter]` / `actors <Class> [max]` — live UObjects / world actors (with loc/role/rep).
- `props <obj|0xADDR|world|pc|pawn|gm|gi|engine|netdriver> [filter]` — dump property values.
- `class <Class>` / `cdo <Class> [filter]` / `func <Class> <Fn>` / `subclasses <Class>` — reflection.
- `set <obj> <prop> <value>` — set a scalar/enum/name/vector property.
- `call <obj> <Function> [args...]` — ProcessEvent a UFunction (object args by 0xADDR or keyword; e.g. `call UserFunctionsLib LoadGame world <SaveName> 0`).
- `exec <UE console command>` — via UKismetSystemLibrary::ExecuteConsoleCommand (e.g. `exec stat fps`, `exec open 127.0.0.1:7777`).
- `tp <x> <y> <z>` | `tp host` — teleport the local pawn (client `tp host` jumps next to a player ship).
- `find <substr>` , `load <path>` , `hooks` , `ping`.

## Notes
- Object args accept `0xADDR` (validated live), a path name, or keywords `world/pc/pawn/gm/gi/engine/netdriver`.
- Commands that touch the world run on the **game thread** (pumped from the UGameEngine::Tick hook); if a command times out the game is on a loading screen / not ticking.
- Loading a save headlessly: `call UserFunctionsLib LoadGame world <SaveName> 0` (or `scripts/loadsave.sh <port> [name]`).
