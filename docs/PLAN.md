# EVERSPACE 2 Co-op Mod — Plan

_Last updated: 2026-08-19_

## 0. Ground truth (verified locally)

| Item | Value |
|---|---|
| Game | EVERSPACE™ 2, Steam appid 1128920, build 18856734 |
| Engine | **Unreal Engine 5.5.4** (custom Rockfish branch "ES2WinSteamShippingCE") |
| Install | `~/.local/share/Steam/steamapps/common/EVERSPACE™ 2/` |
| Exe | `ES2/Binaries/Win64/ES2-Win64-Shipping.exe` (169 MB, x64, imagebase 0x140000000) |
| **PDB** | `ES2/Binaries/Win64/ES2-Win64-Shipping.pdb` — **1.8 GB, full, unstripped** (types + globals + publics). Every function RVA and every class layout is available. |
| Runtime | Proton Experimental (Wine 11), prefix `steamapps/compatdata/1128920/pfx` |
| Host HW | 8 cores, 15 GB RAM, Intel Iris Xe iGPU, KDE Plasma/Wayland (XWayland available) |
| Netcode in binary | `UWorld::Listen`, `UWorld::ServerTravel`, `UIpNetDriver`, `UPendingNetGame`, `OnlineSubsystemSteam`, `APlayerController::ServerExecRPC`/`ClientMessage`, `UGameInstance::EnableListenServer`, `UClass::SetUpRuntimeReplicationData` — all present |
| Logging | UE_LOG mostly compiled out (NO_LOGGING); only a few custom file logs. The mod brings its own logger. |
| Game classes | `AESPawn` (ship pawn, player + NPC), `AESPlayerController`, `AESGameModeBase` (+`SpawnDefaultPawnAtTransform_Implementation`), `UESGameInstance`, `UPlayerData`, `UShipMovementComponent`, `UWeaponComponent`, `AWeaponBase`, `AProjectileBase`, `UHealthComponent`, `UGameplayLib` (SpawnPlayerShip/SpawnNPCPawn/GetESPlayerPawn…), `UImGuiDevSubsystem`, `UESCheats` |
| No network code in game classes | no `Server*/Client*` RPCs in ES2 classes — pure single-player design |

## 1. Architecture decision

**Native-UE-replication hijack with surgical hooks** (host-authoritative, star topology; host = listen server in-place).

Why not a fully custom "ghost" netcode: ES2 is combat-heavy; for real co-op the enemies/projectiles/loot must be
host-authoritative and mirrored on clients — which is exactly what UE's actor replication does for free once
`bReplicates`/`bReplicateMovement` are set, and `APawn` defaults to `bReplicates=true`. The PDB lets us call every
engine internal directly, so the mod is a thin C++ DLL that:

1. turns the host's running world into a listen server (`UGameInstance::EnableListenServer` / `UWorld::Listen`),
2. lets clients `open host:7777` (standard `UPendingNetGame` → `LoadMap` → `Login/PostLogin/RestartPlayer`),
3. hooks the handful of places where single-player assumptions break (GameMode-on-client, spawners on clients,
   player-ship spawn for remote players, origin shifting, HUD singletons),
4. uses `APlayerController::ServerExecRPC` (client→server, reliable) and `ClientMessage` (server→client, reliable),
   intercepted by hooks, as a free bidirectional string channel for mod control messages,
5. adds an own lightweight UDP channel later for high-rate ship transforms if the RPC channel proves insufficient,
6. replicates extra gameplay properties by flipping `CPF_Net` on selected `FProperty`s on both sides and calling
   `UClass::SetUpRuntimeReplicationData()`.

Loader: **`dwmapi.dll` proxy** (game imports 4 functions) placed next to the exe; under Proton needs
`WINEDLLOVERRIDES="dwmapi=n,b"`. Built with **zig c++ (x86_64-windows-gnu)** from Linux — no root needed.
Hooking: MinHook (vendored). Addresses: generated from the PDB (`sdk/symbols.tsv`, `tools/pdb_types.py`).

