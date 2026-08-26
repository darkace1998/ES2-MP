# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A co-op multiplayer mod for Everspace 2 (UE 5.5.4, single-player, Steam appid 1128920), developed on
Linux/Proton. It is a Windows x64 `dwmapi.dll` proxy cross-compiled from Linux with zig, loaded into
`ES2-Win64-Shipping.exe`, that inline-hooks engine/game functions at addresses taken from the game's
shipped full PDB. Design: host-authoritative listen server using UE's native actor replication; the mod
only patches the places where the game's single-player assumptions break. 2 players work end to end;
structures are sized for 4. Not done (see `docs/NOTES.md` "Remaining"): a live 3–4 player run, a
completed Steam join with a second account, docking/stations/mission item rewards (XP-on-kill itself
was observed and is verified by `verify.py`).

`docs/NOTES.md` is the authoritative status/findings log (its "Hard-won gotchas" list is the crash
history); `docs/PLAN.md` the phase plan; `docs/INSTALL.md` the end-user flow. There is no unit test,
lint, CI or Makefile — the only regression signal is the live two-instance harness below. The project
skills under `.claude/skills/es2-*` (build, launch, console, coop-test, debug, symbols, sdkgen) hold the
detailed runbooks; invoke them rather than re-deriving steps.

## Commands

Game-facing scripts source `scripts/proton-env.sh` (paths, Proton env, the `~/es2game` ASCII symlink —
the real install path contains `™`, which breaks `proton run`). Requirements: zig at
`~/.local/opt/zig/zig` (override with `ZIG=`), `llvm-pdbutil`, `llvm-undname`, `llvm-objdump`,
`llvm-readobj`, Proton Experimental; `screenshot.sh` also needs `xprop` + ImageMagick on DISPLAY=:0.

### Build → deploy → restart
```
scripts/build.sh [debug]          # zig c++ -target x86_64-windows-gnu -> mod/build/dwmapi.dll (+.pdb); full log in mod/build/build.log
scripts/deploy.sh                 # atomic copy into the game's Win64 folder; running instances keep the OLD dll
scripts/kill.sh                   # stop every game instance — required after deploy; nothing hot-reloads
scripts/undeploy.sh               # back to vanilla
```
`build.sh` globs `mod/src/*.cpp` (no source list to maintain), shows only `error` lines and
`unused`/`format` warnings (other warnings are in `build.log`), and ends by printing the four exported
`Dwm*` names.

