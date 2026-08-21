# ES2 Co-op — running findings & log

## State (2026-08-20): 2-player co-op with own ships, shared combat, world state and jumps

### Validation re-run (2026-08-20, fresh build): 23/23
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
- **Correct for desync, never aim FOR the player.** The first version of the aim fix replaced the
  client's focus point with the host's copy of the target's position. That hits — and turns every shot
  into a guided one: you could fire well beside an enemy and still kill it. The client now also reports
  where IT sees that target, and the host shifts the player's own aim point by the difference. Aiming
  dead-on hits; aiming beside it misses by exactly as much as the player missed by.
- **ES2 replicates no hitpoint state, so enemies look untouched on a client.** The host reports each
  NPC's hull/shield/armour, but only when it changes (a fight is then a handful of messages, not a stream
  per enemy). Applied through `SetCurrentHitpointsWithRatio` like the player values, so the bars move.
- **An NPC dying is invisible on a client** — the host runs the death Blueprint that spawns the
  explosion, the client just has the actor replicated away, so enemies blink out. The host announces the
  death as the hull reaches zero, deliberately BEFORE the actor is destroyed so the client can still
  resolve the NetGUID, and the client calls `Die` on its own copy. `Die` is a BlueprintNativeEvent whose
  Blueprint half carries the FX, and calling it client-side was verified not to hit the null-GameMode
  crash path.

- **NPC proxies are driven from `AActor::ReplicatedMovement`, not left to local physics.** They replicate
  with bRepPhysics, so a client integrates their physics between updates and lurches on every correction.
  Three things did NOT work: raising `NetServerMaxTickRate` from 30 (no effect; bandwidth was never the
  limit at MaxClientRate 100000 and cull distance 1e30); hooking
  `AActor::PostNetReceiveLocationAndRotation` and `PostNetReceivePhysicState` (neither ever fires — UE5
  applies physics replication through its own path); and absorbing outlier steps per tick (strictly worse,
  the body keeps simulating underneath so corrections stack). What works is following the authoritative
  state directly with the same interpolate-and-extrapolate used for remote player pawns.
  `ReplicatedMovement` was verified fresh on the client and tracks the host closely.
- **Measure fidelity, not step size, and never across machines.** Two console round-trips are tens of ms
  apart; at ~10000 uu/s that skew is worth hundreds of units — the size of the effect — so every
  cross-machine position number is noise. Per-frame step size is also misleading: an actor that lags shows
  SMALLER steps without being smoother. The metric that reproduces is deviation of the displayed position
  from that actor's own `ReplicatedMovement`, both read in the SAME tick (`jitter <guid> [sec]`): two
  independent "off" runs agreed at 589/602 uu mean and 1514/1251 uu worst, against 523 uu mean and 960 uu
  worst with following on. The smoothness (jerk) difference stayed inside run-to-run noise — do not claim
  it.
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
- **Never call back into ES2 gameplay from inside a mod message handler.** Handlers run in the middle of
  `UActorChannel::ReceivedBunch -> ReceivedRPC -> execClientMessage`. Running the NPC death Blueprint
  there (it spawns effects and destroys actors) re-entered `ProcessEvent` and took the client down with
  an access violation in `ProcessEvent`'s own parameter `memcpy`. The exact same call is fine one tick
  later — queue the work and drain it from a tick, as `ClientDeathFxTick` does.
- **`WG_Ingame_HUD_C::ReInit` only rebinds the top-level widget.** Its children (`Crosshair`,
  `PrimariesAndDevices`, `WG_JumpDrive`, …) cache the pawn's components in their own `Construct` and
  ReInit does not cascade. After a pawn swap they still point at the retired pawn, so events fire on a
  component nothing on screen is listening to. `RebindStaleChildWidgets` re-runs `Construct` on exactly
  the children holding a component owned by some other actor.