Debugging: the mod opens a **TCP debug console on 127.0.0.1:27100+N** (exec console commands, list/dump
objects, call functions, net status) and writes `Saved/ES2Coop/es2coop-<pid>.log`. Local testing uses two
instances in the same Proton prefix: host with `-nullrhi -nosound` (cheap, headless) + rendered client, or
two rendered 720p instances.

## 2. Phases

| Phase | Deliverable | Verification |
|---|---|---|
| **P0 Toolchain & loader** ✅ | zig build, `dwmapi.dll` proxy, launch scripts (Steam & direct Proton), logger, PDB tools, skills | DLL loads in game, log written, game still runs |
| **P1 Engine access layer** ✅ | GUObjectArray/FNamePool access, UObject iteration, FName↔string, class/property/function lookup, ProcessEvent, console-command exec, TCP debug console, MinHook hooks | from console: `objects AESPawn`, `exec stat fps`, `props <obj>` work |
| **P2 Networking bring-up** ✅ | host `listen`, client `connect ip`, both load same level, server sees 2 PlayerControllers, client gets a pawn; GameMode-on-client shim; survive a few minutes | two local instances, net status in console, no crash |
| **P3 Player ship sync (2P)** 🟡 (channel live; interp+loadout pending) | each side sees the other's ship moving; remote ship spawned via `SpawnDefaultPawnAtTransform`/`SpawnPlayerShip`; transform sync client→host via RPC channel, host→clients via replicated movement; interpolation | fly around each other in one location |
| **P4 Combat sync** | host-authoritative NPCs replicated to clients, client-side spawners/AI suppressed, damage from clients routed to host, health/shield replicated, projectiles visual | kill the same enemies together |
| **P5 World state** | location jumps (`ServerTravel` + clients follow), docking/stations, missions/dialog gating, loot, save handling (each player keeps own ship/loadout; world progress = host) | play a mission together |
| **P6 4 players & polish** | star topology already N-player; per-player channel/ids, join-in-progress, UI (join/host menu via ImGui or console), Steam P2P/NAT (OnlineSubsystemSteam `steam.<id>` URLs) | 4 local/LAN instances |

Primary target: **2-player**; all data structures are per-player arrays (max 4) from the start.

## 3. Key risks & mitigations

- **Game code assumes `GetGameMode()` non-null on client** → create a local AESGameModeBase on the client too
  (`UGameInstance::CreateGameModeForURL` + set `UWorld::AuthorityGameMode`), then suppress host-only logic via hooks.
- **Client-side level spawners/AI** create their own NPCs → hook `UWorld::SpawnActor` on clients: reject non-`bRemoteOwned`
  `AESPawn`/`AProjectileBase` spawns except for the local player ship.
- **World origin shifting** (`CheckForWorldOriginShifting`) → disable in MP (`UESGameInstance::SetWorldOriginShifting(false)`).
- **Blueprint CDOs may set `bReplicates=false`** → force on at spawn (`AActor::SetReplicates(true)` + `SetReplicateMovement`).
- **Hardware** (iGPU, 15 GB) → headless `-nullrhi` host for local tests; real 2-PC test by the user.
- **Steam single-instance / SteamAPI** → launch the second instance directly through the Proton script; verify.
- **Engine updates** change addresses → everything is regenerated from the PDB by scripts (`scripts/gen-sdk.sh`).

## 4. Repository layout

```
docs/PLAN.md            this file; docs/NOTES.md running findings
tools/pdb_publics.py    PDB publics → sdk/symbols.tsv (rva, mangled, demangled)
tools/pdb_types.py      TPI dump indexer + class layout / enum / vtable / header generator
sdk/symbols.tsv         1.1M symbols; sdk/uclasses.txt; sdk/es2_functions.txt (game module functions)
sdk/gen/*.h             generated RVAs + offsets used by the mod
mod/src/                the DLL (proxy, core, ue layer, hooks, net, console)
mod/third_party/minhook
scripts/                build.sh, deploy.sh, launch-*.sh, console.py, gen-sdk.sh
.claude/skills/         es2-build, es2-launch, es2-symbols, es2-console, es2-logs
```
