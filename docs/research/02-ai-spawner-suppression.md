# ES2 co-op research: ship2

_Auto-generated from the PDB research workflow (2026-08-19)._

## Summary

## How NPC creation and AI work in ES2 (UE 5.5.4, ES2-Win64-Shipping)

### 1. Spawn call graph — everything funnels into two engine functions

Verified by direct rel32 xref scan of the whole `.text` (0x1000..0x7764000) plus disassembly of every node:

```
UGameplayLib::SpawnNPCPawn            (05E3E3AC) ─┐
UGameplayLib::SpawnNPCPawnWithParams  (05E3E834) ─┤ (exec stub only)
   └─ SpawnNPCPawnWithParams_Native   (01504C8C) ─┤
UGameplayLib::SpawnNPCPawnForParent   (05E3E4F8) ─┤ → UGameplayStatics::BeginDeferredActorSpawnFromClass (014DA498)
UGameplayLib::SpawnPawnFromClass      (05E3EB1C) ─┤        │
UGameplayLib::SpawnSavableActor       (05E3F130) ─┤        └→ UWorld::SpawnActorDeferred<AActor> (014DB418)
UGameplayLib::RestoreSavableActor     (05E39574) ─┤              │
UGameplayLib::SpawnPickups            (015036D8) ─┤              ▼
UGameplayLib::SpawnJumpTarget         (05E3DD9C) ─┤     UWorld::SpawnActor(UClass*,FTransform const*,FActorSpawnParameters const&)  ← 014DB504  *** ULTIMATE FUNNEL ***
UMissionLib::SpawnMission_Internal    (016C3EC0) ─┤              ▲   ▲   ▲
UMapLib::SpawnTemporaryLocation       (05E84FC0) ─┤              │   │   └── UWorld::SpawnActorAbsolute (04CD0204)  ← replication
SpawnPOI                              (05E6B914) ─┤              │   │            ├── UPackageMapClient::SerializeNewActor  (04D9FB88)  [legacy repl]
AESGameModeBase::SpawnChallenges/Perk/ActiveMissions ┘           │   │            └── UNetActorFactory::InstantiateReplicatedObjectFromHeader (04D6CA6C) [Iris]
BP "SpawnActor" node (execBeginDeferredActorSpawnFromClass / execBeginSpawningActorFromBlueprint)  │
                                                                 │   └── UWorld::SpawnActor(UClass*,FVector*,FRotator*,params) (01518E24)
ADockableStation::SpawnShip           (05DDAC58) ────────────────┘             ← UAIBlueprintHelperLibrary::SpawnAIFromClass (058EF62C)
UGameplayLib::SpawnPlayerShip         (05E3EE5C) ────────────────┘
AESGameModeBase::SpawnDefaultPawnAtTransform_Impl (05DEC54C) ────┘
```

Group / gizmo layer (all ES2 location population):
```
AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors (05DE9610)
   ├─ ULocationLib::SpawnOrRestoreLocationActors (05E6A52C) ─→ ULocationLib::SpawnGroupAtGizmo (01959A6C)
   ├─ AESGameModeBase::SpawnActiveMissions (05DEB8B4)                    │
   ├─ AESGameModeBase::SpawnPerks (0178E11C)                             ▼
   └─ AESGameModeBase::SpawnChallenges (05DEBDF8)     ULocationLib::SpawnGroup_Internal (0150BE40)
AESGameModeBase::Tick (0137CAE0) → ULocationLib::TickBusyness (015076B0)  │
   → TickBusyness_Internal (0150773C) → SpawnGroupAtGizmo ────────────────┤
AWantedLevelManager::SpawnAndCreateGroupSaveState / ABattleSimulator::SpawnGroupFromFaction ─┤
                                                                          ▼
                                            ULocationLib::SpawnActor_Internal (015062C8)
                                                ├─ ASpawnGizmoBase::SpawnClass (0150441C, BlueprintNativeEvent, vtbl +0x728)
                                                │     ├─ ASpawnGizmoBase::SpawnClass_Implementation (015044F4) ─→ SpawnNPCPawnWithParams_Native
                                                │     ├─ ABattleSimulator::SpawnClass_Impl (05DD8C7C) ─→ base impl
                                                │     ├─ AWantedLevelManager::SpawnClass_Impl (05E3D588) ─→ base impl
                                                │     └─ ANpcDockingPoint::SpawnClass_Impl (05DD92F8) ─→ UGameplayLib::SpawnNPCPawn
                                                └─ UGameplayLib::SpawnNPCPawnWithParams_Native (01504C8C)
```

**Narrowest ES2-level choke: `UGameplayLib::SpawnNPCPawnWithParams_Native` (01504C8C)** — every gizmo/group/savable NPC spawn goes through it (5 callers, all verified). It does NOT catch `SpawnNPCPawn` (used by `ANpcDockingPoint`), `SpawnPawnFromClass`, or `ADockableStation::SpawnShip`.

**Narrowest engine-level choke: `UWorld::SpawnActor` (014DB504)** — literally every actor creation in the process, including replication.

### 2. bRemoteOwned is a reliable discriminator (proven)

`FActorSpawnParameters` is 128 bytes; the bitfield byte is at **+0x32**, `bRemoteOwned = bit0 (mask 0x01)`, `bNoFail = bit1`, `bDeferConstruction = bit2`.

Proof of *set*: `UPackageMapClient::SerializeNewActor` builds its params at `[rbp+0x80]` and at `04DA06B5` does `or r9b, 0x3` → `mov byte ptr [rbp+0xb2], r9b` (0xb2 − 0x80 = 0x32), i.e. `bRemoteOwned=1, bNoFail=1`; `ObjectFlags = 8 (RF_Transient)` at +0x34; `OverrideLevel` at +0x20; `Template`=archetype at +0x08.
The Iris path `UNetActorFactory::InstantiateReplicatedObjectFromHeader` does the identical thing at `04D6CDAF` (`or al,0x3` → `mov byte ptr [rbp-0xe], al`, params base `rbp-0x40`).

Proof of *use*: in `UWorld::SpawnActor` at `014DB7A9..014DB7E5`, `rdi` = `FActorSpawnParameters*` and the arguments to `AActor::PostSpawnInitialize(Transform, Owner, Instigator, bRemoteOwned, bNoFail, bDeferConstruction, ScaleMethod)` are built as `[rsp+0x20] = [rdi+0x32] & 1`, `[rsp+0x28] = ([rdi+0x32]>>1)&1`, `[rsp+0x30] = ([rdi+0x32]>>2)&1`.

Counter-check: `ADockableStation::SpawnShip` sets only `bDeferConstruction` (`or al,0x4` at 05DDACC6, base rbp-0x80); no gameplay path in ES2 sets bit0. Only three engine callers reach `SpawnActorAbsolute`: the two replication paths (both set bRemoteOwned) and Sequencer spawnables (`FLevelSequenceActorSpawner::SpawnObject`, `UMovieSceneSpawnableActorBindingBase::SpawnObjectInternal`) which do **not** — so a blanket block on !bRemoteOwned would also kill cutscene spawnables. Keep the class filter tight.

**Important simplification:** replication never passes through `UGameplayStatics::BeginDeferredActorSpawnFromClass` (014DA498). On a client, *every* call to that function is locally-initiated gameplay/Blueprint code — no bRemoteOwned test needed there, and `UGameplayStatics::FinishSpawningActor` (014DA88C) explicitly null-checks its actor (`test rcx,rcx; je` at 014DA895), so returning `nullptr` from the Begin half is contractually safe. Same for `SpawnNPCPawnWithParams_Native` (has a clean `xor ebx,ebx` null-return path at 01504D65) and for the two direct `UWorld::SpawnActor` users (`ADockableStation::SpawnShip` null-checks at 05DDADD2; `SpawnAIFromClass` null-checks at 058EF894).

### 3. AI — ES2 preserves the stock NM_Client guard

`AESPawn::PostInitializeComponents` (01515EEC) calls `APawn::PostInitializeComponents` (016AC814) and adds nothing AI-related. The stock guard is intact:
```
016AC84E cmp byte [rdi+0x2B8],0     ; AutoPossessPlayer
016AC85E cmp byte [rdi+0x2B9],0     ; AutoPossessAI != Disabled
016AC86B cmp qword [rdi+0x2D8],0    ; Controller == null
016AC8C0 call UWorld::InternalGetNetMode
016AC8C5 cmp eax,3                  ; NM_Client
016AC8C8 je  skip                   ; <-- clients never auto-spawn the AI controller
016AC8CA mov cl,[rdi+0x2B9]         ; AutoPossessAI
016AC8D0 mov al,[rbp+0x13D]         ; World->bStartup
016AC8D6 cmp cl,3                   ; PlacedInWorldOrSpawned
016AC8E5 call [rax+0x830]           ; vtable slot 262 = APawn::SpawnDefaultController
```
`AController::Possess` (016ACD68) also refuses when `Role != ROLE_Authority` (`test byte [rcx+0x338],0x4` = bCanPossessWithoutAuthority, else `cmp byte [rcx+0x168],3`).