- **The reticle is one leg further on, and ES2 never drives it on a client.** The shape lives in
  `WG_Crosshair_C::WeaponCategory` and is set by that widget's `SetWeaponCategory`. On a client the
  switch itself works and the HUD's slot highlight follows it — only that one call is missing, so a
  thermo gun fires under a coil gun's reticle. The value is the equipped `FWeaponInfo::WeaponItem`'s
  `UItem::GetSubCategoryID()` (`cat_coil_gun`, `cat_thermo_gun`, …); the mod pushes it at 5 Hz and only
  when it differs. `reticle` on the console reports every stage of that lookup.
- **ES2 draws the floating damage number and the hitmarker from one function**, and it is not on the
  damage path proper: `UGameplayLib::DamageDealtByPlayerOrPlayerFriend` (RVA 0x15572A0, ICF-unique)
  classifies the victim's hitpoint component (`IsA<UShieldComponent>` / `UArmorComponent` /
  `UHealthComponent`) to decide which slot the single float belongs in, then calls
  `AESHUD::ShowHitpointNumbers` and `AESHUD::OnPlayerDealtDamage` (that second one IS the hitmarker).
  It gates all of it on comparing the causer with `UGameplayStatics::GetPlayerPawn(world, 0)` — the
  LOCAL player. That is why a co-op client saw neither: on the host the test fails for the client's
  routed fire, so the host correctly stays silent, and the client's own damage path is blocked. Nobody
  was left to draw it. The host now forwards each hit (`DM`) and the client draws it with ES2's own
  `UPooledHUDTextPanel::AddDamageTextWithActor`. Coalesce before sending — a beam weapon calls this
  every tick and one reliable RPC per call is the flood that used to kill clients.
- **`DamageCauser` is not the shooter.** In `DamageDealtByPlayerOrPlayerFriend(hpComp, amount,
  instigator, causer, victim, hit, bIsCritical, bIsKill)` the causer is whatever physically delivered
  the hit: the pawn for some instant-hit paths, but the WEAPON actor (`BP_Weapon_CoilGun`) or the
  PROJECTILE (`BP_Projectile_Autocannon`) for everything else. Attributing by causer silently drops
  those hits. ES2 itself attributes through the instigator — it reads `AController::Pawn` (+0x2E8) and
  compares that with `GetPlayerPawn(world, 0)` — so do the same, with the causer only as a fallback.
- The last parameter is **`bIsKill`, not a radial-damage flag**: at the call site in
  `UHealthComponent::TakeDamage` it is `UHealthComponent::IsDead()`'s return, stored to `[rsp+0x38]`
  (the 8th argument). It is what `AESHUD::OnPlayerDealtDamage` wants for the kill-confirm marker.
  The same stack frame confirms argument 5 is the victim: it is `[hpComp+0x90]`, the component's owner.
- **Do not sum damage events to save bandwidth.** Coalescing per victim at 10 Hz made the client draw
  3.28 where ES2 would have drawn 1.73 and then 1.55, and the number visibly stopped matching the
  weapon. Keep one message per hit and bound the RATE instead (queue per player, drain a couple per
  flush); merge only when a shooter genuinely outruns the budget, which is what ES2 does for constant
  beams anyway.
- Shield damage and hull damage are separate weapon stats and differ by more than an order of magnitude
  — measured on one coil gun, ~1.7 per shield hit against ~36 per hull hit. A small number on screen is
  usually a shield hit, not a bug.
- **Damage numbers are pooled, not spawned**, which is why diffing live widget classes across a hit
  finds nothing. The pool is `WG_Ingame_HUD_C -> PooledHudText`; `ActiveTexts` (non-empty for about a
  second) is the observable that tells you a number is on screen.
- **`ESPawn::Die` takes three parameters and does nothing useful anyway.** Measured on the host with
  hitpoints already at zero: the actor survived and no explosion appeared. The mod used to call it with
  a null parameter block. `BP_ShipBase_C` is where the real sequence lives: `Explode` runs all of it
  including `DestroyAfterExploding`, which destroys a REPLICATED actor on the client and killed it
  seconds later when the timer fired. **`SpawnExplosion` (no parameters, effect only) is the one to
  call.** Only ever pass a null parameter block to a function with `NumParms == 0`.
