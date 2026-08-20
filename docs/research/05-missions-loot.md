# ES2 co-op research: topic

_Auto-generated from the PDB research workflow (2026-08-19)._

## Summary

## P5 world state (content half) — how ES2 actually works, and what that means for co-op

### 1. The single most important architectural fact
`AESGameModeBase : AGameMode : AGameModeBase`. `AESGameModeBase::GetESGameMode` (01505960) is just `UGameplayStatics::GetGameMode` (0145A9AC), which is `UWorld+0x158` (`AuthorityGameMode`) — **null on a network client**. Therefore, on the joining client, none of these ever run: `AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors` (05DE9610) → `SpawnActiveMissions` (05DEB8B4) / `SpawnPerks` (0178E11C) / `SpawnChallenges` (05DEBDF8); `DisplayCurrentObjectives` (0190AD30); `LogDialogSubtitle` (01928210); `SpawnDefaultPawnAtTransform_Implementation` (05DEC54C). **No suppression hooks are needed on the client — mission/challenge/perk actors simply never exist there.** That answers Q2's first half: run all mission logic host-only; there is nothing to suppress.

Second key fact: **no ES2 game class overrides `GetLifetimeReplicatedProps`** (only `AGameStateBase` 04AB79DC, `AGameState` 04AB7600, `APlayerController` 04E8AA2C do). ES2 has *zero* custom property replication. Everything except base-`AActor` transform/relevancy must be mirrored by the mod.

Third: `UPlayerData` is a **per-process singleton** — `UGameplayLib::GetPlayerData` (0127398C) is literally `*(UESGameInstance**)(base+0x09AC5E88) -> +0x1C0`. Host and client each have their own complete, independent `UPlayerData`. This is exactly what the per-player-inventory design needs, and it is also where all mission state lives.

### 2. Mission architecture
Mission state is stored **in `UPlayerData`, not in the actors**. `UPlayerData::MissionSaveState` is `TArray<FTaskSaveGameData>` at **+0x3F8** (Num at +0x400, stride 0x140 = sizeof 320). `UMissionLib::FindTaskInPlayerData(FName)` (01312104) is a linear scan of that array matching `TaskID` at struct offset +0x30. `CompletedMissions` (TArray<FName>) is at +0x1438; tracked-mission FNames at +0x160C/0x1614/0x161C (Main/Side/Job).

`AMissionTaskBase` is an **AActor** (sizeof 1008), spawned by the GameMode at level start from `UMissionLib::GetActiveMissionsToSpawn()` (05E79160, reads UPlayerData) via `BeginDeferredActorSpawnFromClass` + `FinishSpawningActor`. The actor is a *runtime executor* holding live state (`TaskState` +0x2D8, `ProgressValue` +0x300, `StageValue` +0x304, `Attributes` TMap +0x318, delegates +0x378..+0x3D8). `AMissionBase` (sizeof 1208) derives from `AMissionTaskBase` and adds rewards at +0x418 (`FMissionRewards`) and a mission-type byte at +0x400.

**Every state mutation funnels through exactly one function: `UMissionLib::UpdateTaskInPlayerData(AMissionTaskBase*, bool, bool)` at 015124A0.** A full xref scan of `.text` found 13 call sites and they are *all* the mutation paths: `execSetProgress`, `execIncProgress`, `execSetStation`, `execSetLocation`, `execUpdateTaskData`, `BroadcastStateChange` (= every state switch), `EnterStage_Internal` (= every stage change), `SetAttribute`, `FindOrCreateAttribute`, `SaveStateOfCurrentlySpawnedMissions`. This function: reads `GetESHUD`, reads `GetPlayerData`, calls the task's vtable[239] `GetSaveGameData()` → `FTaskSaveGameData`, finds the matching record in `MissionSaveState` and overwrites it, computes `bStateChange/bProgressChange/bStageChange` flags into an `FHUDTaskData`, and calls `AESHUD::ShowMissionTaskUpdate` (017913C8) for the HUD toast. **This is the one and only hook the host needs for mission progress.**

Completion path in detail: `SwitchState` (05EBD188) → `SwitchState_Internal(state, bForce, stage)` (05EBD194): writes `TaskState` at +0x2D8, stamps `TimeStamp` from `UPlayerData+0x2130` for terminal states, calls `BroadcastStateChange()` (0195E9AC → `UpdateTaskInPlayerData(this,0,0)` + fires `OnTaskActivated` when state==Active), then for Failed/Succeeded/Aborted/AbortedAndHidden calls `ClearMissionCheckpoint` + `AbortChildren`, and for Active calls `InitTimeLimit`. `EMissionTaskState::Type` = {Invalid 0, InactiveAndHidden 1, Inactive 2, AwaitingActivation 3, Active 4, Failed 5, Succeeded 6, Aborted 7, AbortedAndHidden 8, RewardPending 9}.

Progress: `SetProgress`/`IncProgress` are inlined into their exec stubs (05CB4888 / 05CB4028) — they write +0x300, call `UpdateTaskInPlayerData(this,0,0)`, then broadcast `OnTaskProgressed` (+0x3B8).

`ASpawnMissionItem` and `UAsyncSpawnMission` are thin: the former is a save-stateful actor with no interesting logic; the latter is a latent BP node wrapping `UMissionLib::SpawnMission_Internal`. `UMissionLogIDs` / `UDialogIDs` are pure FName-constant holders (`CreateSingleton` + statics only).

### 3. Client display path — the good news
**The mission log and world indicators are 100% data-driven from `UPlayerData` and need no mission actors.**
- `UMissionLib::GetCurrentObjectivesForMissionLog` (05E7AB20) reads only `FindTaskInPlayerData`, and where it needs objective text it **spawns a throwaway task actor with BeginPlay suppressed** (`TLoadClassSynchronous` → `BeginDeferredActorSpawnFromClass` → `FinishSpawnSuppressBeginPlay` 017915E8 → `GetObjectiveDescription()` → `AActor::Destroy`).
- `UMissionLib::GetMissionObjectiveTextFromTaskData` (05E7E97C) uses the class **CDO** (`TGetClassDefaultObjectIfLoaded`) — no actor at all.
- `UMissionLib::GetAllChildTasks` (018CE41C) reads `FindTaskInPlayerData` recursively.
- `UMapLib::RefreshMissionAndWaypointIndicators` (016C9C58) reads `GetPlayerData` + `UMapLib::GetIngameMissionIndicatorLocations` (016CA50C → `UMissionLib::GetActiveTasksLocations` 013125C4, all UPlayerData) and then sets `EIndicator::MainMission/SideMission/Job` on `UHUDMarkerComponent`s of registered `AJumpTarget`s. It is reflected and callable from the client.
- `AESHUD` **does exist on the client** (`AGameModeBase::InitializeHUDForPlayer` 04A3D8B4 → `APlayerController::ClientSetHUD` RPC), and `UGameplayLib::GetESHUD` (015576B8) = `GetPlayerController(0)->MyHUD` works on both sides.
- `AESHUD::ShowMissionTaskUpdate(FHUDTaskData const&)` (017913C8) is a BlueprintImplementableEvent (FindFunctionChecked + ProcessEvent via vtable+0x278) — the mod can call it directly with a synthesized `FHUDTaskData`.

So: **mirror `FTaskSaveGameData` records + the Tracked* FNames into the client's `UPlayerData`, then call `RefreshMissionAndWaypointIndicators` and `ShowMissionTaskUpdate`.** The only thing the client loses is `AESGameModeBase::DisplayCurrentObjectives` (0190AD30, which iterates `USelfRegisteringComponent::GetAllActorsOfRegister(ERegisterIds::MissionTaskBase=15)` and calls `UMissionLib::ShowActiveMissionObjectivesInHUD` 01791754) — the mod replaces that with a direct `ShowMissionTaskUpdate` call.

### 4. Dialog
`UDialogManager` is a per-process singleton pair: `GetSingleton(bool bMenu)` (0184197C) reads two globals at RVA **09ACBBD0** (game) / **09ACBBD8** (menu). All public entry points funnel into one private method: **`UDialogManager::EnqueueDialog(FName DialogID, FDialogFinishedDelegate const*, float Delay, EDialog::Type, EDialogBehavior::Type, bool PlayOnce)` at 01841D10**, reached from `EnqueueMissionDialog` (01840B90, passes `EDialog::MainMission`=0x28) and `EnqueueDialog` (01840F9C). It gates on `DialogCounter::TryInc` (01841EF4) which is backed by `UPlayerData::DialogCounterSaveState` (+0xB58). `EDialog::Type` = {Undefined 0, SmallTalk 10, Lore 20, GenericMission 30, MainMission 40, Movie 50}; `EDialogBehavior::Type` = {Default 0, DontPauseInIngameMenu 1, Modal 2, Persistent 3}; `EDialogSoundType` = {VoiceActor 0, TextToSpeech 1, NoSound 2}.

**Yes, there is a single broadcastable entry point.** Hook 01841D10 on the host to observe `(DialogID, Delay, Type, Behavior, PlayOnce)`; on the client, call the reflected static `UDialogManager::EnqueueMissionDialog` (01840B90) or `EnqueueDialog` (01840F9C) with the same args. `AESGameModeBase::LogDialogSubtitle` (01928210) is only a mission-log writer — it appends `FDialogLogEntry` to `UPlayerData::DialogLog` (+0x19D8) via the gamemode's cached PlayerData at gamemode+0x518; it is host-only and cosmetic (client's dialog log stays empty unless mirrored).

