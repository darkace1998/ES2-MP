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
- **NPC visual desync is NOT fixed, and three approaches failed.** Measure it per-frame on ONE machine
  (`jitter <guid> [sec]`): the host moves a scout with a worst frame 1.26x its median step, the client
  1603 uu against a 519 uu median — 3.1x. What did not work: (a) raising `NetServerMaxTickRate` from 30
  had no measurable effect, and bandwidth was never the limit (MaxClientRate 100000,
  NetCullDistanceSquared 1e30); (b) hooking `AActor::PostNetReceiveLocationAndRotation` AND
  `PostNetReceivePhysicState` caught nothing at all — UE5 applies physics replication through its own
  physics path, not those callbacks; (c) detecting outlier steps per tick and walking the error off made
  it strictly worse (max step 1608 -> 3099 uu), because the body keeps physics-simulating underneath and
  the corrections stack. Untried next idea: stop client-side NPC proxies simulating physics at all and
  drive them purely from replicated transforms.
- **Never compare a host reading with a client reading through two console calls.** They are tens of ms
  apart; at ~10000 uu/s that skew is worth hundreds of units, the same size as what is being measured.
  Every cross-machine position number taken that way is noise — measure per-frame on one machine instead.

- **Never aim a networked shot at a world point.** A client's copy of a moving NPC trails the host's by
  the network latency — measured 574-3100 uu at ~10000 uu/s, many ship-lengths. Firing along a ray to
  where the CLIENT saw the ship misses on the host every time, which is exactly why a client could kill a
  stationary turret but never a moving ship. When the host knows which actor the player is on (synced
  auto-aim / lock target) it aims at where IT has that actor; the reported point is only the fallback for
  empty space.
- **A client regenerates its own shield.** `UShieldComponent::TickRegeneration` has no authority check, so
  local regen races the host's authoritative value and the bar visibly saws. Blocked on clients — the host
  is the only thing that should move hitpoints there. Verified tracking: host 0.306/0.355/0.400/0.454,
  client 0.300/0.355/0.393/0.433, monotonic, no oscillation.
- **Mod traffic on the reliable channel starves actor replication.** Streaming aim at 20 Hz plus six NPC
  aims at 10 Hz measurably worsened NPC position error: mean 1016 uu with it on versus 660 uu with it off.
  Aim is now 20 Hz only while the trigger is down, 4 Hz idle, and NPC aim 6 Hz / 4 per tick — mean error
  574 uu. Budget this channel: it also carries loadouts, world state and travel.
- **A force-lock at 185 km is not a sync bug.** The `lock` console command locks the nearest target at any
  distance; ES2 on the host then correctly drops an out-of-range lock. The verify check gates on the
  target being within 10000 uu before asserting.

- **A client's shots had no aim on the host — literally NaN.** The host pulls a client's trigger, but the
  client's server-side `UWeaponComponent` has no player behind it: `SmoothedAutoaim` takes its inputs from
  a controller with no camera or crosshair and writes `FocusLocation` (+0x888) as `(nan, nan, nan)`.
  `AWeaponBase::GetAimDirection` reads exactly that field, so the authoritative shot was aiming at
  nothing. Measured live: client `(203550, -62014, 33505)` vs host `(nan, nan, nan)`. The client now
  streams its own FocusLocation at 20 Hz and the host stamps it onto the server-side components at the
  top of `UWeaponComponent::TickComponent` — the shot is taken earlier in that same tick than the
  `SmoothedAutoaim` call that overwrites the field, so one write per tick is enough.
- **Steam persona names break naive log parsing.** Once the roster carries real names, the `players`
  table has entries like `[DC-Lan Party] DARKACE` — spaces and brackets — so a `(\S+)` name column
  silently matches nothing and the harness reports "no client player registered".

- **`TSubclassOf` really is passed indirectly.** `UWidgetBlueprintLibrary::Create` opens with
  `mov rbp, qword ptr [rdx]` — it dereferences the class argument. Passing the `UClass*` by value makes
  it read the pointer as an address and return null ("CreateWidget failed"). Pass `&theClass`.
- **`UPanelWidget` has no `InsertChildAt`** in this build (only AddChild/RemoveChild/RemoveChildAt), so an
  injected menu entry can only be appended. The main menu's VerticalBox has no spare room either: two
  extra rows pushed the DLC entry off the panel, which is why the multiplayer entry doubles as its own
  status line rather than adding a second row.

- **The HUD caches the pawn; co-op replaces it.** `WG_Ingame_HUD_C` is created under the GameInstance,
  resolves the player pawn and its weapon/device/consumable components once in `Construct`, and ES2 never
  rebinds — single-player never swaps the player pawn. After the loadout placeholder swap (or a respawn)
  a client's HUD reads a destroyed actor, which is what froze weapon swap, the drive charge and the
  equipment slots. Cure is ES2's own `WG_Ingame_HUD_C::ReInit` via `PC -> MyHUD -> IngameHudWidget`;
  `loadout::HudRebindTick` calls it on every pawn change. Do NOT re-run `Construct` — it rebuilds the
  child slot widgets.
- **Never measure across the loadout swap.** The client's ~25 KB blob lands ~95 s after it joins, and
  applying it REPLACES the client's pawn and returns it to the spawn point. Both `combat-test.py` and
  `verify.py` were sampling a weapon component belonging to the retired pawn, reporting "no damage
  attributable" and a dead `bFireActivated` while routing was in fact fine. Both now wait for
  `shipdata stash` to report `applied=1` first.