- **`BP_Explosion_Base_C` has `RemoteRole = ROLE_None`** — destruction explosions never replicate, so a
  client must spawn its own or enemies simply blink out of existence.
- **Never lift the client damage block wholesale to get the visuals back.** Measured: the client dies
  within five seconds, because mirrored NPC fire impacts locally and ES2's damage chain reaches the
  `AESGameModeBase` a client does not have. Forward the presentation instead of unblocking simulation.
- Console `peek` reads raw memory and `class <0xaddr>` resolves a Blueprint class from an instance —
  both needed because native C++ members (`FWeaponInfo::SpawnedWeapons`) have no UProperty and Blueprint
  classes are not in the native class registry. `peek` VirtualQuery-checks the span first; probing a
  bad address used to fault the game.

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

## Code audit (2026-08-21): 16 bugs found and fixed, verify.py 25/25

A full read-through of `mod/src`, `scripts/` and `tools/`. Everything below was fixed and re-verified
against a live host+client session. Grouped by what would have bitten first.

**Would hang or corrupt the game**
- `console::Dispatch`/`RunNow` built the "unknown command" reply *while holding* `g_cmdsMutex`, and
  `HelpText()` takes the same non-recursive mutex — one typo (`staus`) deadlocked the TCP thread with
  the lock held, so every later console command hung, and the game thread froze the moment a queued job
  or `menu`'s `console::Dispatch` needed it. Lookup now copies the entry and releases the lock first.
- `hooks.cpp`'s `std::map` was written from the init thread while the console thread (`hooks`) and the
  game thread (`combat damageblock`, via `hooks::Enable`) read it. Added a mutex.
- `call <obj> <Fn> <args...>` walked the UFunction's whole property list, which also contains its LOCAL
  variables (past `ParmsSize`). One extra argument therefore wrote past the parms buffer — heap
  corruption inside the game. Now only `CPF_Parm` properties take arguments, extras are reported.
