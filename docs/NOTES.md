# ES2 Co-op — running findings & log

## Current state (2026-08-20): 2-player co-op with own ships, shared combat and shared jumps

One command brings up a full session: `scripts/coop-session.sh --kill`
One command checks it: `python3 scripts/verify.py --travel`  → **13/13 checks pass**

### What works
- **Loader / tooling** — `dwmapi.dll` proxy built from Linux with zig; every address generated from the
  game's 1.8 GB PDB; TCP debug console per instance; crash symbolisation (`scripts/crash.sh`).
- **Session** — host `listen`, client `connect`, HELLO/WELCOME handshake, 4-slot player registry.
- **Ship movement** — the owning client is authoritative over its own ship (host clears
  `bReplicateMovement` on client-driven pawns), the host mirrors it locally with dead-reckoning +
  convergence, and relays it to other clients (`PT`) for 3rd/4th players. No rubberbanding.
- **Per-player loadout** — each client serializes its own ship (`FShipData` → `FShipDataState` →
  `UScriptStruct::ExportText`, ~25 KB) and streams it to the host, which substitutes the single
  `UInventoryLib::GetCurrentShip()` call the spawner makes. **Proven**: with the client rewriting
  `coil_gun`→`scatter_gun` in its own blob, the host kept `coil_gun` and the client's host-spawned
  pawn had `scatter_gun`.
- **Combat** — the client forwards fire intent; the host presses the trigger on that player's
  server-side controller, so shots/damage/kills happen in the host's authoritative simulation and
  replicate. Host broadcasts each player's `HitpointRatio` at 4 Hz.
- **Client-side authority** — measured: a client has **0 AIControllers and 0 authoritative pawns**;
  UE's own architecture (no GameMode on a client) already suppresses client NPC spawning and AI.
  A spawn guard + census (`spawnlog`, `guard`) exists as a safety net, using
  `FActorSpawnParameters::bRemoteOwned` to distinguish replication from local gameplay spawns.
- **Travel** — `goto <LocationID>` (either side) runs ES2's real `ChangeLocation`; the host re-arms
  its listen server on the far side and clients auto-reconnect. World origin shifting is forced off
  in MP (it silently desynced all absolute coordinates between host and client).
- **Capacity** — `maxplayers` (GameSession default is 16; ES2 has no cap of its own).

### Hard-won gotchas (all cost a crash to find)
- MSVC x64 large-struct returns: **member** functions are `(this, sret)`; **static** ones are `(sret)`.
- `FString`/`TSubclassOf` parameters "by value" are passed **indirectly**; taking one by value in a
  detour double-frees it.
- `UInventory::CreateShipDataFromState` calls `GetCurrentShip` internally → hooking it needs a
  re-entrancy guard, and the spawner calls it ~850×/spawn so the substitution needs a return-address
  filter.
- `AESHUD::Tick` faults on a null local pawn (respawn/death/travel gaps) — guarded.
- `AGameModeBase::RestartPlayerAtTransform` only spawns when the controller has **no** pawn.
- The host game mode caches the last spawned pawn/controller/playerdata; a joining client clobbers it.
- ICF folds identical functions — never hook an RVA shared by >1 symbol (`gen_sdk.py` flags these).

## Remaining
- **Missions / dialog / loot** (P5 second half): mission logic is host-only by nature (no GameMode on
  clients); the client needs the host's objective/HUD state fed to it, and loot must be routed to the
  collecting player's own inventory. Research in `docs/research/05-missions-loot.md`.
- **Damage attribution**: `UHealthComponent::TakeDamage` and friends call `GetESPlayerPawn()`, a
  singleton that always returns the *host's* ship, so XP/credit for a client's kills currently accrues
  to the host. Inventory of these singletons is in `docs/research/06-steam-p2p-4player.md` §6.
- **4 players**: all structures are N-player and capacity is raised, but only 2 instances fit in this
  machine's RAM (~3 GB each), so 3-4 players is untested here.
- **Steam P2P**: `netdriver steam` + `connect steam.<SteamID64>:7777` is wired; the URL resolution path
  is fully mapped in `docs/research/06-steam-p2p-4player.md` §2. Needs two real Steam accounts that are
  friends to test.
- **Death**: a dead player becomes `BP_Pawn_GameOver_C`; co-op needs respawn-instead-of-gameover.

## Perf note (user tip): prefer `-dx11` (DXVK) over DX12 (vkd3d) on this machine.