- **`bFireActivated` is a bad probe.** It is only true while the weapon is mid-cycle and it hangs off a
  pawn that may have just been replaced. Assert on the host's `combat fireApplied` counter instead —
  it increments exactly when the host presses that player's trigger.

- **`ApplyESRadialDamage` was never suppressed on clients, only `ApplyESPointDamage`.** Explosive impacts
  therefore killed the client instantly: `AProjectileBase::OnImpact -> Explode -> ApplyESRadialDamage ->
  AWeaponBase::CheckForItemDamageChangingEffects -> AESGameModeBase::CheckForItemDamageChangingEffects_BP`
  on a null game mode (AV reading 0x10). Both are now blocked together while a client.
- **Component init order.** `AActor::PostActorConstruction` (0x156A448) runs `PreInitializeComponents` ->
  `AActor::InitializeComponents()` -> `PostInitializeComponents`. Anything a *component* reads in its
  `InitializeComponent` must exist before the FIRST of those. Filling `AESPawn::ShipData` at PostInit was
  one step too late for `UDeviceComponent::Init` / `UConsumableComponent::InitializeComponent`, which is
  why a client had weapons (rebuilt explicitly by the mod) but no devices or consumables.
- **The combat authority test must aim first.** Teleporting a ship leaves it pointing wherever it was;
  ES2's auto-aim only covers a narrow cone, so without `aim` the client shoots into space and the test
  reports "no damage attributable" even though routing works. With `aim` it destroys the target.

- **`-nullrhi` hosts crash the moment a weapon fires.** `AWeaponBase::PlayWeaponFireFX` (rva 0x12D55F8)
  calls `UGameplayStatics::SpawnEmitterAttached(MuzzleFlashParticleSystem, RootComponent, ...)` and then
  does `and byte ptr [rax+0x513], -2` on the result **without a null check**. Without an RHI that spawn
  returns null, so the host dies with `EXCEPTION_ACCESS_VIOLATION writing address 0x513` inside
  `ProcessNewShot -> ProcessFiring -> PlayWeaponFireFX`. Nothing to do with co-op — it is ES2's missing
  null check — but it makes `--nullrhi-host` unusable for anything that shoots. Guard the FX call (skip
  when `AActor::RootComponent` @0x1B8 is null) if a headless host is wanted for 3-4P testing.

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
- **Join latency.** From entering the host's world to the client's loadout being fully transferred is
  now ~16 s (was ~29 s): ~12 s waiting on the HELLO/WELCOME handshake and ~4 s for the 54-chunk blob.
  Neither HELLO nor the loadout export needs a pawn — the export reads this process's UPlayerData —
  and both used to wait for one. The remaining ~12 s is the handshake itself, not the transfer.
- **Aim is synced in both directions now.** A client streams its own FocusLocation to the host (which
  fires for it), and the host streams the FocusLocation of every *currently firing* NPC back to clients,
  keyed by the same NetGUID the trigger mirroring uses. Only firing shooters are sent, at 10 Hz, six per
  tick round-robin, so a busy fight cannot flood the reliable channel. Verified by matched NetGUID:
  identical focus on both machines, bar the ~100 ms of staleness while an NPC is turning.
  `CurrentAutoAimTarget` rides along as a NetGUID and is stamped the same way (verified: guid 35 on both
  machines for the same turret).
- **A NetGUID only resolves backwards reliably on a CLIENT.** `FNetGUIDCache::GetObjectFromNetGUID` is
  the direction a client needs, so its map is populated; a server mostly needs object -> guid and asking
  it the other way returns whatever is there — observed handing back `Default__BP_Outlaw_Scout_C`, a class
  default object, for a guid that genuinely belonged to a turret. Acting on that meant locking onto a CDO.
  Always round-trip the answer (`GetNetGUID(result) == guid`) and fall back to scanning actors on the
  object -> guid direction, which is reliable on both sides.
- **Target lock DOES sync; SetLockedTarget needs nothing special.** It was measured setting the weak
  pointer correctly (index=111307 serial=30088) and ES2's own weapon tick never clears it
  (instrumented across the tick: heldBefore=106, heldAfter=106, clearedByTick=0). Its only precondition
  is a self-lock guard — `if (newTarget == OwnerPrivate) return;` at 0x12A611F. What actually broke it
  was the bad reverse lookup above, plus an "only attempt when the reported guid changes" rate limit that
  then refused to ever retry after that first poisoned attempt. The real guard is `want == GetLock(wc)`,
  which costs one comparison once the lock is in place. SetLockedTarget does restart the missile lock, so
  carry `RemainingMissileLockTime` across the call.
- **XP not yet observed firing.** The path is implemented and armed, but the harness cannot reliably make a parked ship land a kill, so no live award has been measured.
- **Docking, stations, and mission *item* rewards** are not mirrored; `UMissionLib::AddNonItemRewards` is mapped (XP + credits + job score — it does *not* grant faction standing) but not yet hooked.
- **Mission records that only one side has.** The client applies deltas to existing `FTaskSaveGameData` records; creating one from scratch (320 bytes with TArray/TMap members) is deliberately not attempted.

## Perf note (user tip): prefer `-dx11` (DXVK) over DX12 (vkd3d) on this machine.