### 5. Conditions — Q4 answered: **`UConditionBaseComponent` is NOT the mission-condition system.**
It is the **status-effect / buff-debuff framework**: `ConditionStack` `TArray<FToken>` (+0x158), `Stacktype` (`EConditionStacktype` = Invalid/Intensity/Duration/MaxIntensity/MaxInstigatorLevel), `TokenLimit`, `bIsDebuff`, `bIsImpairingMovement`, `bShowConditionInHud`, `EndOverheatedCondition`, `TickIntensity`/`TickDuration`, `HasPawnAnyMovementImpairingConditions`. Subclasses are `UBuildupConditionComponent`, `UConditionEmitterComponent`, `UConditionInstigatorComponent`. It never touches `UPlayerData`. It belongs to P4 Combat (damage-over-time, EMP, overheat), not to P5. There is no generic "gameplay condition met" query system: mission gating is done inside the mission-task Blueprint graphs (delegates `OnTaskActivated/Succeeded/Failed/Ended/Progressed/StageChanged` at +0x378..+0x3D8, plus `FindAttribute`/`SetAttribute` on the task's `Attributes` TMap).

### 6. Loot — the whole pipeline is hard-wired to local player 0
Four independent places prove it:
- `ULootCollectComponent::OnBeginOverlapCollect` (01504334): `if (this->OwnerPrivate(+0x90) != GetPlayerController(world,0)->GetPawnOrSpectator()) return;` then appends to `PickupsInRange` (+0x240).
- `APickupBase::OnCollect_Implementation` (016B720C): compares the Collector against `UGameplayStatics::GetPlayerPawn(world, 0)`; only on match does it take the player path.
- `APickupBase::Tick` (0150D2C0) and `InteractPullItem` (016B7E28) / `InteractCollect` (016B7104): all use `GetESPlayerController` (0150D280) / `GetPlayerPawn(...,0)`.
- `ULootDropComponent::DropLoot` (016CCDC4): the `bOnlyDropLootWhenPlayerIsCloseOrWhenDamagedByPlayer` gate uses `GetPlayerPawn(world,0)` + `GetDistanceTo`.

Collection routes the item into **`Collector(AESPawn)->ShipData(+0x4B8).Inventory(+0x10 → pawn+0x4C8)`** via `UInventory::AddItem` (0153C8FC), then broadcasts `APickupBase::OnPickedUpByPlayer` (+0x318), adds a HUD log message, calls `UPlayerData::OnItemCollected` (016B688C) and broadcasts `UESGameInstance::OnPlayerPickedUpItem` (gameinstance+0x910). Note `UGameplayLib::SpawnPlayerShip` (05E3EE5C) fills the pawn's `ShipData` from `UInventoryLib::GetPlayerShip(idx)` (01805EE0) = the **local** `UPlayerData` — so on the host the client's server-side pawn currently carries the *host's* ship+inventory.

**Answer to "who collects, and how does the item reach the client's own inventory": don't fight the hard-coding — run pickups locally.** Host owns *which* loot exists and *whether it is consumed*; each machine spawns its own local (non-replicated) `APickupBase` and lets the untouched vanilla path collect it into that machine's own `UPlayerData`. `APickupBase` has **no** `bReplicates` set in its C++ ctor (01717C64 never touches AActor+0x5B), so nothing replicates today anyway. `UGameplayLib::SpawnPickups` (015036D8) and `SpawnPickupFromItem` (05E3EC44) / `SpawnPickupFromItemID` (05E3EDB0) work fine on a client (plain deferred SpawnActor). For "give this exact item to that player", the wire format is `FItemState` (sizeof 464): `UItem::GetItemState()` (05E9B940) → marshal → `UItem::CreateItemFromState()` (05E98820) → `UInventoryLib::AddItemToRespectiveInventory` (05E47738, routes to crafting / quest / current-ship cargo).

### 7. XP
`UXPComponent` is on `AESPawn+0x3B8`; `ULootDropComponent` on `AESPawn+0x3A8`. `AESPawn::RemoveDropAndXP(bool bRemoveDrop, bool bRemoveXP)` (0193E9A0) just empties `LootDrop->LootToDrop` (+0x120) and zeroes `XP->XP` (+0xA0) — it is a suppressor, not an awarder.

The award is `UXPComponent::OwnerHealthDepleted(AActor*, AActor*, AController*)` (019423AC): sums max hitpoints of Health/Armor/Shield components, divides the player's damage contribution (`UFactionComponent->FactionEntries(+0x150)[0]+0xC`) by it, compares against `NeededPlayerDamageRatio` (+0xA8); if the player earned it, it scales `XP` (+0xA0) by the player-level-vs-pawn-level delta (`UPlayerData+0x128` vs pawn+0x1038) and then, in the cold path at 02B2558C..02B2572F, calls `AESHUD::ShowXPNumbers` (05AF7AE4) and **`UGameplayLib::AddXP(float, bool, bool, float)` (05E209B8) → `UPlayerData::AddXP` (05DFA9F8)**. Crucially the difficulty gate in that path is `GetESGameModeBase(...)` and is **skipped entirely when the gamemode is null**, so calling `AddXP` on the client works. `GetPlayerLevel` (05E2E478) and `GetMaxPlayerLevel` (019424C0) both read the local `UPlayerData` (+0x128).

**Award XP to both players**: hook `UXPComponent::OwnerHealthDepleted` on the host (post), read the scaled `XPComponent+0xA0`, broadcast, and have each client call `UGameplayLib::AddXP(xp, false, false, 0.f)` locally. Same shape for mission rewards: `UMissionLib::AddNonItemRewards` (05E71518) calls `AddXP` + `UGameplayLib::ChangeCredits` (05E21DF0) + `IncJobScore` + decal unlocks — hook it on the host and replay it on the client.

### 8. The design, in one line per system
- **Host-only**: mission/challenge/perk actor spawning and all mission Blueprint logic; loot *drop decision*; NPC kill authority; dialog *triggering*. (Nothing to suppress on the client — no GameMode there.)
- **Mirrored host→client**: `FTaskSaveGameData` deltas (one hook), Tracked* mission FNames, `CompletedMissions`, dialog play commands, loot-spawn descriptors and consume/destroy events, XP-on-kill amounts, mission non-item rewards.
- **Client-only / strictly per-player**: `UPlayerData` inventory / credits / XP / level / ships, `DialogCounterSaveState`, all `APickupBase` actors and their collection, HUD and mission-log UI, `UConditionBaseComponent` status effects on one's own pawn.

## Key functions

- **UMissionLib::UpdateTaskInPlayerData** @ `015124A0` unique=True — THE single choke point for every mission state mutation. Full .text xref scan gives exactly 13 call sites: execSetProgress(05CB49EA), execIncProgress(05CB418E), execSetStation(05CB4B82), execSetLocation(0195A4F2), execUpdateTaskData(05CB80BA jmp), BroadcastStateChange(0195E9BA), EnterStage_Internal(01922E25), SetAttribute(05EBB7A6), FindOrCreateAttribute(05E9A37C), SaveStateOfCurrentlySpawnedMissi
  - `public: static void __cdecl UMissionLib::UpdateTaskInPlayerData(class AMissionTaskBase *, bool, bool)`
  - ABI: Free static. RCX = AMissionTaskBase*, DL = arg2 (bool), R8B = arg3 (bool). Confirmed in prologue: 'mov r14b,r8b; mov r15b,dl; mov rbx,rcx'. Observed call sites: all internal mutation paths pass (false,false); the reflected BP node AMissionTaskBase::UpdateTaskData tail-jumps here with (true,false) (0
- **AMissionTaskBase::SwitchState_Internal** @ `05EBD194` unique=True — The task state machine. Early-outs if NewState==current (unless bForce and current state is 5..9). Writes TaskState at this+0x2D8; for states 4..9 stamps TimeStamp(this+0x310) from UPlayerData+0x2130 (PlayingTimeSecondsDouble); if the class is AMissionBase and state qualifies, copies FMissionRewards from the CDO (this+0x418); calls BroadcastStateChange(0195E9AC); for Failed/Succeeded/Aborted/Abort
  - `private: void __cdecl AMissionTaskBase::SwitchState_Internal(class TEnumAsByte<enum EMissionTaskState::Type>, bool, int)`
  - ABI: RCX=this, DL=NewState (TEnumAsByte, 1 byte), R8B=bForce, R9D=StageOverride (int, -1 = keep). Public wrapper AMissionTaskBase::SwitchState (05EBD188) is a 3-instruction thunk: 'or r9d,-1; xor r8d,r8d; jmp'.
- **AMissionTaskBase::BroadcastStateChange** @ `0195E9AC` unique=True — Calls UpdateTaskInPlayerData(this,0,0), then if TaskState(+0x2D8)==4 (Active) fires the OnTaskActivated multicast at this+0x378. A finer-grained alternative hook point to UpdateTaskInPlayerData if you only want state transitions.
  - `protected: void __cdecl AMissionTaskBase::BroadcastStateChange(void)`
  - ABI: RCX=this. No args.
- **AMissionTaskBase::EnterStage_Internal** @ `01922E04` unique=True — If Stage differs (or bForce): writes StageValue at this+0x304, calls UpdateTaskInPlayerData(this,0,0), fires OnStageChanged (this+0x3C8) and calls the BP event OnEnteredStage(01922E5C).
  - `private: void __cdecl AMissionTaskBase::EnterStage_Internal(int, bool)`
  - ABI: RCX=this, EDX=Stage (int), R8B=bForce.
- **AMissionTaskBase::execSetProgress** @ `05CB4888` unique=True — Steps the int 'Progress' parameter out of the FFrame bytecode, compares it against ProgressValue at Context+0x300, and if it differs writes it, calls UMissionLib::UpdateTaskInPlayerData(this,0,0) at 05CB49EA, then fires the OnTaskProgressed multicast at this+0x3B8 with the new value. This is the 'mark objective progress' entry point that mission Blueprints call.
  - `public: static void __cdecl AMissionTaskBase::execSetProgress(class UObject *, struct FFrame &, void *const)`
  - ABI: Standard UFunction exec thunk (RCX=Context UObject*, RDX=FFrame&, R8=RESULT). The native SetProgress body is inlined here; there is no separate AMissionTaskBase::SetProgress symbol. Reflected param name: 'Progress' (int). Callable via ProcessEvent on a mission task actor.
- **AMissionTaskBase::execIncProgress** @ `05CB4028` unique=True — Reads Increment; if >0 and it changes ProgressValue(+0x300), writes it, calls UpdateTaskInPlayerData(this,0,0) at 05CB418E, fires OnTaskProgressed (+0x3B8) with the new value.
  - `public: static void __cdecl AMissionTaskBase::execIncProgress(class UObject *, struct FFrame &, void *const)`
  - ABI: Exec thunk; native IncProgress inlined. Reflected param name: 'Increment' (int).
- **UMissionLib::FindTaskInPlayerData** @ `01312104` unique=True — GetPlayerData() then linear scan of the TArray at UPlayerData+0x3F8 (Num at +0x400), stride 0x140, matching FName at record+0x30 (TaskID). This is the primitive the whole mission-log/objective UI is built on — and the primitive the mod uses to write mirrored records on the client.
  - `public: static struct FTaskSaveGameData * __cdecl UMissionLib::FindTaskInPlayerData(class FName)`
  - ABI: RCX = FName by value (8 bytes, fits a register). Returns a RAW POINTER into UPlayerData::MissionSaveState — writable, and invalidated by any array growth. There is a second overload UMissionLib::FindTaskInPlayerData(FName, FTaskSaveGameData&) at 013120A0 which IS reflected and returns a copy.
- **UMissionLib::GetAllMissionsInPlayerData** @ `015B3484` unique=True — Filters UPlayerData::MissionSaveState by mission type using GetMissionTypeFromTaskData(015B3314). Use on the host to build the full-state snapshot sent on join.
  - `public: static class TArray<struct FTaskSaveGameData, class TSizedDefaultAllocator<32>> __cdecl UMissionLib::GetAllMissionsInPlayerData(enum EMissionType::Type, bool, bool, bool)`
  - ABI: Returns TArray by value -> hidden return pointer in RCX; EDX=EMissionType, then 3 bools in R8B/R9B/stack.
- **UMissionLib::GetActiveMissionsToSpawn** @ `05E79160` unique=True — Reads UPlayerData and returns the FTaskSaveGameData set the GameMode should instantiate at this location. Called only from AESGameModeBase::SpawnActiveMissions.
  - `public: static class TArray<struct FTaskSaveGameData, class TSizedDefaultAllocator<32>> __cdecl UMissionLib::GetActiveMissionsToSpawn(void)`
  - ABI: Hidden return pointer in RCX, no other args.
- **AESGameModeBase::SpawnActiveMissions** @ `05DEB8B4` unique=True — GetActiveMissionsToSpawn() -> for each record, TLoadClassSynchronous(TaskSoftClass) -> UGameplayStatics::BeginDeferredActorSpawnFromClass(0x1414DA498) + FinishSpawningActor(0x1414DA88C). HOST ONLY (GameMode is server-side). Called from AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors (05DE9610) alongside SpawnPerks (0178E11C) and SpawnChallenges (05DEBDF8).
  - `private: void __cdecl AESGameModeBase::SpawnActiveMissions(void)`
  - ABI: RCX=this (AESGameModeBase*). Private, but reachable via AESGameModeBase::execRightBeforeBeginPlayIsCalledForAllActorsInTravelMode.
- **AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors** @ `05DE9610` unique=True — Host-only world-content bootstrap: calls SpawnActiveMissions(+0x24), SpawnPerks(+0x2c), SpawnChallenges(+0x34). Ideal post-hook for 'the host's mission world is now built — snapshot and broadcast it'.
  - `private: void __cdecl AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors(void)`
  - ABI: RCX=this.
- **AESGameModeBase::DisplayCurrentObjectives** @ `0190AD30` unique=True — Iterates USelfRegisteringComponent::GetAllActorsOfRegister(ERegisterIds::MissionTaskBase=15) (0150FDA8), filters by class, and calls UMissionLib::ShowActiveMissionObjectivesInHUD (01791754) for each. This is the actor-driven objective display the client cannot use — replace it with a direct AESHUD::ShowMissionTaskUpdate call.
  - `private: void __cdecl AESGameModeBase::DisplayCurrentObjectives(void)`
  - ABI: RCX=this. Reflected as a BlueprintCallable on AESGameModeBase — but the GameMode object does not exist on a client, so it is unreachable there by any means.
- **UMissionLib::ShowActiveMissionObjectivesInHUD** @ `01791754` unique=True — GetESHUD, reads the task's vtable[242] GetOwningMissionID and vtable[241] GetOwningMissionTitle, calls GetAllChildTasks(018CE41C) which reads UPlayerData, and for each child spawns a THROWAWAY task actor (BeginDeferredActorSpawnFromClass + FinishSpawnSuppressBeginPlay 017915E8) purely to call GetObjectiveDescription(01791690), then fills FHUDTaskData and calls AESHUD::ShowMissionTaskUpdate (017913
  - `public: static void __cdecl UMissionLib::ShowActiveMissionObjectivesInHUD(class AMissionTaskBase *)`
  - ABI: RCX = AMissionTaskBase*. Needs a live task actor for the top-level task only.
- **AESHUD::ShowMissionTaskUpdate** @ `017913C8` unique=True — THE client-side mission HUD entry point. Feed it a synthesized FHUDTaskData built from a mirrored FTaskSaveGameData and the client shows the same objective/progress toast the host does — no mission actor required.
  - `public: void __cdecl AESHUD::ShowMissionTaskUpdate(struct FHUDTaskData const &)`
  - ABI: RCX = AESHUD*, RDX = const FHUDTaskData* (376-byte struct passed BY HIDDEN POINTER). Internally it copy-constructs the struct, does FindFunctionChecked("ShowMissionTaskUpdate") and ProcessEvent via vtable+0x278 — i.e. it is a BlueprintImplementableEvent, so the HUD Blueprint does the actual drawing.
- **UMissionLib::GetCurrentObjectivesForMissionLog** @ `05E7AB20` unique=True — Proof that the mission log is actor-free: it only calls FindTaskInPlayerData (3x), and to get objective text it spawns a task actor with BeginPlay suppressed, calls GetObjectiveDescription, then AActor::Destroy. Works verbatim on the client once MissionSaveState is mirrored.
  - `public: static class TArray<struct FObjectiveDescription, class TSizedDefaultAllocator<32>> __cdecl UMissionLib::GetCurrentObjectivesForMissionLog(class UObject const *, class FName, class FName, bool, bool, bool)`
  - ABI: Hidden return pointer in RCX; RDX=WorldContext, R8/R9 = two FNames by value, 3 bools on the stack.
- **UMapLib::RefreshMissionAndWaypointIndicators** @ `016C9C58` unique=True — Reads UPlayerData (TrackedMainMission +0x160C / TrackedSideMission +0x1614 / TrackedJob +0x161C / CustomWaypoint +0x1604), resolves target locations via GetIngameMissionIndicatorLocations (016CA50C -> UMissionLib::GetActiveTasksLocations 013125C4), then walks GetAllActorsOfRegister and calls UHUDMarkerComponent::AddOrRemoveIndicator (016C9F4C) with EIndicator::MainMission/SideMission/Job on jump t
  - `public: static void __cdecl UMapLib::RefreshMissionAndWaypointIndicators(void)`
  - ABI: No args. Reflected (Z_Construct_UFunction_UMapLib_RefreshMissionAndWaypointIndicators exists) so it is also ProcessEvent-callable on the UMapLib CDO.
- **UHUDMarkerComponent::AddOrRemoveIndicator** @ `016C9F4C` unique=True — Adds/removes a HUD marker indicator on one actor. EIndicator::Type = {Invalid 0, MainMission 1, MainMissionLocation 2, SideMission 3, SideMissionLocation 4, Job 5, JobLocation 6, Waypoint 7, Homebase 8, JumpGate 9, FastTravel 10, Wormhole 11, ItemShop 12, ShipDealer 13, JobBoard 14, Station 15, Wanted 16, Debuffed 17, AncientAltar 18, MobileHomebase 19}. Use on the client to mark host-driven missi
  - `public: void __cdecl UHUDMarkerComponent::AddOrRemoveIndicator(class TEnumAsByte<enum EIndicator::Type>, bool)`
  - ABI: RCX=this, DL=EIndicator::Type (1 byte), R8B=bAdd. AddIndicator (016CA614) is the bAdd=true tail-jump target.
- **UDialogManager::EnqueueDialog** @ `01841D10` unique=True — THE single dialog choke point. Every public static (EnqueueDialog 01840F9C, EnqueueMissionDialog 01840B90, EnqueueDialogWithEvent 05EC8900, EnqueueMissionDialogWithEvent 05EC8984) resolves the singleton via GetSingleton(bool) (0184197C) and tail-calls this. It gates on DialogCounter::TryInc (01841EF4), which is backed by UPlayerData::DialogCounterSaveState (+0xB58). Hook here on the host to observ
  - `private: bool __cdecl UDialogManager::EnqueueDialog(class FName, class FDialogFinishedDelegate const *, float, enum EDialog::Type, enum EDialogBehavior::Type, bool)`
  - ABI: Instance method: RCX = UDialogManager*, RDX = FName DialogID (by value), R8 = FDialogFinishedDelegate* (may be null), XMM3 = float Delay, then EDialog::Type at [rsp+0x20] and EDialogBehavior::Type at [rsp+0x28], bool PlayOnce at [rsp+0x30]. NOTE the float is in XMM3 because it is the 4th slot, even 
- **UDialogManager::EnqueueMissionDialog** @ `01840B90` unique=True — The clean CALL target for the client to replay a host-triggered dialog line. Resolves the game (non-menu) UDialogManager singleton and forwards to EnqueueDialog.
  - `public: static void __cdecl UDialogManager::EnqueueMissionDialog(bool &, class FName, float, enum EDialogBehavior::Type, bool)`
  - ABI: Static. RCX = bool& WasEnqueued (out), RDX = FName DialogID (by value), XMM2 = float Delay, R9D = EDialogBehavior::Type, [rsp+0x28 of the caller frame] = bool PlayOnce. Hard-codes EDialog::Type = 0x28 (MainMission). Reflected params in the PDB: WasEnqueued, DialogID, Delay, DialogBehavior, PlayOnce.
- **UDialogManager::GetSingleton** @ `0184197C` unique=True — Reads one of two process-global UDialogManager pointers: game manager at RVA 09ACBBD0, menu manager at RVA 09ACBBD8. Per-process, so both host and client have their own live manager.
  - `private: static class UDialogManager & __cdecl UDialogManager::GetSingleton(bool)`
  - ABI: CL = bIsMenuManager. Returns a reference (raw pointer in RAX).
- **AESGameModeBase::LogDialogSubtitle** @ `01928210` unique=True — Cosmetic mission-log writer only: constructs an FDialogLogEntry and appends it to the PlayerData cached at gamemode+0x518, i.e. UPlayerData::DialogLog (+0x19D8, Num at +0x19E0). Host-only because the GameMode does not exist on the client; the client's dialog log stays empty unless the mod mirrors entries. Not the dialog trigger.
  - `private: void __cdecl AESGameModeBase::LogDialogSubtitle(class FName const &, class FText, enum EDialogBehavior::Type, enum EDialogSoundType)`
  - ABI: RCX=this, RDX = const FName*, R8 = FText (16 bytes -> BY HIDDEN POINTER), R9D = EDialogBehavior, EDialogSoundType on the stack.
- **ULootCollectComponent::OnBeginOverlapCollect** @ `01504334` unique=True — PROOF that loot collection is local-player-0 only: 'if (!this->bEnabled(+0x250)) return; pawn = GetPlayerController(world,0)->GetPawnOrSpectator(); if (this->OwnerPrivate(+0x90) != pawn) return;' then checks APickupBase::CanAutoCollectPickup (01958D5C) and appends the pickup to PickupsInRange (this+0x240). On a listen server the host's player-0 pawn is the only pawn that can ever satisfy this test
  - `protected: void __cdecl ULootCollectComponent::OnBeginOverlapCollect(class UPrimitiveComponent *, class AActor *, class UPrimitiveComponent *, int, bool, struct FHitResult const &)`
  - ABI: Bound as a dynamic component-overlap delegate. RCX=this, RDX=OverlappedComp, R8=OtherActor (the APickupBase), R9=OtherComp, then int/bool/FHitResult& on the stack.
- **APickupBase::OnCollect** @ `016B71B0` unique=True — The universal 'this pickup was collected by actor X' entry point. Called from APickupBase::InteractCollect (016B7104) and from the auto-collect tick path.
  - `public: int __cdecl APickupBase::OnCollect(class AActor *, bool)`
  - ABI: BlueprintNativeEvent WRAPPER: RCX=this, RDX=AActor* Collector, R8B=bool. It does FindFunctionChecked("OnCollect"), checks the UFunction's native flag, and either calls vtable[229] (0x728) directly or goes through ProcessEvent. Hook HERE (not the _Implementation) to catch collections even when a Blue
- **APickupBase::OnCollect_Implementation** @ `016B720C` unique=True — The real collection logic. HUDMarker->SetHiddenInHUD(false) (016B8CC0); removes itself from AESPlayerController::PickupsBeingPulled (+0x9E0); item = PickupEntry.PickupInventory(this+0x388)->GetFirstCargoItem (0153F744); compares Collector against UGameplayStatics::GetPlayerPawn(world,0) to decide the 'is the player' flag; adds the item to Collector(AESPawn)->ShipData.Inventory (pawn+0x4C8) via UIn
  - `public: virtual int __cdecl APickupBase::OnCollect_Implementation(class AActor *, bool)`
  - ABI: vtable slot [229] on APickupBase (matches the 0x728 indirect call in the wrapper). RCX=this, RDX=Collector, R8B=bool. Per the project's ABI rule, call it through the vtable slot rather than by RVA if you ever need to invoke it.
- **ULootDropComponent::DropLoot** @ `016CCDC4` unique=True — Rolls the loot table (FRandomStream::GenerateNewSeed), gates on bOnlyDropLootWhenPlayerIsCloseOrWhenDamagedByPlayer using UGameplayStatics::GetPlayerPawn(world,0) + AActor::GetDistanceTo (another local-player-0 hard-coding that breaks for a distant client), then calls UGameplayLib::SpawnPickups (015036D8) and records the results in LastSpawnedPickups (this+0x110). LootToDrop is the TArray<FPickupE
  - `public: void __cdecl ULootDropComponent::DropLoot(bool)`
  - ABI: RCX=this, DL=bool. Note: ULootDropComponent::TickComponent is at 0192E204 which is ICF-FOLDED across 10 different TickComponent bodies — NEVER hook 0192E204.
- **UGameplayLib::SpawnPickups** @ `015036D8` unique=True — THE universal pickup spawner: LoadClass<APickupBase>, BeginDeferredActorSpawnFromClass + FinishSpawningActor per entry. Plain deferred SpawnActor with no authority check, so a client can call it to materialise mirrored loot locally. Post-hook it on the host to capture every pickup that appears in the world.
  - `public: static bool __cdecl UGameplayLib::SpawnPickups(class UObject *, class TArray<struct FPickupEntry, class TSizedDefaultAllocator<32>> const &, struct UE::Math::TVector<double> const &, struct UE::Math::TRotator<double> const &, class TArray<class APickupBase *, class TSizedDefaultAllocator<32>> &)`
  - ABI: Static. RCX=WorldContext, RDX=const TArray<FPickupEntry>* (by hidden pointer), R8=const FVector* (24 bytes, by hidden pointer), R9=const FRotator* (by hidden pointer), out TArray<APickupBase*>& on the stack.
- **UGameplayLib::SpawnPickupFromItem** @ `05E3EC44` unique=True — Builds an FPickupEntry from the item (FPickupEntry::FPickupEntry(UItem*) 018F1D80), resolves the pickup class path via UItemLib::GetPickupClassPathForItemCategory, then delegates to SpawnPickups. The convenient client-side call for 'spawn the loot the host told me about'.
  - `public: static class APickupBase * __cdecl UGameplayLib::SpawnPickupFromItem(class UObject *, class UItem *, struct UE::Math::TVector<double> const &, struct UE::Math::TRotator<double> const &, bool &)`
  - ABI: Static. RCX=WorldContext, RDX=UItem*, R8=const FVector*, R9=const FRotator*, bool& out on the stack. Sibling SpawnPickupFromItemID(UObject*, FName, FVector const&, int, FRotator const&, ...) is at 05E3EDB0.
- **UGameplayLib::IsPlayerCurrentlyPullingLoot** @ `05E34C68` unique=True — Returns AESPlayerController::PickupsBeingPulled(+0x9E0).Num() > 0 for LOCAL player 0 only (via GetESPlayerController 0150D280). Purely local UI/state; each machine answers about its own player, which is already the correct co-op behaviour.
  - `public: static bool __cdecl UGameplayLib::IsPlayerCurrentlyPullingLoot(class UObject const *)`
  - ABI: RCX=WorldContext.
- **UInventory::AddItem** @ `0153C8FC` unique=True — Low-level inventory insert. Prefer UInventoryLib::AddItemToRespectiveInventory for the client-side 'give item' path since it picks the right container.
  - `public: int __cdecl UInventory::AddItem(class UItem *, class TEnumAsByte<enum EInventoryCategory::Type>, int, bool, bool)`
  - ABI: RCX=this, RDX=UItem*, R8B=EInventoryCategory (1 byte), R9D=int Amount, two bools on the stack. EInventoryCategory::Type = {PrimaryWeapon 0, SecondaryWeapon 1, EnergyCore 2, Sensor 3, Shield 4, CargoUnit 5, Plating 6, Thrusters 7, Device 8, Consumable 9, Cargo 10, INVALID 11}. Returns the amount actu
- **UInventoryLib::AddItemToRespectiveInventory** @ `05E47738` unique=True — THE client-side 'give this player the item' entry point. GetPlayerData(), then routes: IsItemStorableInCraftingInventory (016B6878) -> crafting inventory; IsItemIDStorableInQuestInventory (05E4D53C) -> quest inventory; otherwise GetInventoryOfCurrentShip (014DE7B4) + UInventory::AddItem. Operates entirely on the LOCAL UPlayerData, which is exactly the per-player-inventory semantics we want.
  - `public: static bool __cdecl UInventoryLib::AddItemToRespectiveInventory(class UItem *, bool)`
  - ABI: Static, RCX=UItem*, DL=bool (show message). Reflected -> also ProcessEvent-callable on the UInventoryLib CDO.
- **UItem::GetItemState** @ `05E9B940` unique=True — Serialises a live UItem into the plain-data FItemState (template id, seed, name seed, level, rarity, amount, ammo, condition, affix, grade, chips, catalysts, attribute states). The wire format for 'player N picked up item I'.
  - `public: struct FItemState __cdecl UItem::GetItemState(void)`
  - ABI: RCX = hidden return pointer for the 464-byte FItemState, RDX = this. NOT a reflected UFunction — must be called by RVA.
- **UItem::CreateItemFromState** @ `05E98820` unique=True — NewObject<UItem> + applies the state (with UItemLib::TransformDeprecatedIDs for legacy ids). The client-side other half of the loot wire protocol; feed the result to UInventoryLib::AddItemToRespectiveInventory or UGameplayLib::SpawnPickupFromItem.
  - `public: static class UItem * __cdecl UItem::CreateItemFromState(struct FItemState const &)`
  - ABI: Static; RCX = const FItemState* (464 bytes, BY HIDDEN POINTER). NOT reflected — call by RVA.
- **UScriptStruct::ExportText** @ `02D01634` unique=True — Generic reflection-driven struct <-> text marshalling. The lowest-effort robust wire format for FTaskSaveGameData and FItemState across the existing string-based RPC channel, instead of hand-marshalling nested TArray/TMap/FString members.
  - `public: void __cdecl UScriptStruct::ExportText(class FString &, void const *, void const *, class UObject *, int, class UObject *, bool) const`
  - ABI: Paired with UScriptStruct::ImportText at 02D06F3C. FItemState::StaticStruct is 05BF898C; FTaskSaveGameData has no custom serializer (its TCppStructOps::Serialize is the ICF'd no-op stub at 01300270), so generic reflection-based text marshalling is safe.
- **UXPComponent::OwnerHealthDepleted** @ `019423AC` unique=True — THE on-kill XP award. Sums max hitpoints across Health/Armor/Shield components, divides the player's damage share (UFactionComponent::FactionEntries[0]+0xC) by it and compares to NeededPlayerDamageRatio (this+0xA8); scales XP (this+0xA0) by the player-level vs pawn-level delta (UPlayerData+0x128 vs pawn+0x1038); then AESHUD::ShowXPNumbers (05AF7AE4) and UGameplayLib::AddXP (05E209B8). The GetESGam
  - `public: void __cdecl UXPComponent::OwnerHealthDepleted(class AActor *, class AActor *, class AController *)`
  - ABI: Bound as a dynamic delegate on the owner's health component. RCX=this (UXPComponent, on AESPawn+0x3B8), RDX=DamagedActor, R8=DamageCauser, R9=InstigatedBy. The XP award itself happens in the outlined cold block at 02B2558C..02B2572F (llvm mis-attributes that range to icu_64::Normalizer2::getCombinin
- **UGameplayLib::AddXP** @ `05E209B8` unique=True — Adds XP to the LOCAL UPlayerData and handles level-up. The client calls this directly with the amount mirrored from the host to get shared XP with independent levels.
  - `public: static bool __cdecl UGameplayLib::AddXP(float, bool, bool, float)`
  - ABI: Static. XMM0 = float XP, DL = bHideLevelUpAnimation, R8B = bIgnoreLevelCap, XMM3 = float (4th). It reloads the global UESGameInstance* (RVA 09AC5E88) -> +0x1C0 and forwards to UPlayerData::AddXP (05DFA9F8, RCX=this, XMM1=XP, R8B, R9B, [rsp+0x20]=float). Reflected param names in the PDB: bHideLevelUp
- **AESPawn::RemoveDropAndXP** @ `0193E9A0` unique=True — A SUPPRESSOR, not an awarder: bRemoveDrop empties LootDrop(pawn+0x3A8)->LootToDrop(+0x120); bRemoveXP zeroes XP(pawn+0x3B8)->XP(+0xA0). Useful for the mod to guarantee a pawn yields no duplicate loot/XP on a machine that should not award it.
  - `public: void __cdecl AESPawn::RemoveDropAndXP(bool, bool)`
  - ABI: RCX=this (AESPawn*), DL=bRemoveDrop, R8B=bRemoveXP (reflected prop names confirmed: NewProp_bRemoveDrop, NewProp_bRemoveXP).
- **UGameplayLib::GetPlayerLevel** @ `05E2E478` unique=True — Reads UPlayerData+0x128 (PlayerLevel) from the local process singleton. Per-player by construction — each side reports its own level, which is the desired co-op behaviour.
  - `public: static int __cdecl UGameplayLib::GetPlayerLevel(void)`
  - ABI: No args. Sibling GetMaxPlayerLevel at 019424C0.
- **UGameplayLib::GetPlayerData** @ `0127398C` unique=True — The root of all per-player state. The mod should read/write this directly rather than hooking it.
  - `public: static class UPlayerData * __cdecl UGameplayLib::GetPlayerData(void)`
  - ABI: No args. Body is 4 instructions: load the global UESGameInstance* at RVA 09AC5E88, deref +0x1C0, null-guard. Both host and client have their own.
- **UMissionLib::AddNonItemRewards** @ `05E71518` unique=True — Grants mission rewards: UGameplayLib::AddXP (05E209B8), UGameplayLib::ChangeCredits (05E21DF0), UMissionLib::IncJobScore (05E80918), faction standing, and decal unlocks via AMissionBase::GetRewardDecalIDs + UPlayerData::UnlockDecal. Hook on the host to replay the same reward set on the client so both players level and earn together.
  - `public: static void __cdecl UMissionLib::AddNonItemRewards(struct FMissionRewards const &, class TEnumAsByte<enum EMissionType::Type> const &, class TEnumAsByte<enum EFactionGroup::Type> const &, class FName const &)`
  - ABI: Static. RCX = const FMissionRewards* (32 bytes, BY HIDDEN POINTER), RDX/R8 = pointers to the enum bytes, R9 = const FName*. The BP-facing variant is UMissionLib::AddNonItemRewardsByUI (exec at 05CA0514, reflected).
- **UGameplayLib::ChangeCredits** @ `05E21DF0` unique=True — Mutates UPlayerData::Credits (+0x120). Per-player; the client calls it locally to mirror a mission credit reward.
  - `public: static void __cdecl UGameplayLib::ChangeCredits(int, enum ECreditsTransferType::Type, bool, bool)`
  - ABI: Static. ECX = int Delta, EDX = ECreditsTransferType {Undefined 0, Restock 1, Repair 2, BuyOrSellItem 3, BuyOrSellShip 4, JobReward 5, Collected 6, ChallengeReward 7, Perk 8, DeviceReset 9}, R8B/R9B bools. Forwards to UPlayerData::ChangeCredits (016B5A6C).
- **UMissionLib::RemoveTaskFromPlayerData** @ `016C41CC` unique=True — Deletes a record from UPlayerData::MissionSaveState and calls UMapLib::RefreshMissionAndWaypointIndicators (016C9C58) at +0x1fb. Also reached from UMissionLib::SetMissionRewardsClaimed (05E83530). This is the 'task disappeared' delta the client must also apply.
  - `public: static void __cdecl UMissionLib::RemoveTaskFromPlayerData(class FName, bool)`
  - ABI: Static, RCX = FName by value, DL = bool.
- **USelfRegisteringComponent::GetAllActorsOfRegister** @ `0150FDA8` unique=True — O(1) actor enumeration by category. Relevant ids: Pawn=2, HUDMarkerComponent=8, Pickup=9, Container=13, Mission=14, MissionTaskBase=15, JumpTarget=11, Challenge=33, MissionItem=38, MissionItemPOI=39. The mod's cheapest way to enumerate pickups/mission actors on either side.
  - `public: static class TArray<class AActor *, class TSizedDefaultAllocator<32>> const & __cdecl USelfRegisteringComponent::GetAllActorsOfRegister(enum ERegisterIds::Type)`
  - ABI: ECX = ERegisterIds::Type. Returns &Global[id] where the global TArray<TArray<AActor*>> lives at RVA 09BAA730 (Num at 09BAA738). Per-process registry, populated on both sides for whatever actors exist locally (including replicated ones, since registration happens in component BeginPlay).
- **AESGameModeBase::GetESGameMode** @ `01505960` unique=True — Returns nullptr on any network client. This is the proof that ALL AESGameModeBase-driven world content (missions, challenges, perks, objective display, dialog logging) is inherently host-only in the existing listen-server setup — no suppression work required.
  - `public: static class AESGameModeBase * __cdecl AESGameModeBase::GetESGameMode(class UObject const *)`
  - ABI: RCX = WorldContext. Calls UGameplayStatics::GetGameMode (0145A9AC) which is literally 'UWorld+0x158' (AuthorityGameMode) with a class check.

## Types

### FTaskSaveGameData (size 320)
- 0x0000 TaskClass UClass* — resolved class; do not send over the wire
- 0x0008 TaskSoftClass TSoftClassPtr<AMissionTaskBase> (40 bytes = FSoftObjectPtr; FSoftObjectPath = FTopLevelAssetPath(16) + FString SubPathString(16)) — THIS is what the client needs to resolve the task class for text/CDO lookups; send the asset path string
- 0x0030 TaskID FName — the record key; UMissionLib::FindTaskInPlayerData matches on this
- 0x0038 TaskState TEnumAsByte<EMissionTaskState::Type> — the completion field (6 = Succeeded)
- 0x0039 TaskDisplayType TEnumAsByte<EMissionTaskDisplayType::Type>
- 0x003C Stage int — mission stage
- 0x0040 Progress int — objective counter
- 0x0044 SpawnedDuringMissionStage int
- 0x0048 DifficultyLevel int
- 0x004C TimeLimitType TEnumAsByte<EMissionTaskTimeLimit::Type>
- 0x0050 TimeLimit float
- 0x0054 TimeRemaining float
- 0x0058 TimeStamp float
- 0x005C bIsMission bool
- 0x0060 LocationID FName — drives mission markers/indicators via GetActiveTasksLocations
- 0x0068 StationID FName
- 0x0070 JobOfferStationID FName
- 0x0078 ChildTaskIDs TArray<FName> — objective tree for the mission log
- 0x0088 Attributes TMap<FName,FESVariant> (80 bytes) — mission BP scratch state
- 0x00D8 Rewards FMissionRewards (32 bytes)
- 0x00F8 bHideInMissionLog bool
- 0x00F9 bShowExclusivelyInMissionLog bool
- 0x00FA bIsHidden bool
- 0x00FB MissionFaction TEnumAsByte<EFactions::EFaction>
- 0x00FC MissionFactionGroup TEnumAsByte<EFactionGroup::Type>
- 0x00FD EnemyFaction TEnumAsByte<EFactions::EFaction>
- 0x0100 NPCSpawns FNPCSpawns (32 bytes)
- 0x0120 bIsDynamic bool
- 0x0128 SerializedTask TArray<uint8> — opaque BP-serialised blob; potentially large, exclude from per-frame deltas
- 0x0138 UE5SerializeEngineVer unsigned
- 0x013C bHideInitially bool

### AMissionTaskBase (size 1008)
- (derives AActor at offset 0; bReplicates is AActor+0x5B bit3 and the C++ ctor at 01833778 NEVER touches it — mission tasks do not replicate)
- 0x02A8 SelfRegistering USelfRegisteringComponent* — registers into ERegisterIds::MissionTaskBase (15)
- 0x02B0 MissionTaskID FName — key into UPlayerData::MissionSaveState
- 0x02B8 LocationID FName
- 0x02C0 StationID FName
- 0x02C8 JobOfferStationID FName
- 0x02D0 ParentTask AMissionTaskBase*
- 0x02D8 TaskState TEnumAsByte<EMissionTaskState::Type> — written by SwitchState_Internal
- 0x02E0 ObjectiveText FText
- 0x02F0..0x02F6 bIsOptional / bIsHidden / bHideInMissionLog / bShowExclusivelyInMissionLog / bFailIfPlayerLeaves / bFailIfPlayerTurnsWanted / bCantLeaveWhileActive
- 0x02F7 TaskDisplayType TEnumAsByte<EMissionTaskDisplayType::Type>
- 0x02F8 TimeLimit float
- 0x02FC TimeLimitType TEnumAsByte<EMissionTaskTimeLimit::Type>
- 0x0300 ProgressValue int — written by SetProgress/IncProgress
- 0x0304 StageValue int — written by EnterStage_Internal
- 0x0308 SpawnedDuringMissionStage int
- 0x030C TimeRemaining float
- 0x0310 TimeStamp float — stamped from UPlayerData+0x2130
- 0x0318 Attributes TMap<FName,FESVariant>
- 0x0368 ChildTaskIDs TArray<FName>
- 0x0378 OnTaskActivated FTaskEventSignature
- 0x0388 OnTaskSucceeded
- 0x0398 OnTaskFailed
- 0x03A8 OnTaskEnded
- 0x03B8 OnTaskProgressed FTaskProgressEventSignature
- 0x03C8 OnStageChanged FTaskStageEventSignature
- 0x03D8 OnTimeExpired
- 0x03E8 bEnableUpdatesOfPlayerData bool — UpdateTaskInPlayerData early-outs if this is 0
- 0x03E9 bIsDynamic / 0x03EA bHideInitially / 0x03EB bSuppressBeginPlay / 0x03EC bInstanceWasRestored
- vtable[238] IsMission (0x770), [239] GetSaveGameData (0x778), [240] ApplySaveGameData (0x780), [241] GetOwningMissionTitle (0x788), [242] GetOwningMissionID (0x790)

### AMissionBase (size 1208)
- (derives AMissionTaskBase)
- 0x0400 mission-type byte — read by UpdateTaskInPlayerData and SwitchState_Internal
- 0x0418 FMissionRewards — copied from the CDO on terminal state transitions

### FHUDTaskData (size 376)
- 0x0000 TaskData FTaskSaveGameData (320 bytes)
- 0x0140 OwningMissionID FName — filled from vtable[242]
- 0x0148 TaskID FName — filled from task+0x2B0
- 0x0150 MissionTitle FText — filled from vtable[241]
- 0x0160 ObjectiveText FText — filled from AMissionTaskBase::GetObjectiveDescription (01791690)
- 0x0170 bIsNewTask bool
- 0x0171 bStateChange bool
- 0x0172 bProgressChange bool
- 0x0173 bStageChange bool
- 0x0174 bPermanentlyDisplay bool
- (ctor 01311E0C, dtor 017915B8, operator= 017914A0 — all unique)

### FMissionRewards (size 32)
- 0x0000 XP int
- 0x0004 Credits int
- 0x0008 OkkarCredits int
- 0x000C Standing int
- 0x0010 Items TArray<FItemContainerContent> — note: UMissionLib::AddNonItemRewards deliberately does NOT grant these; item rewards are handed out elsewhere (see open questions)

### FObjectiveDescription (size 32)
- 0x0000 TaskState TEnumAsByte<EMissionTaskState::Type>
- 0x0008 Description FText
- 0x0018 LocationID FName
- (returned by UMissionLib::GetCurrentObjectivesForMissionLog, ctor 05CA7600)

### FMissionIDsArray (size 16)
- 0x0000 NameArray TArray<FName> — trivial wrapper used for TMap values of mission id lists

### UPlayerData (size 8624)
- (derives USaveGame; process singleton reached via UGameplayLib::GetPlayerData 0127398C = *(RVA 09AC5E88)->+0x1C0)
- 0x0120 Credits int
- 0x0124 OkkarCredits int
- 0x0128 PlayerLevel int — compared against UGameplayLib::GetMaxPlayerLevel in the XP path
- 0x012C XP float
- 0x0130 StationCargo UInventory*
- 0x01E8 QuestItems UInventory* — mission item destination
- 0x02A0 CraftingResources UInventory*
- 0x03F8 MissionSaveState TArray<FTaskSaveGameData> — THE mission state store (Num at +0x400, Max at +0x404, stride 0x140). Mirror this to the client.
- 0x0468 Ships TArray<FShipData> — per-player ships; UInventoryLib::GetPlayerShip(idx) reads this
- 0x0AF0 MissionCheckpointTaskID FName / 0x0AF8 MissionCheckpointID / 0x0B00 MissionCheckpointLocationID
- 0x0B58 DialogCounterSaveState TMap<FName,int> — per-player dialog play counts, gates DialogCounter::TryInc
- 0x0C98 DockableStationJobOffers TArray<FJobOffersSaveState>
- 0x1438 CompletedMissions TArray<FName> — mirror this
- 0x1458 Challenges TArray<FChallengeState>
- 0x119C CurrentShip int
- 0x160C TrackedMainMission FName / 0x1614 TrackedSideMission / 0x161C TrackedJob / 0x1624 TrackedChallenge — read by UMapLib::RefreshMissionAndWaypointIndicators; mirror these
- 0x1604 CustomWaypoint FName
- 0x1698 FactionGroupStandings TMap<EFactionGroup,FFactionGroupStanding>
- 0x16E8 JobScore FJobScore
- 0x19D8 DialogLog TArray<FDialogLogEntry> (Num at +0x19E0) — written only by the host's AESGameModeBase::LogDialogSubtitle
- 0x2130 PlayingTimeSecondsDouble double — the clock mission timestamps are stamped from

### ULootCollectComponent (size 608)
- (derives USceneComponent)
- 0x0090 OwnerPrivate AActor* (UActorComponent) — compared against GetPlayerController(0)->GetPawnOrSpectator() in OnBeginOverlapCollect: THE local-player-0 lock
- 0x0230 CollectSphere USphereComponent*
- 0x0238 CollectRadius float
- 0x0240 PickupsInRange TArray<APickupBase*>
- 0x0250 bEnabled bool — first early-out in OnBeginOverlapCollect; a cheap host-side kill switch

### ULootDropComponent (size 336)
- 0x00A0 RandomLootFromTable TEnumAsByte<ELootType::Type>
- 0x00A4 LootID FName
- 0x00B0 FixedLootEntries TArray<FFixedLootEntry>
- 0x00C0 SpawnRadius float / 0x00C4 SpawnRadiusMin
- 0x00D9 bOnlyDropLootWhenPlayerIsCloseOrWhenDamagedByPlayer bool — the gate that uses GetPlayerPawn(world,0); force it false on the host so loot still drops near a remote client
- 0x0108 LootRarityBonus float / 0x010C LootQuantityFactor float
- 0x0110 LastSpawnedPickups TArray<APickupBase*> — read this in a post-hook of DropLoot to learn what was created
- 0x0120 LootToDrop TArray<FPickupEntry> — cleared by AESPawn::RemoveDropAndXP
- 0x0130 OnPickupsSpawned FOnPickupsSpawned — an alternative observation point
- 0x0140 bEnableLootDrop bool / 0x0141 bLootDropped bool

### APickupBase (size 968)
- (derives AActor; ctor 01717C64 never writes AActor+0x5B, so bReplicates is FALSE by default — pickups do not replicate)
- 0x02B0 SelfRegistering USelfRegisteringComponent* — ERegisterIds::Pickup = 9
- 0x02C0 HUDMarker UHUDMarkerComponent*
- 0x02C8 Interact UInteractComponent* / 0x02D0 InteractPull
- 0x0308 OnPickupContentsChanged FOnPickupContentsChangedDelegate
- 0x0318 OnPickedUpByPlayer FOnPickedUpByPlayerDelegate — broadcast(pickup, amount, bFullyCollected) at the end of OnCollect_Implementation
- 0x0378 PickupEntry FPickupEntry (24 bytes: FString PickupClassPath at +0, UInventory* PickupInventory at +0x10 -> absolute this+0x388)
- 0x0390 PreLootScreenItem UItem*
- 0x0399 bPlayerPullsLoot bool
- 0x03A0 HUDInfoArray TArray<FPickupHUDInfo>
- vtable[229] OnCollect_Implementation, [230] GetPickupName_Implementation

### FItemState (size 464)
- 0x0000 ItemTemplateID FName
- 0x0008 Seed int / 0x000C NameSeed int — items are seed-generated, so these plus the template reproduce the roll
- 0x0010 ItemLevel int / 0x0014 VirtualLevelOffset float
- 0x0018 Rarity TEnumAsByte<EItemRarity::Type>
- 0x001C Amount int / 0x0020 Ammo int
- 0x002C Condition float / 0x0030 CooldownRemaining / 0x0034 CooldownLastSet / 0x0038 TimestampInventoryAdd
- 0x0040 InstalledChipIDs TArray<FName> / 0x0050 Catalysts TArray<FName>
- 0x0060 AffixID FName / 0x0068 Grade TEnumAsByte<EItemGrade::Type>
- 0x0070 AttributeStates TArray<FItemAttributeState>
- 0x0080 AttributeOriginalPositionInRange TMap<FName,float>
- (round-trip: UItem::GetItemState 05E9B940 -> UItem::CreateItemFromState 05E98820; ctor 05BC565C, dtor 05AA1010, StaticStruct 05BF898C)

### FShipData (size 976)
- 0x0000 Name FText
- 0x0010 Inventory UInventory* — at AESPawn+0x4C8 (ShipData is AESPawn+0x4B8). THIS is where APickupBase::OnCollect_Implementation puts collected items.
- 0x0018 ShipItemInstance UItem* / 0x0020 UltimateDevice UItem*
- 0x0028 ShipModules TArray<FShipModuleState>
- 0x0038 AppearanceID FName
- 0x0360 HealthRatio float / 0x0364 ArmorRatio / 0x0368 UltimateRatio
- (UGameplayLib::SpawnPlayerShip 05E3EE5C copies this from UInventoryLib::GetPlayerShip(idx) 01805EE0 = the LOCAL UPlayerData::Ships)

### UXPComponent (size 168)
- 0x00A0 XP float — the award amount; scaled in place by OwnerHealthDepleted, zeroed by AESPawn::RemoveDropAndXP
- 0x00A4 GainCondition TEnumAsByte<EGainCondition::Type>
- 0x00A8 NeededPlayerDamageRatio float — minimum damage share the player must have dealt
- (lives at AESPawn+0x3B8; ULootDropComponent at AESPawn+0x3A8)

### UConditionBaseComponent (size 360)
- 0x00B4 Stacktype TEnumAsByte<EConditionStacktype::Type> {Invalid 0, Intensity 1, Duration 2, MaxIntensity 3, MaxInstigatorLevel 4}
- 0x00C8 ConditionId FName / 0x00D0 ConditionName FText / 0x00E0 Description FText
- 0x00F8 bIsDebuff bool / 0x00F9 bIsImpairingMovement / 0x00FA bShowConditionInHud / 0x00FC bCanBeRemoved
- 0x0100 OnConditionAdded / 0x0110 OnConditionRemoved / 0x0120 OnConditionTick FConditionDelegate
- 0x0130 TokenLimit int / 0x0134 bIsBuildupCondition bool
- 0x0158 ConditionStack TArray<FToken>
- VERDICT: this is the status-effect/buff-debuff framework (subclasses UBuildupConditionComponent, UConditionEmitterComponent, UConditionInstigatorComponent), NOT a mission-condition system. It never reads UPlayerData. Belongs to P4 Combat.

### UDialogManager (size 608)
- 0x0028 DialogIDs UDialogIDs*
- 0x0033 bIsSuppresed bool — SetSuppressionAllDialogManager (01840B6C) flips this; a blunt client-side mute switch
- 0x0034 bIsMenuDialogManager / 0x0035 bIsCinematicDialogManager
- 0x0050 DialogQueue TArray<DialogQueueEntry>
- 0x0060 ParagraphQueue TArray<ParagraphQueueEntry>
- 0x0070 CurrentSpeakerID FName / 0x0078 CurrentSubtitleText FText / 0x0088 CurrentDialogSoundType EDialogSoundType
- 0x01C0 DialogStarted / 0x01D0 DialogFinished / 0x01E0 CurrentDialogSkipped FDialogEvent_Multicast
- 0x0210 SubtitleLineStarted FDialogSubtitleLineEvent_Multicast — what AESGameModeBase::LogDialogSubtitle binds to
- 0x0258 CurrentAudioComp UAudioComponent*
- (two process globals: game manager at RVA 09ACBBD0, menu manager at RVA 09ACBBD8)


## Hook plan

- **UMissionLib::UpdateTaskInPlayerData** @ `015124A0` [host] — The one and only mission-progress event source. A full .text xref scan proves all 13 mutation paths (progress, stage, state, station, location, attributes, save-sweep) pass through here.
  - behaviour: Observe (call original first, then read state). Post-hook: from the AMissionTaskBase* in RCX read MissionTaskID(+0x2B0), TaskState(+0x2D8), ProgressValue(+0x300), StageValue(+0x304), LocationID(+0x2B8), StationID(+0x2C0), bIsHidden(+0x2F1); diff against a per-task cache and, on change, queue a TASK_DELTA{TaskID, State, Stage, Progress, LocationID, StationID} to every client. Do NOT mutate arguments.
  - risk: Called at high frequency during scripted sequences (SetAttribute goes through it too) — must be dirty-diffed, not blindly forwarded. The task actor may be mid-construction; guard against a null/garbage RCX and against re-entrancy from your own broadcast. Never install this hook on the client (it would fire from the mod's own writes and echo back).
- **AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors** @ `05DE9610` [host] — Deliver a full mission snapshot at level load and to joiners, so a client that arrives mid-mission gets consistent state instead of only deltas.
  - behaviour: Post-hook. After the original returns (SpawnActiveMissions/SpawnPerks/SpawnChallenges have run), snapshot UPlayerData::MissionSaveState (+0x3F8) plus TrackedMainMission/TrackedSideMission/TrackedJob (+0x160C/+0x1614/+0x161C) and CompletedMissions (+0x1438), and mark the snapshot for send-on-join. Use UMissionLib::GetAllMissionsInPlayerData (015B3484) if you want a filtered copy rather than raw array access.
  - risk: Fires only on the host and only once per level load, so it cannot cover join-in-progress on its own — pair it with an explicit resend when AGameModeBase::PostLogin runs. Snapshotting the full FTaskSaveGameData including SerializedTask (+0x128, an opaque TArray<uint8>) can be large; strip that member from the wire form.
- **UMissionLib::RemoveTaskFromPlayerData** @ `016C41CC` [both] — Mission tasks that disappear (completed-and-claimed, abandoned) must disappear on the client too; and the client must not be able to delete host-authoritative state on its own.
  - behaviour: Host: observe, broadcast TASK_REMOVED{TaskID}. Client: block — swallow calls that did not originate from the mod's own delta applier (use a thread-local re-entrancy flag), and instead forward an abandon/claim REQUEST to the host.
  - risk: Also invoked from UMissionLib::SetMissionRewardsClaimed (05E83530) and from mission-log UI paths, so a naive client-side block can wedge the client's own mission-log UI. Verify live which UI buttons reach it.
- **UDialogManager::EnqueueDialog** @ `01841D10` [both] — Single choke point for every dialog line — makes both players hear the same dialog.
  - behaviour: Host: observe, broadcast DIALOG{DialogID(FName as string), Delay(float), EDialog::Type, EDialogBehavior::Type, bPlayOnce}. Client: block dialogs not originating from the mod's own applier (guard flag), so client-local BP/UI cannot desync the conversation; the applier then calls the reflected static UDialogManager::EnqueueMissionDialog (01840B90) or EnqueueDialog (01840F9C).
  - risk: The 4th argument is a float in XMM3 while the 3rd is a pointer in R8 — get the trampoline signature wrong and the delay is garbage. Menu/UI dialogs go through the same singleton accessor with bMenu=true; filter on the manager instance (game global RVA 09ACBBD0) so you do not broadcast menu chatter. Blocking on the client also suppresses purely local UI voice lines — start with observe-only on the client and tighten later.
- **UXPComponent::OwnerHealthDepleted** @ `019423AC` [host] — Award the same kill XP to both players while keeping levels per-player.
  - behaviour: Post-hook. After the original runs, read the (already level-scaled) XP at UXPComponent+0xA0 and, if > 0, broadcast XP_AWARD{amount}. Clients apply it with a direct call to UGameplayLib::AddXP(amount, false, false, 0.f) (05E209B8).
  - risk: The award decision uses the HOST player's damage share (UFactionComponent::FactionEntries[0]+0xC vs NeededPlayerDamageRatio at +0xA8) — kills made entirely by the client may award zero on the host and therefore zero to everyone. Fixing that properly needs the P4 damage-attribution work; as an interim, treat any kill by a co-op pawn as qualifying. Also note the actual AddXP call lives in an outlined cold block (02B2558C..02B2572F) that llvm mis-attributes to icu_64::Normalizer2::getCombiningClass — hook the entry at 019423AC, not the cold chunk.
- **UMissionLib::AddNonItemRewards** @ `05E71518` [host] — Mission XP / credits / job score / faction standing must land on both players, not just the host.
  - behaviour: Observe. Read the FMissionRewards via the hidden pointer in RCX (XP int at +0x00, Credits +0x04, OkkarCredits +0x08, Standing +0x0C) and broadcast REWARDS{XP, Credits, OkkarCredits, Standing, MissionType, FactionGroup, MissionID}. Client applies with UGameplayLib::AddXP (05E209B8) + UGameplayLib::ChangeCredits (05E21DF0, ECreditsTransferType::JobReward=5) + UMissionLib::IncFactionGroupStanding (05E8089C) + UMissionLib::IncJobScore (05E80918).
  - risk: First argument is a 32-byte struct BY HIDDEN POINTER; the two enum arguments are also passed as const references (pointers to single bytes), not by value — misreading them yields nonsense faction groups. Decal unlocks inside the original are cosmetic and per-player; decide whether to mirror them.
- **UGameplayLib::SpawnPickups** @ `015036D8` [host] — Universal capture point for every pickup that enters the world (loot drops, mission items, container contents) so clients can materialise their own local copies.
  - behaviour: Post-hook. On success, walk the out-parameter TArray<APickupBase*> (5th arg, on the stack) and for each pickup read PickupEntry.PickupInventory (this+0x388) -> UInventory::GetFirstCargoItem (0153F744) -> UItem::GetItemState (05E9B940), then broadcast PICKUP_SPAWN{serverPickupId, FItemState, FVector location, FRotator rotation}. Assign each spawned pickup a mod-side id and keep a host-side map id -> APickupBase*.
  - risk: Marshalling FItemState (464 bytes with nested TArray/TMap/FString) by hand is error-prone — prefer UScriptStruct::ExportText (02D01634) / ImportText (02D06F3C) against FItemState::StaticStruct (05BF898C). High spawn bursts on big kills need batching. Also do not hook this on the client, or the client's own mirrored spawns will loop.
- **ULootDropComponent::DropLoot** @ `016CCDC4` [host] — Make loot actually drop for kills that happen far from the host, and get an explicit per-kill loot event.
  - behaviour: Pre-hook: if the component's bOnlyDropLootWhenPlayerIsCloseOrWhenDamagedByPlayer (this+0xD9) is set and no co-op player is nearby, temporarily clear it so the vanilla GetPlayerPawn(world,0) distance gate cannot suppress the drop; restore it after. Post-hook: read LastSpawnedPickups (this+0x110) as a secondary confirmation of what was created.
  - risk: Mutating a component property, even transiently, is riskier than the read-only SpawnPickups capture — if the mod crashes between clear and restore, the flag stays wrong on that actor. Do NOT hook ULootDropComponent::TickComponent (0192E204): that RVA is ICF-folded across 10 distinct TickComponent bodies.
- **APickupBase::OnCollect** @ `016B71B0` [both] — Arbitrate who consumes a shared pickup, and keep loot ownership consistent across machines while each player's item lands in their own UPlayerData.
  - behaviour: Both sides observe the BlueprintNativeEvent wrapper (not the _Implementation) so Blueprint pickup overrides are still caught. Client: post-hook, if the return value in EAX is > 0, send COLLECTED{serverPickupId} to the host. Host: on receiving that, destroy its authoritative pickup and broadcast PICKUP_DESTROY{serverPickupId} so other clients remove their local copies; for the host's own collections, broadcast the same. For instanced-loot mode, skip the arbitration and simply never send PICKUP_DESTROY.
  - risk: Requires a stable mod-side pickup id shared across machines (a host-assigned counter carried in the PICKUP_SPAWN message), since the actors are genuinely different UObjects on each side. Race: two players collect the same pickup within one RTT — the host must pick a winner and tell the loser to re-spawn its local copy (APickupBase::RecreatePickupsAfterGroupPull 05EB9970 exists but is for a different flow, so a plain re-SpawnPickups is simpler). If BP overrides OnCollect, ProcessEvent is used and the return value still lands in EAX via the wrapper — verify live.
- **UMapLib::RefreshMissionAndWaypointIndicators** @ `016C9C58` [client] — Re-derive world mission indicators from the mirrored UPlayerData after each mission delta, since the host-side call sites (AESGameModeBase::StartPlay etc.) never run on the client.
  - behaviour: CALL, do not hook. After the client's delta applier has written MissionSaveState and the Tracked* FNames, invoke this (directly by RVA, or via ProcessEvent — it is reflected). It reads only UPlayerData and sets EIndicator::MainMission/SideMission/Job on registered jump targets via UHUDMarkerComponent::AddOrRemoveIndicator (016C9F4C).
  - risk: Walks USelfRegisteringComponent::GetAllActorsOfRegister and does path-finding (UMapLib::GetShortestPath 016CA674) — do not call it per-delta in a burst; coalesce to at most once per frame or on a short timer.
- **AESHUD::ShowMissionTaskUpdate** @ `017913C8` [client] — Reproduce the host's objective/progress HUD toast on the client without any mission actor existing locally.
  - behaviour: CALL, do not hook. Build an FHUDTaskData (ctor 01311E0C / dtor 017915B8) from the mirrored FTaskSaveGameData: copy TaskData (0x0..0x140), set TaskID(+0x148) and OwningMissionID(+0x140), fill MissionTitle(+0x150) from UMissionLib::GetMissionTitleFromTaskData (05E7EE70) and ObjectiveText(+0x160) from UMissionLib::GetMissionObjectiveTextFromTaskData (05E7E97C, CDO-based, actor-free), set bStateChange/bProgressChange/bStageChange from the delta. Pass by hidden pointer in RDX with the AESHUD* from UGameplayLib::GetESHUD (015576B8) in RCX.
  - risk: It is a BlueprintImplementableEvent dispatched through FindFunctionChecked + ProcessEvent (vtable+0x278) — an incorrectly constructed FText or a partially initialised struct will crash inside the HUD Blueprint rather than in the mod. Always construct via the real ctor and destroy via the real dtor; never memset.
- **UInventoryLib::AddItemToRespectiveInventory** @ `05E47738` [client] — Route a host-authorised item into the correct container of the CLIENT'S OWN UPlayerData (quest items, crafting resources, or current-ship cargo).
  - behaviour: CALL, do not hook. Rebuild the item with UItem::CreateItemFromState (05E98820) from the mirrored FItemState, then call this. It resolves GetPlayerData() locally, so the item lands in the client's own inventory by construction — no cross-process inventory plumbing needed.
  - risk: UItem::GetItemState / CreateItemFromState are NOT reflected UFunctions, so they must be called by RVA with the correct hidden-pointer ABI (GetItemState: RCX = 464-byte return buffer, RDX = this). FItemState round-trip fidelity for rolled AttributeStates is unverified — test a duplicate-and-compare live before trusting it for rare gear.
- **AMissionTaskBase::SwitchState_Internal** @ `05EBD194` [host] — OPTIONAL finer-grained event than UpdateTaskInPlayerData when you want the explicit old->new state transition (for example to fire a 'mission complete' banner exactly once).
  - behaviour: Pre-hook: capture the old TaskState from this+0x2D8 and the requested new state from DL before calling the original; post-hook, emit a MISSION_STATE{TaskID, oldState, newState, stage} event. Purely additive — never alter the arguments, or the mission state machine desyncs from UPlayerData.
  - risk: Redundant with the UpdateTaskInPlayerData hook (SwitchState_Internal reaches it via BroadcastStateChange), so hooking both doubles the event rate for state changes; dedupe by task id + state. Skip this hook unless the coarse delta proves insufficient.
- **ULootCollectComponent (bEnabled at +0x250) and ULootCollectComponent::OnBeginOverlapCollect** @ `01504334` [host] — Prevent the host from silently vacuuming loot that belongs to a remote player, in shared-loot mode.
  - behaviour: Property write, hook only if needed. The vanilla code already refuses to collect for any pawn that is not GetPlayerController(0)->GetPawnOrSpectator(), so on the host only the host's own pawn auto-collects; the remote player's server-side pawn never will. If a policy of 'loot is claimed by whoever is nearest' is wanted, clear bEnabled (component+0x250) on the host's collect component while a remote player owns the drop, rather than hooking the function.
  - risk: Writing bEnabled affects the host player's own quality of life; prefer the property toggle over a hook because the function is bound as a dynamic multicast delegate and a MinHook detour there sits on a hot overlap path. Note the component is a USceneComponent subclass, so OwnerPrivate is at the UActorComponent offset +0x90 (verified), not a ULootCollectComponent-local field.

## Open questions

- Do the Blueprint pickup classes (BP_Pickup_*) set bReplicates? The C++ APickupBase ctor (01717C64) never writes AActor+0x5B, but a BP class default could. Check live with the console: dump bReplicates on a spawned pickup on the host and see whether any pickup actor appears in the client's GUObjectArray. If BP pickups DO replicate, the client will see duplicate pickups (one replicated, one mod-spawned) and the mirroring design must switch to 'let them replicate, suppress local spawn'.
- Same question for AMissionTaskBase Blueprint subclasses. The C++ ctor (01833778) does not set bReplicates and no ES2 class overrides GetLifetimeReplicatedProps, so mission actors almost certainly do not replicate — but confirm by listing actors of class AMissionTaskBase on the client after the host has spawned missions.
- Exact semantics of UMissionLib::UpdateTaskInPlayerData's two bool arguments. The PDB has no parameter names for it (it is not a reflected UFunction). Observed: all internal mutation paths pass (false,false); the reflected BP node UpdateTaskData tail-jumps in with (true,false). Reading the code, arg3 (R8B) skips the change-detection block and arg2 (DL) forces the changed flag, but this should not be relied on without a live experiment.
- Does any Blueprint on the client dereference AESGameModeBase::GetESGameMode()'s null return? The native call sites all null-check (verified in the XP path and in UMissionLib::UpdateTaskInPlayerData), but ES2's HUD and UI Blueprints may assume non-null. This is the single biggest crash risk for the client and can only be settled by running the client into a mission location and watching for crashes.
- Where are FMissionRewards::Items (offset +0x10) actually granted? UMissionLib::AddNonItemRewards deliberately excludes them, and UMissionLib::SetMissionRewardsClaimed (05E83530) only marks state and calls RemoveTaskFromPlayerData. The item grant is presumably done by the mission-log UI widget calling UInventoryLib::AddItemToRespectiveInventory directly. Find it (breakpoint or console trace on AddItemToRespectiveInventory while claiming a mission reward) before deciding whether reward items should be mirrored or claimed independently per player.
- FItemState round-trip fidelity: does UItem::CreateItemFromState(UItem::GetItemState(x)) reproduce x exactly, including rolled AttributeStates and AttributeOriginalPositionInRange? Verify live with a rare/affixed item before shipping loot mirroring, since a lossy round-trip would silently reroll gear stats for the client.
- Whether UScriptStruct::ExportText/ImportText round-trip FTaskSaveGameData cleanly given its SerializedTask TArray<uint8> member. If the text form is unusable for that member, exclude it from the wire format and confirm the client's mission-log UI does not need it (GetCurrentObjectivesForMissionLog does not appear to read it).
- Whether ULootDropComponent's kill-attribution (UFactionComponent::FactionEntries damage share) counts damage dealt by the client's server-side pawn as 'player damage'. This gates both loot drops and XP; it depends on the P4 damage-routing work and should be re-checked once client weapon fire reaches the host.
- Whether the client's dialog subtitle UI works at all without AESGameModeBase::LogDialogSubtitle running. The subtitle display itself is driven by UDialogManager::ShowSubtitleLineCallback (0183EEF8) -> SubtitleLineStarted multicast (manager+0x210), which is independent of the gamemode, so subtitles should appear — but the client's UPlayerData::DialogLog (+0x19D8) will stay empty, meaning the in-game dialog history screen is blank for the joiner unless entries are mirrored.