### Run (skill: es2-launch)
```
scripts/launch.sh <name> [--port N] [--res WxH] [--nullrhi] [-- extra args]   # writes run/<name>.{pid,port,proton.log}
scripts/coop-session.sh --kill [--steam] [--save NAME] [--host-only] [--no-connect] [--nullrhi-host]
                                  # host (console 27100) + client (27101): load save, listen 7777, connect  (~3 min)
scripts/loadsave.sh <port> [save] # LoadGame headlessly in a running instance
scripts/screenshot.sh <name>      # every ES2 window -> run/shots/
```
Only two instances fit in RAM (~3 GB each) — never launch a third. `--nullrhi` hosts crash on the first
weapon shot (ES2's own null deref in `PlayWeaponFireFX`), so don't use them for anything that fires.

### Test (skill: es2-coop-test)
```
python3 scripts/verify.py [--travel]             # end-to-end checks against the live pair; exit 1 on any FAIL (~2 min, +travel ~4)
python3 scripts/combat-test.py [secs] [--route 0|1] [--ship]   # client fire -> host-side damage, vs a no-fire baseline
python3 scripts/loadout-test.py [from] [to]      # ship provenance: client rewrites its blob, host must show it
python3 scripts/weapon-swap-test.py [swaps]      # equipped-slot / reticle agreement across NextWeapon
```
Harness rules learned the hard way:
- Tests shell out to `console.py` and regex-parse text. Player names can contain spaces and brackets
  (Steam personas) — never match the name column with `\S+`.
- Checks that need a nearby enemy degrade to `INFO` rather than `FAIL`; a green run does not prove those paths ran.
- Run `god 1` on both ports before any combat measurement — an idle host parked near enemies dies and voids the run.
- Wait for the loadout swap (`shipdata stash` shows `applied=1`) before sampling anything on the client;
  applying the blob REPLACES the client pawn, so earlier samples belong to a retired actor.
- Never judge sync by comparing a host reading with a client reading from two console calls — measure
  per frame on one machine (`jitter <guid> [sec]` on the client) over repeated runs.
- Save names are never hardcoded (ES2 rotates autosaves); `coop-session.sh` picks the newest `ES2__AUTO_*`
  and a pinned copy lives in `run/backup/SaveGames/`.

### Observe / debug (skills: es2-console, es2-debug)
```
python3 scripts/console.py <port|name> <command...>   # TCP console; -i interactive, -f file; `help` lists commands
scripts/logs.sh [-f] [N]            # newest mod log: <game>/ES2/Binaries/Win64/ES2Coop/logs/es2coop-<pid>.log
python3 scripts/logof.py <port>     # which log belongs to which instance
scripts/crash.sh [N]                # symbolized N-th newest UE crash (ES2 frames via sdk/funcs.pkl, mod frames via mod/build/dwmapi.pdb)
python3 tools/symbolize_dll.py mod/build/dwmapi.pdb 0x<off>
python3 tools/dump_stackscan.py <crashdir>/UEMinidump.dmp    # real stack when crash.sh shows one ntdll frame
```
Each module registers a status command printing its counters/switches — `coop players net combat loot
respawn attribution menu hooks` by name, plus `shipdata` (loadout), `travelinfo` (travel — the `travel`
command is net.cpp's ServerTravel), `guard`/`spawnlog`/`uiwatch` (authority), `world`/`missions` (world_state),
`steam` (steamp2p). The tests assert on those counters. Log lines carry a bracket tag that is not always
the module name (`[coop] [net] [loadout] [travel] [world] [dmg] [steam] [menu] …`).

### Symbols and SDK headers (skills: es2-symbols, es2-sdkgen)
RVA lookup: `grep -F 'UWorld::Listen(' sdk/symbols.tsv | cut -f1,4`; layouts/enums/vtables via
`python3 tools/pdb_types.py layout|enum|vtable|find sdk/raw/types.txt <Name>`; annotated disassembly via
`python3 tools/disasm.py <name|0xRVA>`. To use a symbol in C++ add it to the `RVAS`/`OFFSETS`/`BITFIELDS`
dicts in `tools/gen_sdk.py` and regenerate (`python3 tools/gen_sdk.py sdk/symbols.tsv sdk/raw/types.txt sdk/gen`).
Never hand-edit `sdk/gen/*.h`. Committed under `sdk/`: `gen/*.h` plus three small indexes the es2-symbols
skill uses (`es2_functions.txt`, `es2_classes_by_funcs.txt`, `uclasses.txt`). Gitignored and rebuildable
via the es2-sdkgen recipe: `sdk/symbols.tsv`, `sdk/raw/`. Also gitignored: `sdk/funcs.pkl` (the sorted
`(rvas, names)` pickle read by `crash.sh` and `disasm.py`) — it has **no generator in the repo**; don't
delete it.

## Architecture

### Addresses and the build guard
PDB → (`tools/pdb_publics.py`, `tools/pdb_types.py index`) → `sdk/symbols.tsv` + types index →
`tools/gen_sdk.py` → `sdk/gen/rvas.h` + `offsets.h` → `scripts/build.sh` → `dwmapi.dll` → `deploy.sh`;
`launch.sh` sets `WINEDLLOVERRIDES=dwmapi=n,b` (what makes Proton load the proxy) and `ES2COOP_CONSOLE_PORT`.

Nothing in `mod/src` contains a literal address: functions are `es2rva::<Class_Method>` (the comment
next to each constant is the exact demangled MSVC signature — the source of truth for ABI questions),
fields are `es2off::<Class>::<Member>` (bitfields `<m>_off`/`<m>_mask`). `gen_sdk.py` reports an RVA
spec matching 0 or >1 symbols as ERROR (still writes the headers minus that identifier, so the build
breaks, and exits 1) and marks ICF-folded addresses (`!!! ICF-FOLDED`, `<ident>_IS_FOLDED`) — calling
those is fine, hooking them is not.

`ue::Init` compares the exe's PE `TimeDateStamp` with `es2rva::PE_TIMESTAMP` and disables the whole
mod on mismatch. That constant is a hand-maintained literal in `tools/gen_sdk.py`, so a game update
needs both a full regeneration and a bump of that literal.

### DLL lifecycle (`mod/src/main.cpp`, `proxy.cpp`)
`proxy.cpp` forwards the four `Dwm*` imports to the real system DLL; everything else starts in
`DllMain`, which only activates inside `ES2-Win64-Shipping.exe` and spawns `InitThread`. That thread:
`LogInit` → `ue::Init` (timestamp guard) → `hooks::Init` → every module's `Register()` → `console::Start`
→ wait for `GEngine` → hook `UGameEngine::Tick` → every module's `OnInit()`.

- `Register()` = console commands only (engine may not exist). `OnInit()` = `hooks::Install` +
  `console::RegisterTick` (one exception: `net::OnInit` rewrites `GEngine->NetDriverDefinitions` to
  IpNetDriver from there). Both run on the init thread, **not** the game thread.
- The game-thread entry points are engine hooks and the `UGameEngine::Tick` detour →
  `console::PumpGameThread`, which drains queued console commands and runs all `RegisterTick` callbacks.
- A new module needs both `X::Register()` and `X::OnInit()` added to the two ordered lists in `main.cpp`.

### Engine access layer (`ue.h` / `ue.cpp`)
Opaque `UObject`-family structs, accessed only through offsets. Patterns used everywhere:
- Call by RVA: `using Fn_X = Ret(*)(Args...); Rva<std::remove_pointer_t<Fn_X>>(es2rva::X)(...)`.
- Read a field: `UE_FIELD(T, obj, es2off::Class::Member)`; globals `*Rva<T*>(es2rva::GEngine)`.
- Virtuals: `VCall<Ret, Args...>(obj, ue::vt::Slot, ...)`; slots come from `pdb_types.py vtable`.
- Reflection for Blueprint-defined things: `FindClass`/`FindObject`/`GetProperties`/`FindFunction` +
  `ProcessEvent`, so HUD/ship BP variants keep working without new offsets.
- `ue::FString`/`ue::TArray` allocate via `FMemory` so the engine may free them; `FString` is move-only.
- Hook shape (no macro): `static Fn_X o_X; static Ret H_X(args) { …; return o_X(args); }` then
  `hooks::Install("Class::Fn", es2rva::Class_Fn, (void*)&H_X, (void**)&o_X)`. Install failures are
  logged, not fatal — check the `hooks` console command. Two modules may not hook the same RVA
  (MinHook refuses the second). Hooks that toggle at runtime are installed then `hooks::Enable(name,false)`.

### Module contract
Core: `ue, hooks, log, console, coop, players, net` (`net` owns listen/connect, the net-driver fixes,
the RPC hooks the message channel rides on, and the remote-pawn spawn wrapper). Feature modules
(`loadout, travel, authority, combat, world_state, loot, attribution, respawn, steamp2p, menu`) depend on
the core and — with one exception, `menu → steamp2p` for invites/presence — not on each other. The core
calls into them at fixed points:
- `main.cpp`: the `Register()` / `OnInit()` lists.
- `coop.cpp` tick (`RegisterTick("coop")`): host branch and client branch call each module's
  `Tick(dt, isHost)` / role-specific ticks.
- `coop.cpp` message dispatch: host side `combat → loadout → travel → world_state::OnServerOp(fromPC, op, body)`,
  client side `combat → loadout → travel → world_state → loot → respawn::OnClientOp(op, body)`.
  First `true` wins and unhandled ops are swallowed. `coop` consumes `T|HELLO|SAY` (host) and
  `WELCOME|PT|SAY|ROSTER` (client) before the chain — a new op name must not collide with any of these.
- `net.cpp` wraps `AESGameModeBase::SpawnDefaultPawnAtTransform` in `loadout::BeginSubstitution/EndSubstitution`;
  `coop.cpp` calls `combat::SetClientDamageBlock` on every role change and `travel::ApplyOriginShiftPolicy`
  whenever it enters Host/Client and on every world change while in MP.

`coop::CurrentRole()` is derived from the world's NetMode on every call (ListenServer/Dedicated = Host,
Client = Client, else None) — modules ask, never cache. `players` is a fixed 4-slot registry indexed by
player id (host = 0, clients get the lowest free slot); on a client it holds only the local slot. `coop`
resets the registry on every world change and on every role change, and on a world change also calls
`combat::OnWorldChanged` / `loadout::OnWorldChanged` to drop their actor-keyed caches (UObject
addresses are recycled, so a stale key is a wrong answer, not just dead weight); the loadout session
(`loadout::ResetSession`) is reset only on a role change.

`menu` splices a MULTIPLAYER toggle into ES2's own main menu; arming it makes the mod run `listen 7777`
itself once a real map is up, and drives Steam invites via `steamp2p` (user flow in `docs/INSTALL.md`).
The harness bypasses it with console `listen`/`connect`.

### Message channel (`coop.cpp`)
Two reliable engine RPCs that carry an `FString` are borrowed: client→host `APlayerController::ServerChangeName`,
host→client `APlayerController::ClientMessage`. `net.cpp` hooks their `_Implementation`s; a payload
starting with `$` is a mod message, anything else falls through to the real function. Framing is
`OP|field|field…` (the `$` is added by `coop::SendToServer / SendToClient / SendToAllClients`, never by
the caller). Handlers run inside the net receive path (`UActorChannel::ReceivedBunch`): never run a
Blueprint graph (effects, widgets, BP spawns) from a handler — queue and drain on a tick (native
UFunction setters such as `SetCurrentHitpointsWithRatio` are called inline today). The channel is shared
by loadout streaming, world state, travel, aim, NPC mirroring and damage numbers; everything on it is
edge-detected, rate-limited or coalesced.

### Authority model
- Host simulates everything. The one exception is each client's own ship: the client streams its transform
  at 20 Hz (`T|…`), the host dead-reckons the server pawn toward it and forces `ReplicateMovement=false`
  on remote pawns so the owner never rubberbands.
- A client never damages anything: fire intent (`F|…`) and aim/lock (`AIM|…`) go to the host, which
  presses the player's server-side trigger. On the client the simulation that would fight the host is
  suppressed (`ApplyESPointDamage/RadialDamage` no-op'd, shield regen skipped; UE itself suppresses
  client-side gameplay spawns — `authority` keeps a spawn census plus an opt-in `guard on` block as a
  safety net, off by default) and presentation is mirrored back from the host (NPC trigger/aim `WF|WA`,
  hitpoints `NH|HP`, deaths `ND`, damage numbers `DM`, loot `LOOT`, mission state `MT|MC|TRK`, dialog
  `DLG`), applied through ES2's own setters so delegates and HUD update.
- Cross-machine actor identity is the server-assigned `FNetworkGUID` — payloads carry guids, never
  pointers. `GetObjectFromNetGUID` is only trustworthy on a client; on the host use the round-trip-and-scan
  pattern of `combat.cpp`'s file-local `ActorFromNetGuid` (verify `GetNetGUID(result) == guid`, else scan
  actors) — move it to `ue.h` before a second module needs it. A guid of 0 from `NetGuidOf` means "not
  replicated yet" (skip sending); in aim/lock payloads guid 0 means "no target" and is applied as such.