- `menu`'s `CopySlotSettings` copied `[Size .. end of UVerticalBoxSlot)`, and the class ends with the
  private `SVerticalBox::FSlot* Slot` (+0x58). The snapshots are taken *before* `RemoveChild` frees that
  FSlot, so every re-added menu row got a dangling Slate pointer stamped into it (and our own row
  aliased Continue's). Now copies only the reflected settings and re-applies them through the slot's
  own setters, with a `static_assert` on the layout.

**Wrong behaviour**
- A timed-out console job stayed queued and ran later anyway — after the caller had been told it failed
  and had re-issued it, so `LoadGame`/`listen`/`connect` could run twice. It is withdrawn on timeout.
- The console bound "the first free port in `[base, base+9]`" even when `ES2COOP_CONSOLE_PORT` pinned
  one, while the harness records the *requested* port: with a straggler on 27100 the tests silently
  drove the wrong instance. A pinned port is now exact-or-nothing (and says so in the log).
- `connect steam.<id>` *persisted* the Steam driver mode and opened the P2P accept gate on a machine
  that is a client, not a host — a later `listen` then came up on SteamNetDriver unasked. The address
  now selects the transport for that connection only.
- `ClearRichPresence` takes no key; `SetConnectPresence("")` was wiping *every* rich-presence key the
  game had set. Use `SetRichPresence("connect", "")`.
- `ConnectString()` hardcoded `:7777` while `listen <port>` stored the real one (for Steam the URL port
  *is* the P2P channel, so an invite on another port could never connect). It uses `travel::ListenPort()`.
- An invitee clicking a lobby slot armed hosting too, so the next map ran `connect` **and** `listen`.
  The two arms are now exclusive, and inviting is refused while joining.
- `MT|` was parsed with `%127[^|]` scansets, which *fail on an empty field*: a task with no location
  silently never got its station applied. Split by hand.
- `FName::Make(std::string)` widened bytes instead of decoding UTF-8 (the sibling `FString` ctor decodes),
  so any non-ASCII name became a new garbage entry in the name table.
- `FindClass("/Script/ES2")` passed no class filter to `StaticFindObject`, so a package (or any object
  at that path) came back as a `UClass*` and was then walked as one — and cached.
- `subclasses` and `call <BPClass>` tested "its class is exactly `Class`", which is false for every
  Blueprint class; both now test `IsA(ClassClass())`.
- Actor/component-keyed caches in `combat` and the pawn latches in `loadout` were never cleared on a
  world change. UObject addresses are recycled, so a stale key is a *wrong answer*, not just dead weight
  (a new NPC inheriting `announcedDead` never gets its explosion). Added `OnWorldChanged()` to both,
  called from coop's world-change branch.
- `T|`/`PT|` accepted `nan`/`inf` (`%lf` parses them) and fed them to `SetActorTransform` +
  `SetPhysicsLinearVelocity`; `coop hz 0` divided by zero. Both rejected/clamped now.
- A Steam persona containing `|` or `=` corrupted every later entry of the `ROSTER` message.

**Rejected after investigation** — `IsValidObject` looked wrong: it documents "not garbage" but only
checks that the pointer is in `GUObjectArray`, so a destroyed actor reads as live for up to a GC cycle.
Making it strict broke three checks: `RebindStaleChildWidgets` detects a stale HUD child precisely by
reading a *retired* pawn's component, and `ActorFromNetGuid` could no longer resolve a dying NPC, so
death FX were lost. The semantics are load-bearing. Reverted, documented, and `IsGarbage()` added for
callers that genuinely want "not pending-kill". **Never tighten `IsValidObject` again.**

## NPC jitter, measured properly (2026-08-21)

All numbers from `jitter <guid> [sec]` on the CLIENT, same moving scout (~12000 uu/s), interleaved runs,
~190 samples per 10 s window. Two metrics: *deviation* = distance from that actor's own
`ReplicatedMovement` read in the same tick (lag), *sd/mean* and *jerk* = frame-to-frame step consistency
(smoothness). Run-to-run noise within one condition is large (404 vs 622 u on the same setting), so only
differences bigger than that are worth anything.

| condition | deviation (mean) | sd/mean | worst jerk |
|---|---|---|---|
| simulating, position only (shipped behaviour) | 513 u | 0.67 | 2.9x |
| simulating, position+velocity+rotation | 580 u | 0.68 | 3.4x |
| **kinematic**, position+rotation, rate 12 | 717 u | **0.30** | **1.87x** |
| kinematic, position+rotation, rate 25 | 531 u | 0.54 | 2.59x |
| kinematic, position+rotation, rate 40 | 556 u | 0.53 | 2.32x |

- **Writing the replicated velocity onto the body does NOT help** (the obvious fix, and it is wrong).
  `NpcFollowTick` runs from the `UGameEngine::Tick` detour *before* the world ticks, so physics integrates
  a full frame AFTER our write; handing it the full velocity overshoots the target by ~v*dt (~600 u at the
  20 fps this box manages with two instances). Left in behind `combat npcvel 0/1`, default OFF.
- **Making the proxy kinematic is what works.** `call 0x<rootcomp> SetSimulatePhysics 0` on one NPC more
  than doubled smoothness (sd/mean 0.74 -> 0.30, jerk 3.30x -> 1.87x) — removing the competing integration,
  not correcting it harder, is the win. Cost is lag, which is then purely a tuning parameter: rate 25 gets
  the deviation back to the shipped 513 u while still being smoother (0.54 vs 0.67). Rotation following
  becomes mandatory in that mode (`combat npcrot 0/1`) because nothing else orients a proxy; note rotation
  replicates as `ByteComponents` (~1.4 deg/axis), so it will want smoothing.
- **Chase rate trades lag against smoothness monotonically** (12 -> 25 -> 40: deviation 717/531/556,
  sd/mean 0.30/0.54/0.53). A chase filter cannot win both; fixed-delay interpolation over a snapshot
  buffer is the way to get both, at a constant known latency.
- **Frame rate dominates the perception on this machine, and it is NOT the mod.** Two instances run at
  ~21 fps (host 22.9, client 20.3); with the mod's per-frame NPC work switched off (`combat npchp 0`,
  `npcfollow 0`, `npcaim 0`, `aimsync 0`) the rate was identical (22.9 / 20.4). At 21 fps a ship at
  12000 uu/s moves ~570 u per frame, which is the same size as the effect being measured. Do not tune
  smoothing against a 21 fps reference.
