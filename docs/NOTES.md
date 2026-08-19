# ES2 Co-op — running findings & log

## Milestone reached (2026-08-19): 2-player shared world works
Two local instances, host + client, both flying in the same location with both player
ships spawned server-side and visible to each other. Reproducible with one command:
`scripts/coop-session.sh --kill`.

### What works
- **Loader**: `dwmapi.dll` proxy (zig-built) loads into `ES2-Win64-Shipping.exe` under Proton
  with `WINEDLLOVERRIDES="dwmapi=n,b"`. PE-timestamp guard confirms the PDB matches.
- **Engine access layer**: GUObjectArray iteration, FName↔string, class/property/function
  reflection, ProcessEvent, StaticFind/LoadObject, console-command exec — all live via the TCP console.
- **Headless save load**: `UUserFunctionsLib::LoadGame` from the console → loads straight into a location.
- **Listen server**: `UGameInstance::EnableListenServer` after forcing GameNetDriver→IpNetDriver and
  hooking `UIpNetDriver::GetSocketSubsystem` → `ISocketSubsystem::Get("Windows")` (Steam had hijacked the default).
- **Client join**: `open 127.0.0.1:7777` → full UE login flow (Browse → SetGameMode → PostLogin →
  RestartPlayer → AESGameModeBase::SpawnDefaultPawnAtTransform spawns BP_Ship_Player_C for the joiner).
- **Native replication**: host's NPCs/turrets/drones replicate to the client automatically (client sees
  them as role 1/3 simulated proxies); both player ships have bReplicates + bReplicateMovement true.
- **Co-op core**: role detection, pause suppression in MP, a 0.5s relevancy sweep (uncaps
  NetCullDistanceSquared on ESPawns), and a client→host player-ship transform channel at 20 Hz over a
  repurposed reliable RPC (`APlayerController::ServerChangeName`), server→client over `ClientMessage`.

### Key fixes discovered
- `™` in the install path breaks `proton run` → launch via `~/es2game` symlink.
- `UNetConnection::LowLevelGetRemoteAddress` is pure-virtual in base → call via vtable slot 93.
- ICF folds many stubs → never hook an RVA shared by >1 symbol (gen_sdk flags them).
- `TSubclassOf<>` / large structs pass by hidden pointer.

## Remaining work (see PLAN.md phases P4–P6)
- **P4 Combat**: suppress client-side spawners/AI so only host authoritative NPCs exist on the client
  (client currently would double-spawn if it ran its own AI ticks — needs the SpawnActor client-guard hook);
  route client weapon fire/damage to the host; replicate health/shield/weapon state.
- **Ship loadout**: the joiner gets a default-loadout server pawn; need to apply the client's own
  UPlayerData ship/equipment to their server-side pawn (SpawnPlayerShip with the client's ship index).
- **Camera/possession polish**: client's pawn is server-authoritative (role 2/3) — verify input/possession
  and remove the transform-channel jitter (currently teleport-based; move to smoothed interpolation).
- **P5 World state**: location jumps (ServerTravel + clients follow via ClientTravel), docking/stations,
  mission/dialog gating so both players advance together, loot ownership.
- **P6 4 players**: star topology already N-player; add per-player ids, join-in-progress, a host/join UI,
  and Steam P2P transport (`steam.<id>` URLs via SteamNetDriver) for real over-internet play.

## Perf note (user tip): prefer `-dx11` (DXVK) over DX12 (vkd3d) on the Iris Xe / 15 GB box.