- The loadout problem: ES2 fills a new pawn from the process-global `UPlayerData`, so every joiner would
  get the host's ship. The client exports its `FShipDataState` via `UScriptStruct::ExportText`, streams
  it (`SD|seq|total|chunk`), and the host substitutes it in `UInventoryLib::GetCurrentShip` only when
  called from the spawner (return-address filter + re-entrancy guard), then respawns the client's pawn
  (UnPossess → RestartPlayer → destroy old pawn, all in one frame).

### Console (`console.cpp`, `commands.cpp`)
Line-based TCP on `127.0.0.1:<port>`, replies end with `<<END>>`. With `ES2COOP_CONSOLE_PORT` set
(every `launch.sh` instance) the mod binds exactly that port or logs and disables its console — so
`run/<name>.port` is always right; only an unpinned launch scans 27100–27109.
`console::Register(name, help, handler, onGameThread=true)`; handlers append to `out`. Game-thread
commands block up to 30 s waiting for the tick — a timeout means the game is on a loading screen.
`commands.cpp` has the generic inspection set (`objects/actors/props/class/func/call/set/exec/find`, plus
`objtop` — live UObject count histogrammed by class, since `status objects=` is only the array high-water mark);
object args accept `0xADDR`, a path name, or `world|pc|pawn|gm|gi|engine|netdriver`.

