# ES2 Co-op — running findings & log

## State (2026-08-20): 2-player co-op with own ships, shared combat, world state and jumps

### Validation re-run (2026-08-20, fresh build): 21/21
Rebuilt from source and redeployed, then `scripts/coop-session.sh --kill` + `verify.py --travel`:
all 21 checks pass. Session came up clean (host ListenServer, clientConnections=1, 2 PCs each
with a pawn; client netmode=Client in the same world). Co-op jump host->S01L01 with the client
auto-following verified live. Screenshots in `run/shots/coop-live-*.png` show each player flying
its own distinct ship (green Sentinel host / red interceptor client) in the same location.

One harness flake fixed: the `shipdata stash` check sampled once right after connect, before the
client's ~25 KB loadout stream (54 reliable-RPC chunks, up to ~1 min) had arrived — it now polls.
The loadout itself was always correct (client's own ship armed locally, `applied=1`).

Bring up:  `scripts/coop-session.sh --kill`   (add `--steam` to host over Steam P2P)
Check it:  `python3 scripts/verify.py --travel`   → **21/21 checks pass** (19 without `--travel`)

### Working
| Area | What it does |
|---|---|
| Session | host `listen`, client `connect`, HELLO/WELCOME handshake, 4-slot player registry |
| Ship movement | owning client is authoritative (host clears `bReplicateMovement`), host mirrors with dead-reckoning and relays to other clients. No rubberbanding. |
| Per-player loadout | each client streams its own ship (`FShipDataState` → engine text, ~25 KB) and the host substitutes the spawner's single `GetCurrentShip()` call. Proven by rewriting a weapon id in the client's own blob. |
| Client's own ship, locally | a client builds its **own** weapons, devices and consumables through the vanilla component path. ES2 gates all three on `ShipData.Inventory` being non-null and nothing else, so filling ShipData from the client's own `UPlayerData` *before* `PostInitializeComponents` is enough — no authority check is involved anywhere in that chain. |
| Combat | client forwards fire intent; the host presses the trigger on that player's server-side controller, so shots/damage/kills happen in the authoritative simulation. A client never applies damage itself (`ApplyESPointDamage` no-oped in the client role). |
| Enemies you can see shooting | NPC AI runs only on the host and ES2 replicates none of it, so enemies looked inert to a client. The host mirrors weapon trigger *transitions* keyed by `FNetGUIDCache` NetGUID; each side resolves its own copy of the actor and calls the real `StartFire`/`StopFire`. |
| Client authority | measured: a client has 0 AIControllers and 0 authoritative pawns; UE already suppresses this. Spawn census + guard exist as a safety net. |
| Missions | host observes `UMissionLib::UpdateTaskInPlayerData` (dirty-diffed, allocation-free hot path) plus the two writers it cannot see — `UPlayerData::OnMissionCompleted` and `ChangeTrackedMission_Internal`. Client writes its own `FTaskSaveGameData` and refreshes indicators. |
| Dialog | mirrored from the in-game `UDialogManager` singleton only (menu chatter filtered) |
| Loot | instanced: the host announces each drop, every client spawns its own local pickup and collects it into its own `UPlayerData`. Nobody can steal a partner's drop. |
| XP / attribution | every "who is the player?" accessor funnels into `GetPlayerController(world, 0)`, which is always the host. Kills are now scoped to the causing player, that accessor is shimmed for the duration, and `AddXP` is redirected to the earning client. |
| Death | game-over is 100% Blueprint and game-mode-side. The game-over pawn is never possessed (hard veto on `AController::Possess`), the player is respawned in their own ship with hull/shield restored, and `LoadGame`/`ReturnToMainMenu` are blocked while a session is live. |
| Travel | `goto <LocationID>` runs ES2's real `ChangeLocation`; the host re-listens on the far side and clients auto-reconnect. World origin shifting forced off (it desynced all absolute coordinates). |
| Steam P2P | `SteamNetDriver` verified live with `bIsPassthrough=0` (real P2P). The accept gate ES2 leaves shut (`GetNumSessions()<=0`) is forced open while hosting. `steam` self-checks the whole path and prints the join address. |
| Capacity | `maxplayers` (GameSession default 16; ES2 has no cap of its own) |

### Hard-won gotchas (each cost a crash or a silent failure)
- MSVC x64 large-struct returns: **member** functions are `(this, sret)`; **static** ones are `(sret)`.
- `FString`/`TSubclassOf` "by value" params are passed **indirectly**; taking one by value in a detour double-frees it.
- Hooking a function the engine re-enters needs a re-entrancy guard (`CreateShipDataFromState` → `GetCurrentShip`), and when only one call site matters, filter on `__builtin_return_address(0)` — the spawner calls `GetCurrentShip` ~850×.
- `AESHUD::Tick` faults on a null local pawn (respawn/death/travel gaps).
- `AGameModeBase::RestartPlayerAtTransform` only spawns when the controller has **no** pawn — unpossess first.
- ES2's jump must go through `UGameplayLib::ChangeLocation`, not `ESOpenLevel`.
- `EnableListenServer` only creates a net driver when the world has none — select the Steam transport *before* the first listen.
- `UMissionLib::UpdateTaskInPlayerData` is called **every frame** from `AMissionTaskBase::Tick`.
- Two modules must never hook the same RVA (MinHook refuses the second).
- `windows.h` `#define`s `GetClassName` → renamed ours to `GetObjectClassName`.
- `PostInitializeComponents` runs **strictly before** `BeginPlay`. Anything a component reads in `InitializeComponent` must already be there — fixing ship data at BeginPlay is one frame too late and leaves an unarmed default ship.
- `FNetworkGUID` is 8 bytes but **not trivially copyable**, so it returns through a hidden pointer like any large struct. Getting this wrong puts an argument in the sret slot and faults inside `FNetGUIDCache::SupportsObject`.
- Mirroring any AI signal needs edge-detection: ES2's AI calls `StartFire`/`StopFire` every tick it wants to be firing (8000+ calls in 35 s), not once per state change.
- A client has no `AESGameModeBase`. Any ES2 path that reaches it — the whole damage/impact chain does — crashes there, so re-enabling local simulation on a client must go with suppressing that path.
- ICF folds identical functions — never hook an RVA shared by >1 symbol (`gen_sdk.py` flags these).

### Remaining
- **A real 3–4 player run.** All structures are N-player and capacity is raised, but only two instances fit in this machine's RAM (~3 GB each).
- **Completing a Steam connection.** Everything up to the peer is verified on one machine; a second Steam account that is friends with the host is required.
- **Only weapon *trigger* state is mirrored.** Aim direction, target lock and NPC movement still come from whatever the client's own copy of the actor is doing, so mirrored fire is visually plausible rather than frame-exact.
- **XP not yet observed firing.** The path is implemented and armed, but the harness cannot reliably make a parked ship land a kill, so no live award has been measured.
- **Docking, stations, and mission *item* rewards** are not mirrored; `UMissionLib::AddNonItemRewards` is mapped (XP + credits + job score — it does *not* grant faction standing) but not yet hooked.
- **Mission records that only one side has.** The client applies deltas to existing `FTaskSaveGameData` records; creating one from scratch (320 bytes with TArray/TMap members) is deliberately not attempted.

## Perf note (user tip): prefer `-dx11` (DXVK) over DX12 (vkd3d) on this machine.
