# Everspace 2 — Co-op Multiplayer Mod (work in progress)

A co-op multiplayer mod for **Everspace 2** (UE 5.5.4, Steam appid 1128920), built and debugged
on Linux/Proton. Host-authoritative listen server using the engine's native actor replication,
delivered as a `dwmapi.dll` proxy compiled from Linux with zig. Focus is **2-player**, architected for **4**.

## Status
2 players in one shared location: host + client, both ships spawned server-side and mutually
visible; host's NPCs/turrets replicate to the client; a client→host ship-transform channel runs at 20 Hz.
See `docs/PLAN.md` (phases) and `docs/NOTES.md` (findings).

## Quick start
```
scripts/build.sh          # build mod/build/dwmapi.dll (zig)
scripts/deploy.sh         # copy it into the game
scripts/coop-session.sh --kill   # launch host(27100)+client(27101), load save, listen, connect
python3 scripts/console.py 27100 status    # talk to the host
python3 scripts/console.py 27101 status    # talk to the client
scripts/screenshot.sh test # capture both windows -> run/shots/
scripts/kill.sh           # stop all game instances
```
Requires: the game installed via Steam, Proton Experimental, `zig` at `~/.local/opt/zig/zig`,
and llvm tools (`llvm-pdbutil`, `llvm-undname`). The game's PDB (shipped next to the exe) drives
all addresses.

## Layout
- `mod/src/` — the DLL (proxy, ue access layer, hooks, console, net, coop core)
- `mod/third_party/minhook` — vendored hooking lib
- `sdk/` — symbols/offsets extracted from the PDB; `sdk/gen/*.h` compiled into the mod
- `tools/` — PDB extractors (`pdb_publics.py`, `pdb_types.py`, `gen_sdk.py`, `disasm.py`, `symbolize_dll.py`)
- `scripts/` — build/deploy/launch/console/crash helpers
- `.claude/skills/` — seven skills (es2-build, es2-launch, es2-console, es2-coop-test, es2-symbols, es2-debug, es2-sdkgen)

## Releases
Prebuilt `dwmapi.dll` under [Releases](https://github.com/darkace1998/ES2-MP/releases); install steps in
[`docs/INSTALL.md`](docs/INSTALL.md). Each build is pinned to one game build — the mod compares the exe's
PE timestamp with the one baked into `sdk/gen/rvas.h` and disables itself if they differ, so after a game
update it stays inert until the SDK headers are regenerated and it is rebuilt.

## Changelog
See [CHANGELOG.md](CHANGELOG.md).

## License
[GPL-3.0](LICENSE). Note this covers the mod's own source; the symbol names, RVAs and struct offsets
under `sdk/` and the analysis in `docs/research/` are derived from the PDB Rockfish ships with the game
and remain their intellectual property.

## Not affiliated with Rockfish Games. For personal/educational use with your own copy of the game.