## Rules that cost a crash or a silent failure (details in `docs/NOTES.md` "Hard-won gotchas")
- **MSVC x64 ABI**: struct returns use a hidden `sret` pointer when the type is large OR not trivially
  copyable (`FNetworkGUID` is 8 bytes and still sret) — member functions are `(this, sret, …)`, static
  ones `(sret, …)`. "By value" `FString` / `TSubclassOf<>` params are passed by hidden pointer — take
  them as `const void*` in a detour and forward, never as a by-value copy (double free). Verify at a real
  call site with `tools/disasm.py`.
- Pure virtuals (e.g. `UNetConnection::LowLevelGetRemoteAddress`) must go through `VCall`, never the base RVA.
- `ProcessEvent` with a null parms block only for UFunctions with `NumParms == 0` (check with
  `func <Class> <Fn>`); e.g. call `SpawnExplosion`, not `Die`, for NPC death FX.
- Hooking a function the engine re-enters (`CreateShipDataFromState → GetCurrentShip`) needs a
  re-entrancy guard; mirrored ops are replayed under a `g_applying` flag so the host-side hook doesn't
  re-broadcast, and by calling the saved trampoline `o_X` rather than the public function.
- Hot-path hooks (`UpdateTaskInPlayerData` runs every frame; NPC `StartFire/StopFire` thousands of times)
  must be allocation-free and send only on change.