So a client with a proper `NM_Client` world will not create AI controllers for locally-existing pawns — **except** through `UAIBlueprintHelperLibrary::SpawnAIFromClass` (058EF62C), which spawns the pawn, then calls `[vtbl+0x830]` (SpawnDefaultController) **unconditionally**, then `[vtbl+0x8B0]` (`AAIController::RunBehaviorTree`) — no net-mode check at all. That is the one AI hole.

AI chain once a controller exists: `AAiControllerBase::OnPossess` (016ACE54) → `AAIController::OnPossess` (016C6CEC), `AAIController::UseBlackboard` (016AD1D4), then `[this+0x8B0]` = `AAIController::RunBehaviorTree` (016AD0E8) → `NewObject<UBehaviorTreeComponent>` + `RegisterComponent` + `UBehaviorTreeComponent::StartTree` (016AD290). Execution then lives in `UBehaviorTreeComponent::TickComponent` (01262A80). Note `UBehaviorTreeComponent::StartLogic` (016AE270) does **not** go through `StartTree`, so `StartTree` alone is not a complete BT block; `TickComponent` is.

`AESPawn::SetBlackboardValue*/Get*/Clear*`, `OnReceivedAIEvent` (01422710), `RegisterAttackingAi` (0130F2CC), `CountAttackingNPCs` (01510B3C), `RefreshPlayerEnemyList` (05DE7B7C) are all leaf consumers called *by* the BT/controller — hooking them cripples AI without stopping it. Don't.

`UConductorComponent` is electricity-beam conduction (`SpawnParticleSystemTo`, `UpdateBeamColor`), nothing to do with AI conducting — leave alone.

`AGridPathAIController` / `ADetourCrowdAIController` are stock engine classes with no ES2 overrides; `AAiControllerBase : AAIController` is the ES2 one.

### 4. Where the client's simulation actually comes from

ES2 is heavily GameMode-centric: `AESGameModeBase::GetManagerFromGameMode` (0121D814) resolves managers as **child actors of the GameMode** via `UWorld::GetAuthGameMode`, and `AESGameModeBase::GetESGameMode` (01505960) goes through `UGameplayStatics::GetGameMode` (→ `UWorld::AuthorityGameMode`, +0x158). On a true UE client `AuthorityGameMode == nullptr`, so `AMapEventManager`, `AWantedLevelManager`, `ABattleSimulator`, `ASequenceManager`, `TickBusyness`, `SpawnOrRestoreLocationActors`, `SpawnWingmen`, `SpawnActiveMissions/Perks/Challenges`, `CheckForInvasions`, `CheckForWorldLevelingEvent` all cannot run there.

What *does* run on a client:
* **`ADockableStation::BeginPlay` (05D92604)** — a level-placed actor — calls `SpawnShipsForSale` (05DDAF94) and `SpawnPlayerShips` (05DDA8F4) → `SpawnShip` (05DDAC58) → **`UWorld::SpawnActor` directly**. This is a confirmed client-side `AESPawn` spawner that bypasses every `UGameplayLib::Spawn*` entry point.
* **`ADynamicJobManager::BeginPlay` (05DE07E0)** and **`ASequenceBase::BeginPlay` (05EC4DAC)** reach spawn funnels at depth 3–4 (job offers / sequence actors; ASequenceBase gates on `GetManagerFromGameMode`, so it degrades to a no-op on a client).
* Any **Blueprint** on a level actor that calls `SpawnNPCPawn*`, `SpawnPawnFromClass`, `SpawnAIFromClass`, or the K2 SpawnActor node.
* Level-placed NPC pawns themselves (see §5).

If the parent observes full SP simulation on the client, the first thing to check at runtime is whether `GetWorld()->AuthorityGameMode` (UWorld+0x158) is non-null on the client — if it is, the client is not in a real NM_Client world and the whole GameMode layer must be gated too (hooks P2 below).

### 5. Level-placed actors on a client (`bNetStartup`)

`ULevel::InitializeNetworkActors` (04CC30C8) decoded:
```
NetMode = World->InternalGetNetMode()
for each Actor:
  if (!Actor->bActorSeamlessTraveled && !Actor->bActorInitialized):     // 0x5B bit2, 0x5C bit1
      if (Actor->bNetLoadOnClient)                                       // 0x59 bit3
          Actor->bNetStartup = true                                      // 0x58 bit1
          for each owned component: comp[0x8B] |= 0x80                   // bIsNetStartupComponent
      if (NetMode == NM_Client):
          if (Actor->bNetLoadOnClient) Actor->ExchangeNetRoles(true)      // 014DC3F0
          else                          Actor->Destroy(true)              // 014DA5D4
  Actor->bActorSeamlessTraveled = false
```
Consequences on the client, with `AActor::Role` at **+0x168**, `RemoteRole` at **+0x60**, `bReplicates` at **0x5B bit3** (ENetRole: None=0, SimulatedProxy=1, AutonomousProxy=2, Authority=3):
* level actor with `bReplicates=true` → Role 3/Remote 1 swapped to **Role=ROLE_SimulatedProxy(1)**, bound to the server's copy by its static path-name NetGUID.
* level actor with `bReplicates=false` → Role 3/Remote 0 swapped to **Role=ROLE_None(0)**; it stays a purely local actor and runs its own BeginPlay/Tick.
* runtime-spawned local actor → **Role=ROLE_Authority(3)** (unless bRemoteOwned, in which case `AActor::PostSpawnInitialize` exchanges roles).

So `Role == ROLE_Authority` is the clean runtime test for "this actor is my own local simulation". And the mod must flip `bReplicates` on the **CDO / class default** before `InitializeNetworkActors` runs, otherwise level-placed NPCs will not role-exchange and the client's copies will never bind to the host's — you get duplicates by construction.

Destroying the client's copies is safe for level actors that are *not* net-startup-bound (Role==ROLE_None); destroying a Role==ROLE_SimulatedProxy net-startup actor is not (the server's channel expects it). Preferable to preventing BeginPlay wholesale: `AActor::DispatchBeginPlay` (01569648) is the choke, but skipping it leaves `HasActorBegunPlay()` false and breaks EndPlay/component activation — hook the specific ES2 functions instead (`ADockableStation::SpawnShipsForSale`).

Useful cleanup primitives (both reflected UFunctions, callable via ProcessEvent):
* `ULocationLib::EvictAllNPCs(WorldCtx, bPushSuppressBusyness, FModifierHandle&)` @ **05E5AA24** — enumerates `USelfRegisteringComponent::GetAllActorsOfRegister(ERegisterIds::Pawn)`, calls `UJumpDriveComponent::ForceImmediateJump` on each, and optionally `UGameplayLib::PushSuppressLocationBusyness`.
* `UGameplayLib::PushSuppressLocationBusyness(WorldCtx) -> FModifierHandle` @ **05E37964**.
* `USelfRegisteringComponent::GetAllActorsOfRegister(ERegisterIds::Type)` @ **0150FDA8** — `ERegisterIds::Pawn = 2` (also Pickup=9, PointOfInterest=7, SpawnGizmo=16, Mission=14, DockingPoint=12, HUDMarkerComponent=8).

### 6. Must NOT be suppressed on the client
`UWeaponComponent::SpawnWeaponInstancesForSlot` (01518904) — every pawn, including replicated ones, needs its weapon actors; `UActorPool::CreateActor` (014DA668) — projectiles are pooled `AProjectileBase : AActor, IPoolableActor`; `ULootDropComponent::DropLoot` (016CCDC4) and `UGameplayLib::SpawnPickups/SpawnPickupFromItem[ID]` — `APickupBase : AActor`; `UDeviceComponent::SpawnDevice/SpawnDeviceFromItem/RespawnDevices/ReinitDeviceSlot/InstallNewDevice` and `UConsumableComponent::UseConsumable` — `ADeviceBase : AEquipmentBase : AActor`; all `UMissionLib::Spawn*` / `GetMissionLog_Internal` / `GetCurrentObjectivesForMissionLog` / `CreateJobOffer` / `UGameplayLib::SetPinForLocation` — these spawn `AMissionBase : AMissionTaskBase` purely to read text for the client's HUD/mission log; `ADockableStation::SpawnPlayerShips`/`SpawnPlayerShipOfIndex` — the client's own hangar/ship collection is per-player; `APlayerController::SpawnDefaultHUD / SpawnPlayerCameraManager / SpawnSpectatorPawn`, `AESPlayerController::BeginPlay`, `AGameplayCameraSystemActor::GetAutoSpawnedCameraSystemActor`, `APlayerCameraManager::AddGenericCameraLensEffect`; `UChildActorComponent::CreateChildActor` (0188EE38); Sequencer spawnables (cutscene actors — they reach `SpawnActorAbsolute` without bRemoteOwned); `AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation` (05DEC54C, host-side only). None of these classes derive from `AESPawn`, so an `IsChildOf(AESPawn)` filter already excludes all of them except the DockableStation player ships, which need an explicit allow.

### 7. Class filter to use
`Class->IsChildOf(AESPawn::StaticClass())` via `UStruct::IsChildOf` @ **013ACDD0** (RCX=UStruct* derived, RDX=UStruct* base; `UObjectBase::ClassPrivate` is at +0x10), **minus** classes whose CDO has `bIsPlayerPawn` set — `AESPawn::bIsPlayerPawn` is at **+0x10C1** (`UClass::GetDefaultObject(bool)` @ **0125D334**). `AESPawn` is sizeof 4912, bases `APawn`(0), `ISavableInterface`(808), `INavTargetInterface`(816); it is the only ES2 C++ pawn class ("ESPawn" UClass), all NPC ships are BP subclasses of it.