- **"Damage is not registering" was a feedback problem, not a data problem.** Counters over one session:
  client fireSent 56 -> host fireApplied 56; host dmgSent 112 -> client dmgShown 112; deathsRx 6 ->
  deathsPlayed 6, none lost; npcHpApplied 177; client and host FocusLocation agreed to <100 u. Every hit
  is applied and reported. What a client never gets is ES2's own impact presentation: both
  `ApplyESPointDamage` and `ApplyESRadialDamage` are disabled there (they reach the null AESGameModeBase),
  and that path is what produces impact flashes, shield ripples and hit reactions. The mod re-adds only
  the floating numbers, the hitmarker and the death explosion — so shots look like they pass through.
  Mirroring the impact FX the way NPC deaths already are is the fix.

### Kinematic client proxies (2026-08-21) — and a vtable trap worth remembering
`combat npckinematic 0/1` (default ON, client only): each replicated ship proxy this tick drives has its
root body switched to non-simulating, and is then moved purely by our follow code. Measured win on one
scout: step spread sd/mean 0.74 -> 0.30, worst jerk 3.30x -> 1.87x mean. Notes:
- **`UPrimitiveComponent::SetSimulatePhysics` is virtual AND ES2 overrides it**
  (`UMovementRootComponent::SetSimulatePhysics`) — and `UMovementRootComponent` is exactly what every
  ship's `CollisionRoot0` is. Calling the `UPrimitiveComponent` RVA directly silently runs the base
  implementation. It must go through the vtable (`ue::vt::UPrimitiveComponent_SetSimulatePhysics` = 213).
  This was nearly shipped as an RVA call: the live console proof (`call 0x<root> SetSimulatePhysics 0`)
  went through reflection/ProcessEvent, which dispatches virtually — so the experiment worked while the
  implementation would not have. **Check `grep -F '::<Fn>(<args>)' sdk/symbols.tsv | cut -f4 | sort -u`
  for overrides before calling any virtual by RVA.**
- The "is it simulating?" check is a direct flag read (`UPrimitiveComponent::BodyInstance +
  FBodyInstance::bSimulatePhysics_off`), not a call, because it runs per proxy per frame. Re-checking
  every frame also makes the conversion self-healing: anything that re-enables simulation is undone on
  the next tick, whatever the cause.
- Rotation following is forced on in this mode — a kinematic proxy has nothing else to orient it.
- Our own ship is never touched (ROLE_AutonomousProxy is filtered out); it must keep simulating because
  the client is authoritative over its own movement.

### Kinematic proxies: the measurements that shipped it
Same actor, interleaved, two rounds each (`jitter <guid> 10` on the client).

Kinematic vs simulating, on a scout at ~10000 uu/s:

| | deviation (lag) | sd/mean | worst jerk | max step |
|---|---|---|---|---|
| kinematic  | 555 / 594 u | **0.29 / 0.24** | **1.75x / 1.01x** | 729 / 704 u |
| simulating | **354 / 338 u** | 0.68 / 0.69 | 2.92x / 2.62x | 1206 / 1201 u |