- `AESPawn::ShipData` must be filled in `PreInitializeComponents`, before `InitializeComponents`, or
  devices/consumables come up empty. `RestartPlayerAtTransform` only spawns when the controller has no pawn.
- World-origin shifting must stay off in MP (`travel::ApplyOriginShiftPolicy`), or host/client absolute
  coordinates drift. ES2 jumps go through `UGameplayLib::ChangeLocation`, not `OpenLevel` directly.
- A client has no `AESGameModeBase`; any ES2 path that reaches it (the whole damage chain) crashes. Keep
  client damage blocked. `AESHUD::Tick` faults on a null local pawn (`authority` guards it, always on).
- Steam transport must be selected (`netdriver steam`, `steam host 1`) **before** the first `listen`;
  `EnableListenServer` only creates a net driver when the world has none.
- A world change invalidates every actor pointer; caches keyed by `UObject*` must revalidate with
  `ue::IsValidObject` **and** be cleared on the world change (`combat::OnWorldChanged` /
  `loadout::OnWorldChanged`) — UObject addresses are recycled, so a stale key returns a wrong answer.
- `ue::IsValidObject` means "a live, dereferenceable UObject", deliberately **not** "not pending-kill":
  callers rely on reading just-retired objects (the HUD rebind detects a stale child through a retired
  pawn's component; `ActorFromNetGuid` resolves a dying NPC so its death FX still play). Tightening it
  broke three verify checks. Use `ue::IsGarbage()` when you actually mean pending-kill.
- A UFunction's property list also contains its **local variables** (past `ParmsSize`) — filter on
  `CPF_Parm` (0x80) before writing arguments into a parms block, or you write past it.
- `windows.h` defines `GetClassName`; the mod's helper is `ue::GetObjectClassName`.
- Stale game processes keep console ports bound and make the harness talk to the wrong instance —
  always `coop-session.sh --kill` / `scripts/kill.sh`.

## Docs and research
`docs/research/*.md` are PDB-derived design reports, one per feature (01 loadout, 02 spawner
suppression, 03 combat, 04 travel, 05 missions/loot, 06+08 Steam P2P, 07 death/respawn, 10 attribution,
plus the client-UI desync investigation). Two of them were adversarially re-verified before
implementation (08 verifies 06, 09 verifies 05) and both found crash-grade ABI errors — treat any
unverified report's RVAs and ABI as hypotheses: check uniqueness in `sdk/symbols.tsv` and re-read the
disassembly before hooking. Known doc drift: `docs/PLAN.md` names `ServerExecRPC` as the channel (it is
`ServerChangeName`), an old log path under `Saved/`, a client-side GameMode that was never built, and a
`scripts/gen-sdk.sh` / `es2-logs` skill that don't exist (use `tools/gen_sdk.py` and es2-debug);
`README.md` says six skills and its status paragraph predates combat/world-state/travel sync; the
es2-sdkgen skill's step 5 says `PE_TIMESTAMP` regenerates itself (it is the hand-maintained literal).