## Key functions

- **UWorld::SpawnActor** @ `014DB504` unique=True — The single master actor-spawn implementation. Reads SpawnParams+0x32 to derive bRemoteOwned/bNoFail/bDeferConstruction and passes them to AActor::PostSpawnInitialize (014DBE28) at 014DB7E5. Also calls UWorld::AddNetworkActor (014DBB08) and ULevel::TryAddActorToList.
  - `public: class AActor * __cdecl UWorld::SpawnActor(class UClass *, struct UE::Math::TTransform<double> const *, struct FActorSpawnParameters const &)`
  - ABI: MSVC x64 thiscall: RCX=UWorld* this, RDX=UClass* Class, R8=FTransform const* (may be null), R9=FActorSpawnParameters const* (reference => pointer). Returns AActor* in RAX. Every other overload/template forwards here: 01518E24 (FVector/FRotator) at 01518EAF, 04CD0204 (SpawnActorAbsolute) at 04CD0C84,
- **UGameplayStatics::BeginDeferredActorSpawnFromClass** @ `014DA498` unique=True — Deferred-spawn front door. 29 direct callers, essentially every ES2 gameplay spawn plus the Blueprint SpawnActor node (execBeginDeferredActorSpawnFromClass, execBeginSpawningActorFromBlueprint). Calls UObjectBaseUtility::IsA<APawn>, UEngine::GetWorldFromContextObject, then UWorld::SpawnActorDeferred<AActor>. REPLICATION NEVER REACHES THIS FUNCTION.
  - `public: static class AActor * __cdecl UGameplayStatics::BeginDeferredActorSpawnFromClass(class UObject const *, class TSubclassOf<class AActor>, struct UE::Math::TTransform<double> const &, enum ESpawnActorCollisionHandlingMethod, class AActor *, enum ESpawnActorScaleMethod)`
  - ABI: static __cdecl: RCX=UObject const* WorldContextObject, RDX=TSubclassOf<AActor>* (BY HIDDEN POINTER - it is copy-constructed by the callee via TSubclassOf ctor at 142B3A25C; deref +0x0 for the UClass*), R8=FTransform const*, R9B=ESpawnActorCollisionHandlingMethod, [rsp+0x20]=AActor* Owner, [rsp+0x28]
- **UGameplayLib::SpawnNPCPawnWithParams_Native** @ `01504C8C` unique=True — The narrowest ES2 NPC choke. BeginDeferredActorSpawnFromClass -> IsChildOf(AESPawn) -> writes NPCLevel(+0x1038)=Params.NPCLevel+Params.LevelOffset, NPCMark(+0x103C)=Params.Mark, bJumpIn(+0xE70)=Params.bJumpIn, JumpInDelay(+0xE74) -> FindComponentByClass<UFactionComponent> + SetFaction -> FinishSpawningActor. 5 callers: ULocationLib::SpawnActor_Internal, UGameplayLib::SpawnSavableActor, SpawnNPCPaw
  - `public: static class AESPawn * __cdecl UGameplayLib::SpawnNPCPawnWithParams_Native(class UObject const *, class TSubclassOf<class AActor>, struct UE::Math::TTransform<double> const &, struct FSpawnParameter const &)`
  - ABI: static __cdecl: RCX=UObject const* WorldCtx, RDX=TSubclassOf<AActor>* (hidden ptr), R8=FTransform const*, R9=FSpawnParameter const* (48-byte struct by reference). Returns AESPawn* in RAX; has a clean null-return path at 01504D65.
- **UGameplayLib::SpawnNPCPawn** @ `05E3E3AC` unique=True — Does NOT go through SpawnNPCPawnWithParams_Native. BeginDeferredActorSpawnFromClass -> IsChildOf(AESPawn) -> sets +0x103C/+0xE70/+0x1038 -> FinishSpawningActor. Callers: execSpawnNPCPawn (BP) and ANpcDockingPoint::SpawnClass_Implementation (05DD9BCC).
  - `public: static class AESPawn * __cdecl UGameplayLib::SpawnNPCPawn(class UObject const *, class TSubclassOf<class AActor>, struct UE::Math::TTransform<double> const &, int NPCLevel, int NPCMark, bool bJumpIn)`
  - ABI: RCX=WorldCtx, RDX=TSubclassOf* (hidden ptr), R8=FTransform const*, R9D=int NPCLevel, [rsp+0x28]=int NPCMark, [rsp+0x30]=bool bJumpIn. Returns AESPawn*.
- **UGameplayLib::SpawnNPCPawnWithParams** @ `05E3E834` unique=True — BP wrapper: fills in level/variation/mark defaults (UMapLib::GetCurrentLocationLevel, GetLevelVariationForNPCSpawns, GetNPCMark) then calls SpawnNPCPawnWithParams_Native at 05E3EA74.
  - `public: static class AESPawn * __cdecl UGameplayLib::SpawnNPCPawnWithParams(class UObject const *, class TSubclassOf<class AActor>, struct UE::Math::TTransform<double> const &, struct FSpawnParameter const &)`
  - ABI: Same shape as _Native. Only caller is execSpawnNPCPawnWithParams (05B9EE75), i.e. Blueprint-only.
- **UGameplayLib::SpawnNPCPawnForParent** @ `05E3E4F8` unique=True — Spawns a child NPC (drone/wingman) inheriting level/faction from the parent pawn. Reads parent fields incl. +0x10C4 (float).
  - `public: static class AESPawn * __cdecl UGameplayLib::SpawnNPCPawnForParent(class UObject const *, class AESPawn *, class TSubclassOf<class AActor>, struct UE::Math::TTransform<double> const &, struct FSpawnParameter)`
  - ABI: RCX=WorldCtx, RDX=AESPawn* Parent, R8=TSubclassOf* (hidden ptr), R9=FTransform const*, [rsp+0x28]=FSpawnParameter* (48-byte by-value => HIDDEN POINTER). Only caller is execSpawnNPCPawnForParent, i.e. Blueprint-only. Calls SpawnNPCPawnWithParams_Native at 05E3E67F.
- **UGameplayLib::SpawnPawnFromClass** @ `05E3EB1C` unique=True — Plain BP pawn spawn via BeginDeferredActorSpawnFromClass at 05E3EB50 + FinishSpawningActor.
  - `public: static class AESPawn * __cdecl UGameplayLib::SpawnPawnFromClass(class UObject const *, class TSubclassOf<class AActor>, struct UE::Math::TTransform<double> const &)`
  - ABI: RCX=WorldCtx, RDX=TSubclassOf* (hidden ptr), R8=FTransform const*. Blueprint-only caller (execSpawnPawnFromClass at 05B9F2D8).
- **ULocationLib::SpawnActor_Internal** @ `015062C8` unique=True — Per-actor spawn inside a spawn group: resolves the BP class (LoadClass<AActor>), checks progress tokens, then either ASpawnGizmoBase::SpawnClass (0150441C, virtual dispatch) at 01506BBD or UGameplayLib::SpawnNPCPawnWithParams_Native at 01506A71. Callers: SpawnGroup_Internal (0150C22B) and SpawnActorAtGizmo (05E6A432).
  - `protected: static struct FActorSaveState __cdecl ULocationLib::SpawnActor_Internal(class ASpawnGizmoBase *, struct UE::Math::TTransform<double> const *, enum ESpawnGroup::Type, enum ESpawnGroupPool, enum ESpawnMethod::Type, struct FSpawnParameter const &, struct FRandomStream &, class UObject *, float)`
  - ABI: Returns FActorSaveState (sizeof=320) => SRET. RCX = FActorSaveState* hidden return, RDX = ASpawnGizmoBase*, R8 = FTransform const*, R9D = ESpawnGroup::Type, [rsp+0x28]=ESpawnGroupPool (byte), [rsp+0x30]=ESpawnMethod::Type, [rsp+0x38]=FSpawnParameter const*, [rsp+0x40]=FRandomStream*, [rsp+0x48]=UObj
- **ULocationLib::SpawnGroup_Internal** @ `0150BE40` unique=True — Builds a whole NPC group; loops calling SpawnActor_Internal. Callers: SpawnEmptyGroup (0150B339), SpawnGroupAtTransform (01835D2C), SpawnGroupAtGizmo (01959AF3).
  - `protected: static struct FSpawnGroupSaveState __cdecl ULocationLib::SpawnGroup_Internal(class ASpawnGizmoBase *, struct UE::Math::TTransform<double> const *, class FName const &, enum ESpawnGroup::Type, enum ESpawnGroup::Type, enum ESpawnMethod::Type, int, int, int, struct FRandomStream &, class UObject *, float, bool, bool, struct FSpawnParameter const *)`
  - ABI: Returns FSpawnGroupSaveState (sizeof=64) => SRET in RCX; all declared args shift right by one register.