2.6x smoother and 40% smaller worst step, at the cost of ~230 u more lag. Both conditions repeated
tightly, unlike the earlier velocity experiment — this is a real effect, not noise.

The lag has a specific cause and is largely recoverable: this tick runs from the `UGameEngine::Tick`
detour, i.e. BEFORE the world ticks and draws, so what we write is displayed one frame later. Predicting
to `now + dt` (`combat lead 0/1`, default on) cancels most of it. On a GB fighter at ~3000 uu/s:

| | deviation (lag) | sd/mean |
|---|---|---|
| lead on  | **87 / 85 u** | 0.31 / 0.32 |
| lead off | 221 / 208 u | 0.19 / 0.30 |

60% less lag for a slight, noisy smoothness cost (extrapolating further amplifies the whole-number
quantization on the replicated velocity). Absolute numbers are not comparable between the two tables —
different NPC, ~3x different speed.

### Kinematic proxies: validation
- `verify.py` 23/23 with kinematic on by default — but three checks degraded to INFO that run (no enemies
  in range), so the combat paths were NOT covered by it. Forced separately: client parked next to an
  outlaw and firing, host dmgSent 45 -> 139, client dmgShown 45 -> 138 (one still in flight at sample
  time), host deathsSent 0 -> 2, client deathsRx 2 -> deathsPlayed 2 with deathsNoActorAtRx=0 and
  deathsNoActorAtDrain=0, npcHpApplied 43 -> 203. Death FX in particular still work: `SpawnExplosion`
  runs fine on a kinematic actor.
- Nothing re-enables simulation: `madeKinematic` stayed at 12 with `tracked=12` across a whole fight,
  i.e. each proxy was converted exactly once (the counter increments on every SetSimulate(false), so a
  fight over the flag would show up as it climbing).
- Ramming a kinematic NPC is safe: teleporting the client onto one left it at +113 uu with velocity 0 —
  depenetrated, not stuck, not launched — console still responsive, and the host agreed on the position
  exactly (client-authoritative movement sync unaffected).
- The toggle restores properly (`combat kinematic 0` -> restored=6, bodies simulating again).

## Why NPC death explosions are inconsistent on a client (2026-08-21)

Three observed behaviours — some explode, some just vanish, some explode *then* tumble out of control
*then* vanish. Two causes, and one plausible hypothesis that measurement killed.

**1. "Just vanishes": the class has no `SpawnExplosion`.** It is a `BP_ShipBase_C` function, and only the
ship branch inherits it. Class chains read live:
- ship   `BP_Ship_Player_C -> BP_ShipBase_C -> BP_PawnBase_C -> ESPawn` — HAS `SpawnExplosion` (parms=0)
- turret `BP_Turret_Homebase_Station_C -> BP_Turret_Stationary_Base_C -> BP_TurretBase_C -> BP_PawnBase_C -> ESPawn` — only `ESPawn::Die` (parms=3)
- anemone `BP_Cave_Anemone_Small_C -> BP_Cave_Anemone_Base_C -> BP_PawnBase_C -> ESPawn` — none

`ClientDeathFxTick` does `if (!fn || NumParms != 0) continue;` — a **silent** skip that increments no
counter, so it is invisible in `combat` output, and it makes verify's `deathsPlayed == deathsRx` check
fail with no indication why. Any non-ship ESPawn therefore dies without an explosion.

**2. "Explode, then out-of-control, then vanish": the FX fires at the START of the death sequence.**
The host announces on hull<=0.001 (`H_HealthDepleted`), deliberately early so the client can still
resolve the NetGUID. But hull-zero is where ES2's death sequence *begins*: the ship then tumbles out of
control and only explodes and is destroyed at the end (host ships were observed sitting at `hp=0.000`
in the actor list). So the client plays the explosion immediately, keeps faithfully following the host's
dying ship as it tumbles — much more visible now that proxies are kinematic and smooth — and the ship
only disappears when the host finally destroys it.

**3. REFUTED: the mirrored `NH` hull=0 does not start a local death.** Suspected the client's own
Blueprint ran its death sequence off `SetCurrentHitpointsWithRatio(0)`. Tested directly on an unattacked
turret (client-only call, no host involvement): the client's copy was still alive 17 s later and the
host's copy was untouched. An earlier apparent positive was contamination from a live fight. The setter
really does not run the depletion path.

**Fix directions.** (a) Stop depending on a ship-only Blueprint function: spawn the explosion actor
ourselves at the dying actor's transform (the host spawns `BP_Explosion_Base_C`, `RemoteRole=ROLE_None`,
which is why it never replicates), which works for every class — and count the skips instead of
`continue`. (b) Trigger on the real explosion rather than on hull-zero: keep the early `ND` so the client
can resolve and CACHE the actor pointer, then fire the FX on a second message sent when the host actually
destroys/explodes the ship (`AESPawn::Destroyed`, or detecting the host's own `SpawnExplosion` call), so
no late guid resolution is needed.

### Death FX: fixed (2026-08-21)
Both causes above are addressed in combat.cpp.

**Timing.** `ND` no longer plays anything; it only records "the host says this actor is dead" in a
`g_dying` set. The FX is played from a new hook on **`AESPawn::Destroyed`** (client), i.e. the frame our
copy actually goes away — which is when the host really destroyed it, after its out-of-control sequence.
Two things fall out of that for free:
- the guid-resolution race disappears: inside `Destroyed` the actor is by definition still valid, so
  "could not resolve in time" is no longer a way to lose an explosion;
- relevancy loss cannot cause a false explosion, because only actors the host announced are in the set.
A `g_dyingDeadline` (8 s) safety net still plays the FX if our copy somehow outlives the announcement,
so an explosion is never silently dropped; `fxTimeout` counts those.

**Class independence.** `SpawnExplosion` is used when the class has it (ships), otherwise the mod now
spawns `BP_Explosion_Base_C` itself at the actor's transform via
`UGameplayStatics::BeginDeferredActorSpawnFromClass` + `FinishSpawningActor` (TSubclassOf by hidden
pointer, FTransform by const&). The class is found by scanning for the class object, since it is
Blueprint-generated and `FindClass`'s short-name path only sees native classes. Verified live: on a
turret `noFn=1 fallbackSpawned=1 failed=0` and a real `BP_Explosion_Base_C_2147420239` instance appeared
in the level; on a ship `played=1 fallbackSpawned=0 noFn=0`, i.e. it still uses the ship's own function.

New counters (`combat`): `fxAtDestroy`, `fxTimeout`, `pendingDying`, `noSpawnExplosionFn`,
`fallbackSpawned`, `fallbackFailed` — the old silent `continue` is gone. `combat fxtest <guid>` plays the
FX on demand so both paths can be exercised without waiting for a kill, and `combat fxfallback 0/1`
disables the spawned-actor path.

**Validated live.** `verify.py` 23/23 on this build (its explosion check degraded to INFO — no enemies in
range that run — so it was forced separately). Client killing outlaws: host deathsSent=2 -> client
deathsRx=2, **fxAtDestroy=2** (both played from the new `Destroyed` hook, i.e. at the real destruction),
fxTimeout=0, deathsNoActorAtRx=0, deathsNoActorAtDrain=0, deathsPlayed=2; damage 210 sent -> 210 shown.
Note the enemy mix matters when reading these: drones ARE ships
(`BP_Outlaw_Drone_C -> BP_DroneBase_C -> BP_Ship_NPC_C -> BP_ShipBase_C`), so they use their own
`SpawnExplosion`; only the turret/anemone branch takes the spawned-actor fallback.

## Steam invites never armed the join (2026-08-22)

From a real two-account attempt (logs in run/TEST): the host sent the invite, the invitee launched, and
nothing happened. The invitee's own command line was the whole story:

```
cmdline: "...\ES2-Win64-Shipping.exe" ES2 steam.76561198076335468:7777
```

**Steam appends the `connect` rich-presence string to the accepting friend's command line VERBATIM.** It
does not add a `+connect` of its own — the key is documented as *the command line*, so the prefix has to
be part of what we publish. We published the bare address while `CheckInviteCommandLine` searched for
`"+connect "`, so the two halves of our own protocol disagreed and the join was never armed, silently.
Fixed at the publish points (`steamp2p::SetConnectPresence` / `OpenInviteOverlay` prefix it themselves,
idempotently, so no caller can get it wrong), and the parser now accepts either form — it tokenises the
command line honouring quotes and takes a `+connect <addr>` pair or a lone token that looks like a
connect address. Unit-tested against the real failing command line plus negatives (a quoted install path
containing `steam.stuff:1` must not match).

Two further defects the same logs exposed:
- **The host advertised Steam but hosted on IpNetDriver** (`netdriverdef GameNetDriver -> IpNetDriver` in
  the host log) — the menu path never selected the transport, and it cannot be selected after the fact
  because `EnableListenServer` only creates a net driver when the world has none. Now a lobby-slot invite
  sets the intent and the menu runs `netdriver steam` immediately before its `listen`, which also opens
  the P2P accept gate. Deliberately tied to the INVITE, not to the MULTIPLAYER toggle: the toggle also
  covers LAN hosting, where joiners arrive by IP and a SteamNetDriver would lock them out.
- **There was no "joining" state.** `LobbyLine()` only had ON/OFF, so an invitee saw "MULTIPLAYER: ON",
  which reads as "I am hosting" — the opposite of what was about to happen. INSTALL.md already promised
  `JOINING A FRIEND`; it exists now.

Also worth remembering from that host log: it armed hosting and sent invites but never loaded a save, so
no listen server was ever created. Arming is not hosting — the map has to come up.

### The lobby overview drew over the settings screen (2026-08-22)
The four lobby slots are parented to the main menu's ROOT canvas and added last, so they hold the highest
Z-order there and painted over whatever sub-page the menu raised — opening Settings left the overview
sitting on top of the graphics options, covering the Display Mode / Resolution / HDR / V-Sync values.
Their visibility was driven by `LobbyVisible()` alone, which knows nothing about which page is showing.

The signal is ES2's own `WG_MainMenu_New_C::IsInRootMenu()` (0 params, bool return), so the fix covers
every sub-page rather than just the reported one. Two more obvious candidates are both wrong here:
- **`SwitcherSubPages->ActiveWidgetIndex`** is already 1 (`WG_Menu_Main_OptionsScreen_C`) while the front
  page is displayed — the sub-pages live in a WidgetSwitcher whose index does not mean "this is on screen".
  Index 0 is a blank Image, 2 `WG_NewGame_C`, 3/4 `WG_LoadOrSaveGame_C`. The root page's buttons are not in
  the switcher at all.
- **The `Visibility` UPROPERTY is the designer value**, not runtime state: switcher, options page, our
  button and `BoxAllButtons` all read `SelfHitTestInvisible(4)` regardless of what is on screen. (Widgets
  we drive ourselves are the exception — `SetVisibility` does update their property.)

`menu` now prints `lobbyVisible/frontPage/(slots shown)/pendingJoin/steamTransport` so this is checkable
without screenshots. Measured across the transition: front page 1/1/1, sub-page open 1/0/0, back 1/1/1.

Note when testing menu navigation from the console: `call <ButtonOptions> OnBtnClicked` and
`call <menu> OnOptionsPressed` set the navigation state (IsInRootMenu flips) without necessarily running
the visual transition, and calling them out of order can leave the menu drawn as neither page. That is an
artefact of driving Blueprint navigation directly, not a mod bug — `InitRootPage` puts it back.