- **ASpawnGizmoBase::SpawnClass** @ `0150441C` unique=True — Virtual spawn entry for all gizmo subclasses. Overrides: ASpawnGizmoBase::SpawnClass_Implementation (015044F4) -> SpawnNPCPawnWithParams_Native; ABattleSimulator (05DD8C7C) and AWantedLevelManager (05E3D588) -> base impl; ANpcDockingPoint (05DD92F8) -> UGameplayLib::SpawnNPCPawn.
  - `public: class AActor * __cdecl ASpawnGizmoBase::SpawnClass(class TSubclassOf<class AActor>, enum ESpawnGroup::Type, struct FSpawnParameter, struct FRandomStream &)`
  - ABI: BlueprintNativeEvent thunk. RCX=this, RDX=TSubclassOf* (hidden ptr), R8D=ESpawnGroup::Type, R9=FSpawnParameter* (48-byte by-value => HIDDEN POINTER; callee copies it), [rsp+0x28]=FRandomStream*. Dispatches via UObject::FindFunctionChecked(NAME_ASpawnGizmoBase_SpawnClass); if the found UFunction's ow
- **ADockableStation::SpawnShipsForSale** @ `05DDAF94` unique=True — Called from ADockableStation::BeginPlay at 05D927CC (level-placed actor => runs on CLIENTS). Loops calling ADockableStation::SpawnShip which spawns AESPawn via UWorld::SpawnActor directly, bypassing all UGameplayLib entry points. Also called from RespawnShipsForSaleIfAlreadySpawned (05DD67D4).
  - `private: void __cdecl ADockableStation::SpawnShipsForSale(void)`
  - ABI: RCX=ADockableStation* this, void return - trivially safe to no-op.
- **ADockableStation::SpawnShip** @ `05DDAC58` unique=True — Spawns a ship pawn at a station (for-sale stock and the player's stored hangar ships) via UWorld::SpawnActor at 05DDADCA, sets FShipData at +0x4B8, USaveGameComponent::SetSpawnMethod.
  - `private: void __cdecl ADockableStation::SpawnShip(class TSubclassOf<class AESPawn>, struct FShipData *, struct UE::Math::TTransform<double> const &, bool, bool, int)`
  - ABI: RCX=this, RDX=TSubclassOf* (hidden ptr), R8=FShipData*, R9=FTransform const*, stack: bool,bool,int. Sets spawn params bDeferConstruction only (or al,0x4 at 05DDACC6 into params+0x32) - bRemoteOwned is NOT set, so the bRemoteOwned discriminator classifies it correctly as local. Null-checks the SpawnA
- **ADockableStation::SpawnPlayerShips** @ `05DDA8F4` unique=True — Spawns the local player's owned ships in the hangar. This is per-player client-side content - do NOT blanket-block it or the client loses its hangar/ship dealer.
  - `private: void __cdecl ADockableStation::SpawnPlayerShips(bool)`
  - ABI: RCX=this, DL=bool. Called from ADockableStation::BeginPlay at 05D927DE.
- **ADockableStation::BeginPlay** @ `05D92604` unique=True — Level-actor BeginPlay that runs on both host and client. HUD indicators, then SpawnShipsForSale (05D927CC) and SpawnPlayerShips (05D927DE).
  - `public: virtual void __cdecl ADockableStation::BeginPlay(void)`
  - ABI: RCX=this. Note ASpawnGizmoBase::BeginPlay / APOISpawner::BeginPlay / ASpawnGizmoSelector::BeginPlay are all ICF-folded to 02B469E8 (uniq=15) - NEVER hook that RVA; they are trivial and spawn nothing.
- **UAIBlueprintHelperLibrary::SpawnAIFromClass** @ `058EF62C` unique=True — THE AI HOLE ON CLIENTS. UWorld::SpawnActor (01518E24 overload) at 058EF88C, then if Pawn->Controller (+0x2D8) is null calls vtable [+0x830] = APawn::SpawnDefaultController UNCONDITIONALLY (no NM_Client check), then vtable [+0x8B0] = AAIController::RunBehaviorTree. Any Blueprint using the 'Spawn AI From Class' node creates a fully-thinking NPC on a client.
  - `public: static class APawn * __cdecl UAIBlueprintHelperLibrary::SpawnAIFromClass(class UObject *, class TSubclassOf<class APawn>, class UBehaviorTree *, struct UE::Math::TVector<double>, struct UE::Math::TRotator<double>, bool, class AActor *)`
  - ABI: RCX=WorldCtx, RDX=TSubclassOf* (hidden ptr), R8=UBehaviorTree*, R9=FVector* (24-byte by value => hidden ptr), stack: FRotator*, bool bNoCollisionFail, AActor* Owner. Null-checks the spawned pawn at 058EF894.
- **APawn::PostInitializeComponents** @ `016AC814` unique=True — Stock UE. GUARD IS PRESERVED: at 016AC8C0 calls UWorld::InternalGetNetMode (01287CF0), at 016AC8C5 'cmp eax,3' (NM_Client) and at 016AC8C8 'je' skips the SpawnDefaultController call at 016AC8E5 (vtable +0x830). Also requires AutoPossessAI(+0x2B9) != Disabled and Controller(+0x2D8)==null. So clients never auto-create AI controllers.
  - `public: virtual void __cdecl APawn::PostInitializeComponents(void)`
  - ABI: RCX=this. ES2 does NOT override the controller logic - AESPawn::PostInitializeComponents (01515EEC) calls this as Super at 01515F1D and only does bounds/weapon-visibility/aiming-noise setup afterwards.
- **APawn::SpawnDefaultController** @ `016ACA70` unique=True — Spawns AIControllerClass (+0x2C0) and possesses the pawn. Reachable on a client only via SpawnAIFromClass or a BP calling the reflected UFunction.
  - `public: virtual void __cdecl APawn::SpawnDefaultController(void)`
  - ABI: RCX=this, void return. ZERO direct rel32 xrefs - only reached through vtable slot 262 (offset 0x830). ES2 declares no override (no AESPawn::SpawnDefaultController symbol) and it is not a BlueprintNativeEvent, so hooking the base RVA does intercept every ES2 pawn.
- **AController::Possess** @ `016ACD68` unique=True — Stock UE possession with an authority guard. A locally spawned client controller has Role=ROLE_Authority so the guard does not save you there.
  - `public: virtual void __cdecl AController::Possess(class APawn *)`
  - ABI: RCX=this, RDX=APawn*. Guard at 016ACD8E: 'test byte [rcx+0x338],0x4' (bCanPossessWithoutAuthority) else 'cmp byte [rcx+0x168],3' (Role==ROLE_Authority) -> cold-path warn-and-return. Then dispatches vtable [+0x7D8] = OnPossess.
- **AAiControllerBase::OnPossess** @ `016ACE54` unique=True — Calls AAIController::OnPossess (016C6CEC), binds faction-changed delegates on UFactionComponent, AAIController::UseBlackboard (016AD1D4), then calls vtable [this+0x8B0] = AAIController::RunBehaviorTree, then AESPawn::AreControlsDisabled. This is the ES2 'start thinking' entry point.
  - `public: virtual void __cdecl AAiControllerBase::OnPossess(class APawn *)`
  - ABI: RCX=this, RDX=APawn*, void return. AAiControllerBase : AAIController (sizeof 1040).
- **AAIController::RunBehaviorTree** @ `016AD0E8` unique=True — Creates the UBehaviorTreeComponent via NewObject (016AD6EC), RegisterComponent (0152990C), then UBehaviorTreeComponent::StartTree (016AD290).
  - `public: virtual bool __cdecl AAIController::RunBehaviorTree(class UBehaviorTree *)`
  - ABI: RCX=this, RDX=UBehaviorTree*, returns bool in AL. Vtable slot 278 (offset 0x8B0) of AAIController. AAiControllerBase does not override it, so hooking the base RVA is valid.
- **UBehaviorTreeComponent::TickComponent** @ `01262A80` unique=True — Drives all behaviour-tree execution. Complete backstop for AI: no tick, no thinking. Note UBehaviorTreeComponent::StartLogic (016AE270) does NOT route through StartTree (016AD290), so StartTree alone is not a sufficient block.
  - `public: virtual void __cdecl UBehaviorTreeComponent::TickComponent(float, enum ELevelTick, struct FActorComponentTickFunction *)`
  - ABI: RCX=this, XMM1=float DeltaTime, R8D=ELevelTick, R9=FActorComponentTickFunction*.
- **UBehaviorTreeComponent::StartTree** @ `016AD290` unique=True — Stops any running tree (StopTree 01425260) and starts the new asset (ProcessPendingInitialize 01425998).
  - `public: void __cdecl UBehaviorTreeComponent::StartTree(class UBehaviorTree &, enum EBTExecutionMode::Type)`
  - ABI: RCX=this, RDX=UBehaviorTree& (pointer), R8D=EBTExecutionMode::Type.
- **AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors** @ `05DE9610` unique=True — Single kill-switch for the whole ES2 location population: ULocationLib::ResetMissionGroupSavedOrRestoredFlags, ULocationLib::SpawnOrRestoreLocationActors (05E6A52C), AESGameModeBase::SpawnActiveMissions (05DEB8B4), SpawnPerks (0178E11C), SpawnChallenges (05DEBDF8), then a BP ProcessEvent.
  - `private: void __cdecl AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors(void)`
  - ABI: RCX=this, void return. Only reachable via its exec stub (05AF13A0), i.e. called from Blueprint (GameMode BP / level BP).
- **ULocationLib::SpawnOrRestoreLocationActors** @ `05E6A52C` unique=True — Walks all spawn gizmos in the location and calls ULocationLib::SpawnGroupAtGizmo (05E6B302). Sole caller: AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors.
  - `public: static void __cdecl ULocationLib::SpawnOrRestoreLocationActors(class UObject *, struct FRandomStream &)`
  - ABI: RCX=UObject* WorldCtx, RDX=FRandomStream* (8-byte struct by ref). void return.
- **ULocationLib::SpawnOrRestoreLocationPOIs** @ `05E6B838` unique=True — Calls the file-local SpawnPOI (05E6B914) which uses BeginDeferredActorSpawnFromClass to place asteroids/props/POI actors. APOISpawner is a passive marker actor (only ctor/BeginPlay(folded)/OnConstruction).
  - `public: static void __cdecl ULocationLib::SpawnOrRestoreLocationPOIs(class UObject const *)`
  - ABI: RCX=UObject const* WorldCtx. void return. Sole caller: AESGameModeBase::StartPlay at 02B3C512.
- **ULocationLib::TickBusyness** @ `015076B0` unique=True — Periodic ambient-traffic spawner: TickBusyness_Internal (0150773C) -> ULocationLib::SpawnGroupAtGizmo (01507BD2). Server-only in practice (GameMode Tick).
  - `public: static void __cdecl ULocationLib::TickBusyness(float, class UObject *)`
  - ABI: XMM0=float DeltaTime, RDX=UObject* WorldCtx (first arg is float so RCX is unused for the float in x64 - the float goes in XMM0 and RDX carries the second arg). Sole caller: AESGameModeBase::Tick at 0137CCA6.
- **AESGameModeBase::Tick** @ `0137CAE0` unique=True — AGameMode::Tick, time dilation, player stats, then ULocationLib::TickBusyness at 0137CCA6.
  - `public: virtual void __cdecl AESGameModeBase::Tick(float)`
  - ABI: RCX=this, XMM1=float DeltaSeconds.
- **ULocationLib::EvictAllNPCs** @ `05E5AA24` unique=True — CLEANUP PRIMITIVE. Enumerates USelfRegisteringComponent::GetAllActorsOfRegister(ERegisterIds::Pawn), skips child actors, calls UJumpDriveComponent::ForceImmediateJump on each, and optionally UGameplayLib::PushSuppressLocationBusyness.
  - `public: static void __cdecl ULocationLib::EvictAllNPCs(class UObject const *, bool, struct FModifierHandle &)`
  - ABI: RCX=UObject const* WorldCtx, DL=bool bPushSuppressBusyness, R8=FModifierHandle& (out). Reflected: Z_Construct_UFunction_ULocationLib_EvictAllNPCs @ 05C60AB4 - callable via ProcessEvent with a {UObject*, bool, FModifierHandle} param block.
- **UGameplayLib::PushSuppressLocationBusyness** @ `05E37964` unique=True — Pushes a suppression modifier that stops ambient busyness spawning.
  - `public: static struct FModifierHandle __cdecl UGameplayLib::PushSuppressLocationBusyness(class UObject const *)`
  - ABI: Returns FModifierHandle - check its size before calling natively; safest to invoke via ProcessEvent. Paired with UGameplayLib::RemoveSuppressLocationBusyness (05E39028).
- **USelfRegisteringComponent::GetAllActorsOfRegister** @ `0150FDA8` unique=True — Global actor registry lookup. ERegisterIds: INVALID=0, NONE=1, Pawn=2, ShipComponent=3, ExplosiveProp=4, Waypoint=5, Missile=6, PointOfInterest=7, HUDMarkerComponent=8, Pickup=9, Resource=10, JumpTarget=11, DockingPoint=12, Container=13, Mission=14, MissionTaskBase=15, SpawnGizmo=16, Perk=17, TravelModeLocation=18, TravelModePlanet=19, JumpGate=20, Mine=21, POISpawner=22, NavNode=23, SpawnGizmoSel
  - `public: static class TArray<class AActor *, class TSizedDefaultAllocator<32>> const & __cdecl USelfRegisteringComponent::GetAllActorsOfRegister(enum ERegisterIds::Type)`
  - ABI: RCX=ERegisterIds::Type (enum, int). Returns a TArray<AActor*> CONST REFERENCE in RAX (pointer to the live registry array - do not mutate; copy before destroying actors).
- **ULevel::InitializeNetworkActors** @ `04CC30C8` unique=True — Client-side level-actor fixup, decoded from disassembly: sets bNetStartup (+0x58 bit1) for bNetLoadOnClient (+0x59 bit3) actors and flags their components (+0x8B |= 0x80); then if NetMode==NM_Client, ExchangeNetRoles(true) for bNetLoadOnClient actors and Destroy(true) for the rest. Skipped for already-initialized (+0x5C bit1) or seamless-travelled (+0x5B bit2) actors.
  - `public: void __cdecl ULevel::InitializeNetworkActors(void)`
  - ABI: RCX=ULevel* this. Reads OwningWorld at ULevel+0xC0, actor array ptr at +0xA0 / count at +0xA8.
- **AActor::ExchangeNetRoles** @ `014DC3F0` unique=True — Turns Role=Authority into SimulatedProxy (replicated level actors) or ROLE_None (non-replicated level actors) on clients.
  - `public: void __cdecl AActor::ExchangeNetRoles(bool)`
  - ABI: RCX=this, DL=bRemoteOwner. Early-outs if bExchangedRoles (+0x59 bit2) already set; sets that bit; when DL!=0 it takes the cold path that swaps Role(+0x168) and RemoteRole(+0x60).
- **UPackageMapClient::SerializeNewActor** @ `04D9FB88` unique=True — THE client-side replicated-actor creator (legacy replication). This is the path a client-side spawn hook must never block.
  - `public: virtual bool __cdecl UPackageMapClient::SerializeNewActor(class FArchive &, class UActorChannel *, class AActor *&)`
  - ABI: Virtual; RCX=this, RDX=FArchive&, R8=UActorChannel*, R9=AActor*&. Builds FActorSpawnParameters at [rbp+0x80]: Name=0, Template=archetype, Owner/Instigator=0, OverrideLevel=Level, CollisionHandling=1, ScaleMethod=1, bitfield|=0x3 (bRemoteOwned|bNoFail) at 04DA06B5, NameMode=0, ObjectFlags=8 (RF_Trans
- **UNetActorFactory::InstantiateReplicatedObjectFromHeader** @ `04D6CA6C` unique=True — The Iris (UE5.5 new replication) equivalent of SerializeNewActor. Confirms bRemoteOwned is set on BOTH replication paths, so the discriminator is complete regardless of which replication system is active.
  - `public: virtual struct UNetObjectFactory::FInstantiateResult __cdecl UNetActorFactory::InstantiateReplicatedObjectFromHeader(struct UNetObjectFactory::FInstantiateContext const &, class UE::Net::FNetObjectCreationHeader const *)`
  - ABI: Builds its FActorSpawnParameters at [rbp-0x40] and sets the +0x32 bitfield to 0x3 at 04D6CDAF (or al,0x3 -> mov [rbp-0xe],al). Calls SpawnActorAbsolute at 04D6CE61.
- **UWorld::SpawnActorAbsolute** @ `04CD0204` unique=True — Replication + Sequencer spawn entry. Hooking THIS instead of UWorld::SpawnActor would be an alternative 'allow-list' point, but Sequencer shares it.
  - `public: class AActor * __cdecl UWorld::SpawnActorAbsolute(class UClass *, struct UE::Math::TTransform<double> const &, struct FActorSpawnParameters const &)`
  - ABI: RCX=UWorld*, RDX=UClass*, R8=FTransform const*, R9=FActorSpawnParameters const*. Converts the absolute transform to level-relative then calls UWorld::SpawnActor at 04CD0C84. Only 4 callers: SerializeNewActor, UNetActorFactory, FLevelSequenceActorSpawner::SpawnObject, UMovieSceneSpawnableActorBinding
- **AESGameModeBase::GetManagerFromGameMode** @ `0121D814` unique=True — Proves the ES2 manager architecture: UGameInstance::GetWorld -> UWorld::GetAuthGameMode<AESGameModeBase> -> AActor::GetAllChildActors. AMapEventManager / AWantedLevelManager / ABattleSimulator / ASequenceManager are CHILD ACTORS OF THE GAMEMODE, so they cannot exist on a real NM_Client world (UWorld::AuthorityGameMode at +0x158 is null).
  - `public: static class AActor * __cdecl AESGameModeBase::GetManagerFromGameMode(class TSubclassOf<class AActor>)`
  - ABI: RCX=TSubclassOf* (hidden ptr).
- **AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation** @ `05DEC54C` unique=True — Host-side player-pawn spawn (BP_Ship_Player_C). MUST NOT be blocked - runs on the host; the client receives the pawn through replication with bRemoteOwned set.
  - `public: virtual class APawn * __cdecl AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation(class AController *, struct UE::Math::TTransform<double> const &)`
  - ABI: RCX=this, RDX=AController*, R8=FTransform const*. Calls UWorld::SpawnActor at 05DEEDC1.
- **UWeaponComponent::SpawnWeaponInstancesForSlot** @ `01518904` unique=True — Spawns AWeaponBase actors for every pawn including replicated proxies. MUST NOT be blocked on the client or replicated NPCs will have no weapons/visuals.
  - `private: void __cdecl UWeaponComponent::SpawnWeaponInstancesForSlot(int)`
  - ABI: RCX=this, EDX=int slot.
- **UActorPool::CreateActor** @ `014DA668` unique=True — Projectile/effect pooling (AProjectileBase implements IPoolableActor at offset 680). Client-side weapon fire depends on it - do not block.
  - `private: class AActor * __cdecl UActorPool::CreateActor(void)`
  - ABI: RCX=this.
- **UWorld::InternalGetNetMode** @ `01287CF0` unique=True — Net-mode resolution used by every engine guard. Use this in detours to gate client-only behaviour.
  - `private: enum ENetMode __cdecl UWorld::InternalGetNetMode(void) const`
  - ABI: RCX=UWorld* this, returns ENetMode in EAX. NM_Standalone=0, NM_DedicatedServer=1, NM_ListenServer=2, NM_Client=3. Cheap inline alternative: World->NetDriver (UWorld+0x38) != null && NetDriver->ServerConnection (UNetDriver+0xF0) != null  =>  NM_Client.
- **UStruct::IsChildOf** @ `013ACDD0` unique=True — The class-filter primitive for the detours.
  - `public: bool __cdecl UStruct::IsChildOf(class UStruct const *) const`
  - ABI: RCX=UStruct* derived (a UClass* works), RDX=UStruct* base. Returns bool in AL. UObjectBase::ClassPrivate is at +0x10 so ObjectPtr->Class = *(UClass**)((u8*)obj+0x10).
- **UClass::GetDefaultObject** @ `0125D334` unique=True — Needed to read AESPawn::bIsPlayerPawn (+0x10C1) off a candidate class before the actor exists.
  - `public: class UObject * __cdecl UClass::GetDefaultObject(bool) const`
  - ABI: RCX=UClass* this, DL=bool bCreateIfNeeded. Returns the CDO.
- **AActor::Destroy** @ `014DA5D4` unique=True — Cleanup primitive for removing client-local duplicate NPCs (only for actors whose Role (+0x168) == ROLE_Authority(3) or ROLE_None(0); never destroy a ROLE_SimulatedProxy net-startup actor).
  - `public: bool __cdecl AActor::Destroy(bool, bool)`
  - ABI: RCX=this, DL=bNetForce, R8B=bShouldModifyLevel.
- **AActor::DispatchBeginPlay** @ `01569648` unique=True — Global BeginPlay choke. Usable to neutralise whole level-actor classes on a client, but skipping it leaves HasActorBegunPlay()==false and breaks EndPlay/component activation - prefer targeted hooks.
  - `public: void __cdecl AActor::DispatchBeginPlay(bool)`
  - ABI: RCX=this, DL=bFromLevelStreaming. AActor::BeginPlay is vtable slot 116 (offset 0x3A0), Tick slot 160 (0x500), PostInitializeComponents slot 166 (0x530).
- **AESPawn::ShouldPossessAiOnSpawn (exec stub)** @ `0195E760` unique=True — Returns (AutoPossessAI(+0x2B9) - 2) <= 1, i.e. AutoPossessAI in {Spawned, PlacedInWorldOrSpawned}. Confirms ES2 NPC pawn BPs rely on the stock AutoPossessAI mechanism, which is exactly what the NM_Client guard in APawn::PostInitializeComponents disables.
  - `public: static void __cdecl AESPawn::execShouldPossessAiOnSpawn(class UObject *, struct FFrame &, void *const)`
  - ABI: Standard exec stub ABI: RCX=UObject* Context, RDX=FFrame&, R8=void* Result.

## Types

### FActorSpawnParameters (size 128)
- 0x00 Name FName
- 0x08 Template AActor* — the archetype; replication sets this to the server's archetype
- 0x10 Owner AActor*
- 0x18 Instigator APawn*
- 0x20 OverrideLevel ULevel* — replication sets this to the target level
- 0x28 OverrideParentComponent UChildActorComponent*
- 0x30 SpawnCollisionHandlingOverride ESpawnActorCollisionHandlingMethod (uint8)
- 0x31 TransformScaleMethod ESpawnActorScaleMethod (uint8)
- 0x32 bit0 bRemoteOwned — *** THE DISCRIMINATOR ***: mask 0x01. Set (together with bNoFail) only by UPackageMapClient::SerializeNewActor (04DA06B5 'or r9b,0x3') and UNetActorFactory::InstantiateReplicatedObjectFromHeader (04D6CDAF 'or al,0x3'). No ES2 gameplay path sets it.
- 0x32 bit1 bNoFail — mask 0x02
- 0x32 bit2 bDeferConstruction — mask 0x04 (ADockableStation::SpawnShip sets only this)
- 0x32 bit3 bAllowDuringConstructionScript — mask 0x08
- 0x32 bit4 bForceGloballyUniqueName — mask 0x10
- 0x33 NameMode FActorSpawnParameters::ESpawnActorNameMode (uint8)
- 0x34 ObjectFlags EObjectFlags (uint32) — replication sets 0x8 = RF_Transient
- 0x40 CustomPreSpawnInitalization TFunction<void(AActor*)> (64 bytes)

### AActor (net-relevant fields) (size 680)
- 0x58 bit1 bNetStartup — set by ULevel::InitializeNetworkActors for bNetLoadOnClient level actors
- 0x58 bit2 bOnlyRelevantToOwner
- 0x58 bit3 bAlwaysRelevant
- 0x59 bit0 bTearOff
- 0x59 bit2 bExchangedRoles
- 0x59 bit3 bNetLoadOnClient — if false the client Destroy()s the level actor outright
- 0x5B bit2 bActorSeamlessTraveled
- 0x5B bit3 bReplicates — must be true BEFORE ULevel::InitializeNetworkActors for a level actor to bind to the server's copy
- 0x5C bit1 bActorInitialized
- 0x60 RemoteRole TEnumAsByte<ENetRole>
- 0x160 NetDriverName FName
- 0x168 Role TEnumAsByte<ENetRole> — client test: ROLE_Authority(3) = my own local simulation; SimulatedProxy(1) = host-owned; ROLE_None(0) = non-replicated level actor after role exchange
- 0x169 NetDormancy TEnumAsByte<ENetDormancy>
- 0x16A SpawnCollisionHandlingMethod
- 0x184 NetCullDistanceSquared float

### APawn (AI-relevant fields) (size 760)
- 0x2B0 bit0..2 bUseControllerRotationPitch/Yaw/Roll
- 0x2B8 AutoPossessPlayer TEnumAsByte<EAutoReceiveInput::Type>
- 0x2B9 AutoPossessAI EAutoPossessAI — Disabled=0, PlacedInWorld=1, Spawned=2, PlacedInWorldOrSpawned=3
- 0x2C0 AIControllerClass TSubclassOf<AController>
- 0x2D0 LastHitBy TObjectPtr<AController>
- 0x2D8 Controller TObjectPtr<AController> — null check gate in PostInitializeComponents
- 0x2E0 PreviousController TObjectPtr<AController>
- vtable slot 247 (offset 0x7B8) PossessedBy; slot 248 (0x7C0) UnPossessed; slot 262 (offset 0x830) SpawnDefaultController

### AAIController (size 976)
- 0x378 bit0 bStartAILogicOnPossess
- 0x380 PathFollowingComponent TObjectPtr<UPathFollowingComponent>
- 0x388 BrainComponent TObjectPtr<UBrainComponent> — the UBehaviorTreeComponent lives here
- 0x3A0 Blackboard TObjectPtr<UBlackboardComponent>
- vtable slot 278 (offset 0x8B0) RunBehaviorTree; slot 279 (0x8B8) CleanupBrainComponent; slot 282 (0x8D0) InitializeBlackboard
- bases: AController(0), IAIPerceptionListenerInterface(832), IGameplayTaskOwnerInterface(840), IGenericTeamAgentInterface(848), IVisualLoggerDebugSnapshotInterface(856)

### AController (possession guard) (size 840)
- 0x168 Role (inherited from AActor) — 'cmp byte [rcx+0x168],3' guard in AController::Possess
- 0x2E8 Pawn TObjectPtr<APawn>
- 0x338 bit2 bCanPossessWithoutAuthority — mask 0x04, tested first in AController::Possess
- vtable offset 0x7D8 OnPossess

### AESPawn (size 4912)
- 0xE70 bJumpIn bool — written by SpawnNPCPawn / SpawnNPCPawnWithParams_Native
- 0xE74 JumpInDelay FFloatRange (16 bytes)
- 0xF42 PreferPlayerAsTarget bool
- 0x1038 NPCLevel int — written at spawn
- 0x103C NPCMark int — written at spawn
- 0x1040 NPCLevelingData FNPCLevelingData (128 bytes)
- 0x10C1 bIsPlayerPawn bool — *** read this off the CDO (UClass::GetDefaultObject) to exclude player ships from an NPC class filter ***
- 0x1194 RecentDamageToPlayerIndicatorTime float
- 0x1218 bIsStealthDetectedPlayerPawn bool
- 0x12B8 PlayerPawnEnemyTargetList TArray<AActor*>
- bases: APawn(0), ISavableInterface(808), INavTargetInterface(816)

### FSpawnParameter (size 48)
- 0x00 LocationLevel int
- 0x04 NPCLevel int
- 0x08 LevelOffset int — NPCLevel written to pawn is (NPCLevel + LevelOffset)
- 0x0C Yield int
- 0x10 Mark int
- 0x14 bJumpIn bool
- 0x18 JumpInDelay FFloatRange (16 bytes)
- 0x28 DwellTime float
- 0x2C FactionOverride TEnumAsByte<EFactions::EFaction>
- NOTE: 48 bytes > 8, so when declared by value it is passed BY HIDDEN POINTER (verified in ASpawnGizmoBase::SpawnClass and SpawnNPCPawnForParent)

### UWorld (client-detection fields) (size 0)
- 0x38 NetDriver TObjectPtr<UNetDriver> — null in standalone
- 0xF0 DemoNetDriver TObjectPtr<UDemoNetDriver>
- 0x13D bit4 bStartup — used by APawn::PostInitializeComponents to decide PlacedInWorld vs Spawned
- 0x158 AuthorityGameMode TObjectPtr<AGameModeBase> — NULL ON CLIENTS; this is why every ES2 GameMode-driven spawner is inherently server-only
- 0x160 GameState TObjectPtr<AGameStateBase>

### UNetDriver (size 0)
- 0xF0 ServerConnection TObjectPtr<UNetConnection> — non-null only on a client
- 0xF8 ClientConnections TArray<TObjectPtr<UNetConnection>> — non-empty on host

### FActorSaveState / FSpawnGroupSaveState / FRandomStream (size 320)
- FActorSaveState sizeof=320 — returned BY SRET (hidden RCX) from ULocationLib::SpawnActor_Internal and SpawnActorAtGizmo
- FSpawnGroupSaveState sizeof=64 — returned BY SRET from SpawnGroup_Internal / SpawnGroupAtGizmo / SpawnGroupAtTransform
- FRandomStream sizeof=8 (int InitialSeed @0x0, uint Seed @0x4) — always passed by reference (pointer) in these signatures


## Hook plan

- **UGameplayStatics::BeginDeferredActorSpawnFromClass** @ `014DA498` [client] — P0 — the single widest CLIENT-SAFE choke. Kills every ES2 and Blueprint deferred spawn of NPC-class actors without any risk of touching replication (this function is unreachable from the actor channel).
  - behaviour: Detour returns nullptr instead of calling the original when the discriminator fires. Discriminator: (a) net mode is NM_Client — call UWorld::InternalGetNetMode (01287CF0) on UEngine::GetWorldFromContextObject(RCX) or, cheaper, cache a global 'I am the client' flag the mod already has; (b) class filter — RDX is a TSubclassOf<AActor>* so load UClass* C = *(UClass**)RDX; require UStruct::IsChildOf(C, AESPawn::StaticClass()) (013ACDD0) AND !((uint8*)UClass::GetDefaultObject(C,false))[0x10C1] (bIsPlayerPawn). Returning nullptr is safe: UGameplayStatics::FinishSpawningActor (014DA88C) null-checks its actor at 014DA895, SpawnNPCPawnWithParams_Native has a null path at 01504D65, SpawnNPCPawn at 05E3E4DD. Log the class name on every block for the first session.
  - risk: MEDIUM. Must not be applied without the AESPawn class filter: the same function is used by UDeviceComponent::SpawnDevice(FromItem), UConsumableComponent::UseConsumable, UGameplayLib::SpawnPickups, UMissionLib::SpawnMission*/CreateJobOffer/GetMissionLog_Internal/GetCurrentObjectivesForMissionLog, UGameplayLib::SetPinForLocation, UMapLib::SpawnTemporaryLocation, SpawnPOI and every BP SpawnActor node. None of those classes derive from AESPawn, so the filter is sufficient — but verify with logging before enabling blocking.
- **UWorld::SpawnActor(UClass*, FTransform const*, FActorSpawnParameters const&)** @ `014DB504` [client] — P0 — the ultimate funnel / safety net. Catches the paths that bypass BeginDeferredActorSpawnFromClass: ADockableStation::SpawnShip, UAIBlueprintHelperLibrary::SpawnAIFromClass, UGameplayLib::SpawnPlayerShip, and any future/unknown Blueprint route.
  - behaviour: Detour returns nullptr when ALL of: (1) net mode NM_Client — test RCX(UWorld*)+0x38 (NetDriver) != null && NetDriver+0xF0 (ServerConnection) != null, or call UWorld::InternalGetNetMode (01287CF0); (2) *** ALLOW-LIST: R9 (FActorSpawnParameters const*) is non-null and (((uint8*)R9)[0x32] & 0x01) == 0 *** — i.e. bRemoteOwned is NOT set, so this is not a replication-driven spawn; (3) class filter on RDX (a bare UClass* here, not a TSubclassOf): IsChildOf(AESPawn::StaticClass()) && !CDO->bIsPlayerPawn (+0x10C1). Both direct callers null-check the result (ADockableStation::SpawnShip at 05DDADD2, SpawnAIFromClass at 058EF894), so nullptr is safe.
  - risk: HIGH-TRAFFIC function — keep the detour branch-light (net-mode check first, then the bRemoteOwned byte test, then the class test) and never allocate. Sequencer spawnables (FLevelSequenceActorSpawner::SpawnObject, UMovieSceneSpawnableActorBindingBase::SpawnObjectInternal) also reach here through SpawnActorAbsolute WITHOUT bRemoteOwned, so a cutscene ship of an AESPawn class would be blocked on the client — add an escape hatch (e.g. allow when SpawnParams.ObjectFlags has RF_Transient and OverrideLevel is set, or simply allow spawns whose Owner/Instigator chain is a sequencer actor) if cutscenes break.
- **UGameplayLib::SpawnNPCPawnWithParams_Native** @ `01504C8C` [client] — P1 — narrowest ES2-semantic choke; ideal for logging/telemetry and as a cheap first cut before enabling the P0 hooks.
  - behaviour: On client: return nullptr immediately (xor eax,eax; ret is the function's own failure contract). Covers ULocationLib::SpawnActor_Internal, UGameplayLib::SpawnSavableActor, SpawnNPCPawnForParent, SpawnNPCPawnWithParams and ASpawnGizmoBase::SpawnClass_Implementation in one place.
  - risk: LOW. Every caller handles a null return (all of them null-check the AESPawn* before using it).
- **UGameplayLib::SpawnNPCPawn** @ `05E3E3AC` [client] — P1 — the sibling entry that does NOT route through _Native (used by ANpcDockingPoint::SpawnClass_Implementation for station traffic and by the BP exec stub).
  - behaviour: On client: return nullptr.
  - risk: LOW.
- **UGameplayLib::SpawnPawnFromClass** @ `05E3EB1C` [client] — P1 — Blueprint-only pawn spawner (drones, mission ships).
  - behaviour: On client: return nullptr. Optionally also hook SpawnNPCPawnWithParams (05E3E834) and SpawnNPCPawnForParent (05E3E4F8) for logging, though both funnel into _Native.
  - risk: LOW — but note this is likely the path used for the PLAYER'S OWN combat drones (AESGameModeBase::IsSpawningOfPlayerDronesAllowed @ 02292590 is BP-only). Decide whether the client should get host-replicated drones (then block) or predict its own (then allow classes whose CDO is a drone).
- **ADockableStation::SpawnShipsForSale** @ `05DDAF94` [client] — P1 — the one CONFIRMED client-side NPC spawner that runs from a level actor's BeginPlay and bypasses every UGameplayLib entry point.
  - behaviour: On client: return immediately (void, no return value to fake). Called from ADockableStation::BeginPlay at 05D927CC and from RespawnShipsForSaleIfAlreadySpawned (05DD67D4).
  - risk: LOW-MEDIUM. Blocking it means the client sees no ships in the dealer's display racks. Do NOT also block ADockableStation::SpawnPlayerShips (05DDA8F4) — that is the client's own hangar/ship-collection content and is legitimately per-player.
- **APawn::SpawnDefaultController** @ `016ACA70` [client] — P2 (AI) — belt-and-braces: stop any AI controller from ever being created on the client, including via UAIBlueprintHelperLibrary::SpawnAIFromClass which calls this vtable slot with no net-mode check, and via the reflected UFunction from Blueprint.
  - behaviour: On client: return immediately without calling the original. Hooking the base RVA is valid: the function has ZERO direct rel32 xrefs (it is only reached through vtable slot 262 / offset 0x830), ES2 declares no override, and it is a plain UFUNCTION (not a BlueprintNativeEvent) so Blueprints cannot override it either.
  - risk: LOW. The engine already skips this on clients via the NM_Client guard in APawn::PostInitializeComponents (016AC8C5 'cmp eax,3'), so in normal operation the hook never fires — it only closes the SpawnAIFromClass/BP hole.
- **AAIController::RunBehaviorTree** @ `016AD0E8` [client] — P2 (AI) — prevent behaviour trees from ever starting if a controller does slip through.
  - behaviour: On client: return false (AL=0) without calling the original. This blocks the NewObject<UBehaviorTreeComponent> + RegisterComponent + StartTree sequence. Vtable slot 278 (offset 0x8B0); AAiControllerBase does not override it so the base RVA is the real target.
  - risk: LOW.
- **UBehaviorTreeComponent::TickComponent** @ `01262A80` [client] — P2 (AI) — final backstop. UBehaviorTreeComponent::StartLogic (016AE270) does NOT route through StartTree (016AD290), so blocking StartTree alone is insufficient; blocking the tick stops all BT execution regardless of how the tree got started.
  - behaviour: On client: return immediately. RCX=UBehaviorTreeComponent* this; resolve the owning actor's world/net mode (component -> GetOwner) or use the mod's cached client flag.
  - risk: LOW functionally, but this fires every frame per NPC — keep the detour to a single cached-flag test. Prefer enabling only if AI is observed still running after the controller hooks.
- **AAiControllerBase::OnPossess** @ `016ACE54` [client] — P2 (AI) — alternative single-point AI kill if you would rather let controllers exist (for HUD/target-info purposes) but never think.
  - behaviour: On client: return immediately, skipping AAIController::OnPossess, the faction delegate bindings, UseBlackboard and the RunBehaviorTree vtable call at 016ACFB9.
  - risk: MEDIUM — skipping it also skips faction/blackboard wiring, which some non-AI ES2 code may query. Use EITHER this OR the SpawnDefaultController+RunBehaviorTree pair, not both.
- **AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors** @ `05DE9610` [client] — P3 — GameMode kill switch. Only needed IF runtime inspection shows the client's world actually has an AESGameModeBase (UWorld+0x158 AuthorityGameMode != null). In a correct NM_Client world this never runs.
  - behaviour: On client: return immediately. One hook removes ULocationLib::SpawnOrRestoreLocationActors, AESGameModeBase::SpawnActiveMissions, SpawnPerks and SpawnChallenges in a single stroke.
  - risk: LOW on a client; catastrophic if accidentally applied on the host — gate strictly on net mode.
- **ULocationLib::TickBusyness** @ `015076B0` [client] — P3 — stops the periodic ambient-traffic spawner. Same conditional as above (only reachable from AESGameModeBase::Tick, i.e. only if the client has a GameMode).
  - behaviour: On client: return immediately. Alternative single point: AESGameModeBase::Tick (0137CAE0) — but that also disables time-dilation/stat bookkeeping, so prefer TickBusyness. A non-hook alternative that works even on the host's simulation for the client's benefit: call the reflected UFunction UGameplayLib::PushSuppressLocationBusyness (05E37964) once.
  - risk: LOW.
- **ULocationLib::SpawnOrRestoreLocationActors / SpawnOrRestoreLocationPOIs / SpawnWingmen** @ `05E6A52C` [client] — P3 — finer-grained versions of the GameMode kill switch if you want POIs to still populate locally (they are static scenery and are cheap to duplicate) while suppressing NPC groups.
  - behaviour: Hook ULocationLib::SpawnOrRestoreLocationActors (05E6A52C, void, RCX=WorldCtx, RDX=FRandomStream*) and ULocationLib::SpawnWingmen (019555D4, void, RCX=WorldCtx, RDX=FRandomStream*) to return immediately on the client; LEAVE ULocationLib::SpawnOrRestoreLocationPOIs (05E6B838) alone so asteroids/props still exist client-side.
  - risk: LOW. Do not hook the *_Internal group functions (SpawnActor_Internal 015062C8, SpawnGroup_Internal 0150BE40) unless necessary — they return large structs by SRET (FActorSaveState 320 B, FSpawnGroupSaveState 64 B) and a detour must construct a valid empty struct at *RCX and return RCX in RAX.
- **Client-side cleanup after connect (no hook — runtime calls)** @ `05E5AA24` [client] — P1 — remove the client-local NPCs that already exist from LoadMap (level-placed pawns and anything spawned before the hooks were armed), so the client only keeps the host's replicated copies.
  - behaviour: Two options. (a) Call the reflected UFunction ULocationLib::EvictAllNPCs (05E5AA24) via ProcessEvent with {UObject* WorldCtx, bool bPushSuppressBusyness=true, FModifierHandle& out} — it jump-drives every registered pawn out of the location and pushes a busyness suppression. (b) Enumerate USelfRegisteringComponent::GetAllActorsOfRegister(ERegisterIds::Pawn=2) @ 0150FDA8 (returns a const TArray<AActor*>& — COPY it first), and for each actor where *(uint8*)(actor+0x168) == 3 (ROLE_Authority) and the actor is not the local player pawn, call AActor::Destroy (014DA5D4). Never destroy actors with Role==ROLE_SimulatedProxy(1) — those are the host's, bound to an actor channel.
  - risk: MEDIUM. Level-placed actors that are net-startup-bound end up with Role==ROLE_SimulatedProxy after ULevel::InitializeNetworkActors' ExchangeNetRoles; destroying those breaks the channel. Non-replicated level actors end up with Role==ROLE_None(0), and locally spawned ones with ROLE_Authority(3) — only those two are safe to remove.
- **Replication prerequisite for level-placed NPCs (design note, both sides)** @ `04CC30C8` [both] — Make the host's and client's level-placed NPC pawns bind to each other instead of duplicating. Not a detour on 04CC30C8 itself — this documents the ordering constraint it imposes.
  - behaviour: ULevel::InitializeNetworkActors (04CC30C8) runs ONCE per level, before BeginPlay, and only role-exchanges actors that already have RemoteRole set (i.e. bReplicates true at that moment). Therefore the mod must set bReplicates/RemoteRole on the CDO of the relevant classes (or patch AActor construction) BEFORE the level is initialised on BOTH sides — flipping it later via AActor::SetReplicates (046529B0) leaves the client's copy at Role==ROLE_None and it will never bind to the server's copy (both sides then simulate their own). Verify at runtime that a level-placed NPC on the client reports Role==1 (SimulatedProxy) and bNetStartup==1 (byte at +0x58 bit1).
  - risk: HIGH if got wrong — it is the root cause of duplicate level NPCs and is invisible to any spawn hook, because level-placed actors are never spawned at runtime at all.

## Open questions

- Does the client's world actually have an AESGameModeBase? Every ES2 manager (AMapEventManager, AWantedLevelManager, ABattleSimulator, ASequenceManager) is a CHILD ACTOR of the GameMode (proved via AESGameModeBase::GetManagerFromGameMode @ 0121D814 -> UWorld::GetAuthGameMode -> GetAllChildActors), and UWorld::AuthorityGameMode (+0x158) is null on a real NM_Client world. If the observed client-side simulation includes wanted levels / map events / busyness traffic, then the client is NOT in a clean NM_Client world and the P3 GameMode hooks are mandatory. Check at runtime: dump UWorld+0x158 and UWorld::InternalGetNetMode (01287CF0) on the client.
- Which concrete NPC pawns actually appear on the client — level-placed (bNetStartup, Role should be 1 or 0) or runtime-spawned (Role==3)? A one-line runtime dump over USelfRegisteringComponent::GetAllActorsOfRegister(Pawn=2) reading each actor's Role (+0x168), bNetStartup (+0x58 bit1), bReplicates (+0x5B bit3) and class name will decide whether the fix is spawn hooks, replication-role plumbing, or both. I could not determine this statically.
- Does any ES2 Blueprint call UAIBlueprintHelperLibrary::SpawnAIFromClass (058EF62C)? It is the only path that creates an AI controller on a client with no net-mode guard, but whether ES2's content actually uses it can only be confirmed by logging the hook or by scanning the .pak Blueprint bytecode (not available here).
- Do Sequencer/cutscene spawnables ever spawn AESPawn-derived actors? FLevelSequenceActorSpawner::SpawnObject and UMovieSceneSpawnableActorBindingBase::SpawnObjectInternal both reach UWorld::SpawnActorAbsolute WITHOUT setting bRemoteOwned, so the recommended UWorld::SpawnActor discriminator would block them on the client. If cutscene ships disappear on the client, that is the cause.
- Should the client predict its own combat drones / wingmen (UGameplayLib::SpawnNPCPawnForParent, SpawnPawnFromClass, AESGameModeBase::IsSpawningOfPlayerDronesAllowed @ 02292590) or receive them replicated from the host? The static analysis cannot decide this; it is a design call that changes whether those two entry points are blocked or allowed.
- ANpcDockingPoint::LaunchOrAttractShip (05D98674) has ZERO direct rel32 xrefs and is bound as a timer/delegate callback (TBaseUObjectMethodDelegateInstance<0, ANpcDockingPoint, void(void)> ctor at 05D8A824), and the member-function pointer is not stored as an absolute 8-byte pointer anywhere in the image. I could not determine statically where the timer is set, so I could not confirm whether station traffic spawning self-starts on a client. It reaches ULocationLib::SpawnActorAtGizmo (05E6A388), which is covered by the SpawnNPCPawnWithParams_Native and BeginDeferredActorSpawnFromClass hooks, so it is covered defensively.
- ADynamicJobManager::BeginPlay (05DE07E0) reaches a spawn funnel at depth 3 via timers; I did not trace whether the spawned actors are AMissionBase (harmless, UI-backing) or include NPC pawns. Worth logging.
