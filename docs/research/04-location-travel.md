# ES2 co-op research: travel

_Auto-generated from the PDB research workflow (2026-08-19)._

## Summary

## 1. How a location change works in single player

**Input → jump**
`AESPlayerController::InputChargeTravelMode` (05E0D0F0) / `...Released` (05E0D204) gate on `CanCurrentlyCallMobileHomebase`, `IsPlayerRemoteControlling`, `IsRacingActive`, `GetCurrentLocationData`, `IsManuallyLeavingLeviathanPossible`, then call `UJumpDriveComponent::SetOwnerIsChargingJump(bool)` (05D7D508). The destination is nothing more than one FName: `UJumpDriveComponent::TargetLocationID` at **offset 0x378**, written by the reflected UFunction `UJumpDriveComponent::SetTargetLocation(FName)` whose whole native body is inlined in its exec stub (05C5B3E0: `StepExplicitProperty` then `mov [rsi+0x378], rax`). `AESPawn::JumpDrive` is at AESPawn+0x3B0.

`UJumpDriveComponent::TickComponent` (013EF840) counts `CurrentJumpChargeCountdown` (0x368) down and fires its dynamic multicast delegates. `AESGameModeBase::StartPlay` (02B3BF24) binds the game-mode handlers by name via `__Internal_AddDynamic` — the name strings `&AESGameModeBase::PlayerJumpTriggered`, `&AESGameModeBase::PlayerJumpCompleted`, `&AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors[InTravelMode]` all exist as wide-string literals in .rdata.

`AESGameModeBase::PlayerJumpTriggered(TEnumAsByte<EJumpMethod::Type>)` (05DE7394) — 0x54 bytes, ignores its argument entirely: `ESPlayerPawn(+0x508)->DisableInput(ESPlayerController)` (AActor vtable slot **107**, byte offset 0x358), `ESPlayerController(+0x510)->DisableInput(self)`, `ESPlayerController->bJumpTriggered (PC+0x894) = true`.

`AESGameModeBase::PlayerJumpCompleted(TEnumAsByte<EJumpMethod::Type>, AActor*)` (05DE71DC) — **also ignores both arguments**. It does:
```
Old = UMapLib::GetCurrentLocationData(this)             // 01273E9C, returns const FLocationData&
New = FLocationData()                                    // stack, 448 bytes
PC = GetPlayerController(this,0); Pawn = PC->GetPawnOrSpectator()
if (Pawn->IsA(AESPawn) && Pawn->Health(+0x350)->GetRatio() > 0.f)
    if (JD = Pawn->JumpDrive(+0x3B0)) if (JD->TargetLocationID(+0x378) != None)
        New = UMapLib::GetLocationData(JD->TargetLocationID)   // 0145A35C
ChangeLocation_Internal(this, Old, New, /*bDontClearDockingPoint*/false, /*bWithoutWritingState*/false, /*bDontAutoSave*/false)
```

**ChangeLocation family.** The real parameter order comes from the UFunction PropPointers arrays I decoded out of .rdata (0x148AA7BE0 / 0x148AA7B10), **not** from the PDB's positional `bool, bool`:
`ChangeLocation(UObject* WorldContextObject, const FLocationData& OldLocation, const FLocationData& NewLocation, bool bDontClearDockingPoint, bool bDontAutoSave)`.
- `UGameplayLib::ChangeLocation` (05E21E28) → `ChangeLocation_Internal(…, r9b, /*arg5*/false, /*arg6*/arg5)`
- `UGameplayLib::ChangeLocationWithoutWritingLocationStateIntoPlayerData` (05E21E44) → same but `arg5 = true`
- `UGameplayLib::ChangeLocationAndWriteLocationStateIntoPlayerData` has **no native symbol** — its body is folded into `execChangeLocationAndWriteLocationStateIntoPlayerData` (05B8A074), which calls `ChangeLocation_Internal` at 05B8A3F6 with `arg5 = 0` (i.e. it *does* write the state; it is the same as `ChangeLocation` except it never zeroes the flag). All three are reflected UFunctions.

`ChangeLocation_Internal(UObject* WCO, const FLocationData& Old, const FLocationData& New, bool bDontClearDockingPoint, bool bWithoutWritingLocationState, bool bDontAutoSave)` (05E21E60, ~0x11E8 bytes) does, in order:
1. if `Old.LocationType == Temporary(4)`: find `Old.LocationID` in `PlayerData->TemporaryLocations` (0x14B8, stride 0x1C0) and `Empty()` its `SpaceObjects` (+0x148).
2. if `!bWithoutWritingLocationState` → `ULocationLib::SaveLocationState(true)` (05E69190) — snapshots every `ISavableInterface` actor in the level into `PlayerData->LocationSaveStates[CurrentLocation]`.
3. `UInventoryLib::ResetTransferFlagsForAllItems`, `UGameplayLib::RefreshPlayerShipData` (05E37DA4), `UMissionLib::SaveStateOfCurrentlySpawnedMissions`.
4. `UESGameInstance::StopAndClearAllForceFeedbackEffects`; `PD = UESGameInstance::InstancePointer(+0x1C0)`.
5. `PD->PreviousLocation(0x14F0) = Old.LocationID`; `PD->CurrentLocation(0x14F8) = New.LocationID`; if `!bDontClearDockingPoint` → `PD->CurrentDockingPoint(0x1508) = None`; always `PD->PreviousDockingPoint(0x1518) = None`; `PD->ClearRebuyInventoriesOfStations()`.
6. if `Old.LocationID != None && Old.LocationID != New.LocationID` → `PD->CreateNewRandomLocationSeed(Old.LocationID)` (05DFF724).
7. `PD->CurrentSystem(0x1500) = New.SystemID ? New.SystemID : Old.SystemID`.
8. if `!bDontAutoSave` → write player pawn rotation into `PD->CurrentShipRotation(0x11A0)`; if systems differ, reset `PD->CurrentShipLocation(0x11B8)`; set `UESGameInstance::bTriggerAutoSave(+0x830) = true`; `PD->FindOrAddLocationSaveState(New.LocationID).EnteredLocationTimestamp(+0x120) = PD->PlayingTimeSecondsDouble(0x2130)`; randomise `PD->NextRandomStreamSeed(0x1694)`.
9. **Level name selection → `UGameplayLib::ESOpenLevel(WCO, FName, /*bAbsolute*/true, /*Options*/FString(L""))`**, two call sites:
   - **05E223D0** (taken when `New.LocationID == None`): destination is the *system travel map*. It builds `("Map_TravelMode_S" if SystemStr.Len()>=2 else "Map_TravelMode_S0") + SystemStr`, where `SystemStr = (Old.SystemID != None ? Old.SystemID : PD->CurrentSystem).ToString()`. String constants: 0x1493CCB70 `"Map_TravelMode_S"`, 0x1493CCB88 `"Map_TravelMode_S0"`.
   - **05E22F02** (normal path): if `New.LocationType == ELocationType::Temporary(4)` the level name is `New.MapAssetString` (FString at 0x48/0x50); otherwise it is `New.LocationID.ToString()`. Empty ⇒ `L""` (0x147837A3C) ⇒ NAME_None.
   Then `UMapLib::GetCurrentSystemRegion` / `UnlockSystemRegion`, `AESPlayerController::SetCrosshairToCenter`.

**`UMapLib::ChangeLocationLevel(FName, int)` (05E72350) has nothing to do with travel.** It changes a location's *difficulty level*: finds the FLocationData in `UGameData::GetSingleton()` (table at GameData+0x168, count +0x170, stride 0x1C0), writes `FLocationData.StaticLevel` (+0x110), records the override in `PlayerData->ChangedLocationLevels` (TMap<FName,int> at 0x1640) and returns the previous level. Callers: its exec stub, `UPlayerData::DoWorldLevelingEvent`, `UPlayerData::InitAfterLoad`.

## 2. What ESOpenLevel really does — it is a CLIENT travel, it WILL strand connected clients

`UGameplayLib::ESOpenLevel(const UObject*, FName LevelName, bool bAbsolute, FString Options)` (05E26144):
```
UESGameInstance::ResetPauseCounterAndUnPause()        // 05DE86E4
UUiLib::ClearWidgetStacks()                            // 05E87974
UDialogManager::ClearDialogQueue(true)                 // 05EC64FC
GameInstance->GetSubsystem<UESTimelineMarkerSubsystem>()->SetESTimelineGameMode(2)   // 05E3B1F8
if (UGameplayStatics::GetCurrentLevelName(WCO, true) == LevelName.ToString())
    UGameplayStatics::OpenLevel(WCO, FName("EmptyTransitionMap"), bAbsolute, LevelName.ToString() + "?LevelName=")
else
    UGameplayStatics::OpenLevel(WCO, LevelName, bAbsolute, Options)
```
(`"EmptyTransitionMap"` @0x1493CCB48, `"?LevelName="` @0x1493CCB60. The EmptyTransitionMap level BP reads that option back with the reflected helpers `UGameplayLib::GetLevelOptionsFromTransitionMapPassthrough` (05E2D344) / `FormatLevelOptionsForTransitionMapPassthrough` (05E27B18) and re-issues the open. So "re-enter the same location" is a **two-hop** load through a stub map.)

`UGameplayStatics::OpenLevel` (04A42D68) → builds `Cmd = LevelName [+ "?" + Options]`, `FURL TestURL(&Ctx.LastURL, *Cmd, bAbsolute?TRAVEL_Absolute:TRAVEL_Relative)`, `FURL::IsLocalInternal()` → `UEngine::MakeSureMapNameIsValid` (0508A178, resolves short names via `FPackageName::IsShortPackageName` → UOL module → `FindObjectFast<UPackage>` / `FPackageName::DoesPackageExist`, so bare names like `S01ML01` are legal) → **`UEngine::SetClientTravel(World, *Cmd, TravelType)` (05090B60)**.

`SetClientTravel` only writes `FWorldContext::TravelURL (+0xA8)` and `TravelType (+0xB8)` and removes the `Listen` option from `Ctx.LastURL (+0xC0)`. `UEngine::TickWorldTravel` (0188CAAC) then `Browse`es. **There is no `UWorld::ServerTravel`, no `ProcessServerTravel`, no `ClientTravel` anywhere in this chain** — I confirmed the complete caller set of `UGameplayStatics::OpenLevel`: `ESOpenLevel` ×2, `UUserFunctionsLib::LoadGame`, `OpenLevelBySoftObjectPtr`, `execOpenLevel`, `UBenchmarkHelper::HandleTick`. So on a listen server ES2's own jump tears the world down without telling anyone: connected clients are dropped, and because `bAbsolute == true` the new `FURL` does **not** inherit the base URL (verified in `FURL::FURL(FURL*,const TCHAR*,ETravelType)` at 0509C068: the base-copy block at 0509C1ED is guarded by `cmp eax, 2` = TRAVEL_Relative only), so the `Listen` option and port that `UGameInstance::EnableListenServer` pushed into `LastURL` are lost — the host also **stops listening** after its own jump.

## 3. The travel primitives that do work

`UWorld::ServerTravel(const FString& FURL, bool bAbsolute, bool bShouldSkipGameNotify)` (050F5104) in this build:
```
GM = this->AuthorityGameMode (UWorld+0x158)
if (GM && !GM->CanServerTravel(FURL,bAbsolute))   // vtable slot 255 (+0x7F8)  -> return false
this->NextTravelType (UWorld+0x731) = bAbsolute ? TRAVEL_Absolute(0) : TRAVEL_Relative(2)
if (NextURL (UWorld+0x738) is empty  &&  !WorldContext[+0x88] /*seamless in progress*/) {
    NextURL = FURL
    if (GM) GM->ProcessServerTravel(FURL, bAbsolute)   // vtable slot 256 (+0x800)
    else    NextSwitchCountdown (UWorld+0x718) = 0
}
return true
```
**`bShouldSkipGameNotify` (r9b) is never read in this build** — `ProcessServerTravel` is always invoked when a game mode exists. Good for us; just pass `false`.

`AGameModeBase::ProcessServerTravel(const FString&, bool)` (04A466F0): `StartToLeaveMap()` (vtable +0x830 — the AESGameModeBase override is the ICF-folded empty stub 012386C0), reads **`bUseSeamlessTravel` as `test byte ptr [this+0x328], 1` at 04A46745**, builds the URL, calls `ProcessClientTravel` (vtable slot 290 / +0x910), sets `World->NextURL`, and if seamless calls `UWorld::SeamlessTravel` (050F31B8).

`AGameModeBase::ProcessClientTravel(FString&, bool bSeamless, bool bAbsolute)` (04A46384): iterates PCs, and for each whose `Player` is a `UNetConnection` calls `FString::RemoveFromEnd(TEXT("?listen"), 7, …)` on a copy and then **`PC->ClientTravel(URLcopy, TRAVEL_Relative /*r8d=2, hardcoded*/, bSeamless, FGuid{0,0,0,0})`**. Local PCs are returned, not travelled.

`APlayerController::ClientTravel(const FString& URL, ETravelType, bool bSeamless, FGuid)` (04E7F95C): if `bSeamless && Type==TRAVEL_Relative` bumps `this+0x588`; then `ClientTravelInternal` (04E7F994) which is a *client RPC* — it packs the params and calls `ProcessEvent` through vtable +0x278 on `FindFunctionChecked("ClientTravelInternal")`. On the server that sends it to the owning client; on the client `ClientTravelInternal_Implementation` (04E7FA38) runs and calls `GEngine->SetClientTravel`.

`UEngine::LoadMap` (02B44D2C) scans the incoming `FURL`'s option array for `"Listen"` and calls `UWorld::Listen(URL)` at 02B45C4C. `UGameInstance::EnableListenServer(bool,int)` (04A2F090) sets `WorldContext.LastURL.Port` and `LastURL.AddOption(L"Listen")`, then `World->Listen()` if `World->NetDriver (UWorld+0x38) == nullptr`.
⇒ **Travel relatively (`bAbsolute=false`) so the host's new FURL inherits `Listen`+Port from `LastURL`, or append `?listen` explicitly.** UE strips `?listen` from the client copy for you.

## 4. Seamless vs non-seamless

`AGameModeBase::AGameModeBase(const FObjectInitializer&)` (04A195E8) does `eax = [this+0x328]; eax &= ~2; eax |= 4; [this+0x328] = eax` — i.e. it clears `bStartPlayersAsSpectators` (bit1) and sets `bPauseable` (bit2) but **never sets bit0**, so `bUseSeamlessTravel` is left at the zero-initialised **false**. `AESGameModeBase::AESGameModeBase` (05DDDA0C) never touches 0x328 either (grepped the whole 0x73C-byte body). Only a Blueprint CDO could flip it; that must be checked live.
Non-seamless is also clearly the safer choice here: ES2's entire single-player path is a full `Browse`/`LoadMap` teardown, the mod's existing login flow (`Browse → SetGameMode → PostLogin → RestartPlayer → SpawnDefaultPawnAtTransform_Implementation`) is exactly what a non-seamless server travel re-runs, and seamless travel would need a valid `TransitionMap` in `UGameMapsSettings` plus `GetSeamlessTravelActorList`/`HandleSeamlessTravelPlayer` behaviour that ES2 has never exercised. **Keep bUseSeamlessTravel = false.**

## 5. Docking is NOT a level change

`UGameplayLib::IsPlayerDockedToAnyStation(bool)` (0138210C) is 0x28 bytes: `PD = GetPlayerData(); return PD && PD->CurrentDockingPoint (0x1508) != NAME_None;`. Docking state is a single FName in `UPlayerData`, plus the reflected properties `bDockingInProgress`, `bUndockFromCurrentStation`, `DockableStationInstances`, `LastStationUndockTimestamp`.
`ADockableStation::DockPlayer(float)` (05D94508) just arms a timer onto `ADockableStation::ImmediateDock` (05D97410) which reads PlayerData, `UInteractComponent::ConfirmInteract(bool,bool)` and `OnPlayerSkipDockingSequence`. `AESGameModeBase::DelayedUndockPlayerShip` (05DE24CC) reads `PD->CurrentDockingPoint`, resolves it with `UGameplayLib::GetDockableStationActor(FName)` (0195704C) and calls `UInventoryLib::ReinitShipAfterPotentialChanges`. **No docking function anywhere calls `ESOpenLevel`/`OpenLevel`** (verified against the complete caller lists of both). `ADockableStation::OnPlayerBeginDocking_Implementation` and `TriggerUndockAnimation_Implementation` are the ICF-folded empty stub at 012386C0 (30 486 symbols share it — never hook that address).
⇒ Docking is an in-level state + UI state on top of the still-loaded location map. **No travel coordination is needed for docking.** The one cross-player effect is `AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation` (05DEC54C): at 05DEC88B it reads `PD->CurrentDockingPoint` and, when set, spawns the pawn at the resolved `ADockableStation` — so if the host is docked when player 2 joins or after a travel, player 2's ship is spawned inside/at the station.

## 6. World origin shifting is real and one-sided

- `UESGameInstance::SetWorldOriginShifting(bool SetOn)` (05DEB784) is 5 instructions: `WorldOriginShiftingStack (UESGameInstance+0x1150) += SetOn ? +1 : -1`. It is a refcount, not a flag.
- `execGetWorldOriginShifting` (05ACA370) returns `WorldOriginShiftingStack > 0` (there is no separate native getter).
- `UESGameInstance::Init` at 05DE4804 calls `SetWorldOriginShifting(bUseWorldOriginShifting)` where `bUseWorldOriginShifting` is the config bool at UESGameInstance+0x214 — so shifting is on by default whenever that CDO/config bool is true.
- `AESGameModeBase::CheckForWorldOriginShifting()` (0192A884) early-returns unless `InstancePointer && WorldOriginShiftingStack > 0`; the body was outlined to 02B181EA where it takes `GetPlayerPawn(0)->RootComponent(+0x1B8)->ComponentToWorld.Translation(+0x1F0)`, compares its length against `WorldOriginShiftingMaxDistanceTravelMode (GI+0x220)` if `IsInTravelMode` else `WorldOriginShiftingMaxDistance (GI+0x21C)`, and when exceeded calls `UGameplayStatics::SetWorldOriginLocation(WCO, GetWorldOriginLocation() + int(pos))`. It is driven from `AESGameModeBase::TimerHandleWorldOriginShift` (GameMode+0x540) armed in StartPlay.
⇒ **AESGameModeBase only exists on the server.** The host rebases its world origin around its own pawn; the client never runs this code and keeps origin (0,0,0). Every replicated `FRepMovement`/absolute coordinate then differs by the host's accumulated origin offset. Disable it.

## 7. `IsInTravelMode` and `RightBeforeBeginPlay…`

`UGameplayLib::IsInTravelMode(const UObject*)` (01273FF4) = `GetCurrentLevelName(WCO, /*bRemovePrefix*/true).Find("TravelMode") != INDEX_NONE` (literal `"TravelMode"` @0x147837A30). So "travel mode" simply means *the currently loaded level is a `Map_TravelMode_S##` system map*, not a special engine travel state.

`AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors()` (05DE9610) — reflected, called **only** from its exec stub (i.e. from Blueprint, almost certainly each location's Level BP): `ULocationLib::ResetMissionGroupSavedOrRestoredFlags()`, `ULocationLib::SpawnOrRestoreLocationActors(WCO, RandomStream)` (05E6A52C), `SpawnActiveMissions()`, `SpawnPerks()`, `SpawnChallenges()`, then one BP call via `FindFunctionChecked` + `ProcessEvent`.
`AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActorsInTravelMode()` has **no native symbol** — the whole body is the exec stub 05AF13B4: `SpawnActiveMissions(); SpawnChallenges();` and nothing else (no location actors, no perks). Both are reflected UFunctions callable by name.

## 8. Which of these are reflected (callable by name through ProcessEvent)

`Z_Construct_UFunction_…` entries exist (count == 1 each) for: `UGameplayLib::ChangeLocation`, `ChangeLocationWithoutWritingLocationStateIntoPlayerData`, `ChangeLocationAndWriteLocationStateIntoPlayerData`, `ESOpenLevel`, `IsPlayerDockedToAnyStation`, `IsInTravelMode`, `GetPlayerData`, `ReenterLocationAndKeepPosition`; `UMapLib::ChangeLocationLevel`, `GetCurrentLocationData`, `GetLocationData`; `AESGameModeBase::PlayerJumpTriggered`, `PlayerJumpCompleted`, `RightBeforeBeginPlayIsCalledForAllActors`, `RightBeforeBeginPlayIsCalledForAllActorsInTravelMode`, `DelayedUndockPlayerShip`, `CheckForWorldOriginShifting`; `UESGameInstance::SetWorldOriginShifting`, `GetWorldOriginShifting`; `UPlayerData::GetLocationProgress`; `ULocationLib::SaveLocationState`; `UJumpDriveComponent::SetTargetLocation`, `ForceImmediateJump`.
`ChangeLocation_Internal` is *not* reflected (free function). `UWorld::ServerTravel`, `APlayerController::ClientTravel`, `UGameplayStatics::OpenLevel` are engine functions — `OpenLevel` is reflected as `execOpenLevel`; `ClientTravelInternal` is reflected (that is how the RPC works).

## 9. IMPLEMENTABLE CO-OP JUMP SEQUENCE

**Setup, once per map load, on both sides**
- H0/C0. Kill origin rebasing: hook `AESGameModeBase::CheckForWorldOriginShifting` (0192A884) → immediate `ret`. (Belt and braces: also call `UESGameInstance::SetWorldOriginShifting(GI, false)` (05DEB784) once at Init so `GetWorldOriginShifting()` reports false to ES2 logic; do not rely on it alone, it is a refcount ES2 pushes/pops.)
- H1. After every `LoadMap`, re-assert the listen server. If you travel relatively (below) `UEngine::LoadMap` re-`Listen`s automatically from the inherited `Listen` option; verify `World->NetDriver (UWorld+0x38) != nullptr` in `AESGameModeBase::StartPlay` (02B3BF24) and call `UGameInstance::EnableListenServer(GI, true, 7777)` (04A2F090) if it is null.

**The jump itself**
1. **Host, observe intent** — hook `AESGameModeBase::PlayerJumpTriggered` (05DE7394). Read the destination yourself: `Pawn(GameMode+0x508)->JumpDrive(+0x3B0)->TargetLocationID(+0x378)`. Send a `JUMP_BEGIN{DestLocationID}` message to the client so it can fade out / lock input. Call the original.
2. **Host, capture full state** — hook `ChangeLocation_Internal` (05E21E60). At entry you have `Old` (rdx) and `New` (r8) as `const FLocationData*`. Snapshot from `New`: `LocationID(0x30)`, `SystemID(0x3C)`, `Seed(0x44)`, `LocationType(0x130)`, `LocationCategory(0x131)`, `StaticLevel(0x110)`, `MapAssetString(0x48)`; and if `LocationType == Temporary(4)` snapshot the entire 448-byte `FLocationData` because for temporary locations it lives **only** in `PlayerData->TemporaryLocations` and the client's copy will not have it. Let the original run — you want steps 1‑8 of §1 (SaveLocationState, PlayerData bookkeeping, seeds) to happen normally on the host, because the host is the save-game authority.
3. **Host, replace the travel** — hook `UGameplayStatics::OpenLevel` (04A42D68). This is the single choke point for *every* ES2 level change (verified caller set). In the hook:
   - if `World->InternalGetNetMode() != NM_ListenServer` (or `GameMode == nullptr`) → tail-call the original (single-player and menu flows unchanged);
   - else build `FString URL = LevelName.ToString(); if (Options.Len()) URL += TEXT("?") + Options;`
   - **collapse the transition-map hop**: if `LevelName == "EmptyTransitionMap"` and `Options` starts with a level name followed by `"?LevelName="`, just travel straight to that real level name (ServerTravel reloads the same map fine, and this saves the client a second round-trip);
   - call `UWorld::ServerTravel(World, URL, /*bAbsolute=*/false, /*bShouldSkipGameNotify=*/false)` (050F5104) and **return without calling the original**.
   `bAbsolute=false` ⇒ `NextTravelType = TRAVEL_Relative` ⇒ `FURL(&LastURL, URL, TRAVEL_Relative)` inherits Port and the `Listen` option, so the host keeps listening. (If you must travel absolute, append `?listen`; `ProcessClientTravel` strips it from the client copy with `RemoveFromEnd("?listen",7)`.)
4. **Engine does the rest on the host**: `ServerTravel` → `ProcessServerTravel` (04A466F0) → `ProcessClientTravel` (04A46384) → for each remote PC `ClientTravel(URL, TRAVEL_Relative, bSeamless=false, FGuid())` → `World->NextURL = URL` → next `UEngine::TickWorldTravel` → `Browse` → `LoadMap` → `Listen`.
5. **Client** — it receives `ClientTravelInternal` and browses relative to its `LastURL` (the host address), i.e. it reconnects to `host:7777/<Map>`. Hook `APlayerController::ClientTravelInternal_Implementation` (04E7FA38) purely to observe (log, fade, stop your own tickers) and always call the original.
6. **Before the client's map finishes loading**, push the state list below over your own channel and apply it to the *client's* `UPlayerData` (`UESGameInstance::InstancePointer` @RVA 09AC5E88, `+0x1C0` → `UPlayerData*`; or `UGameplayLib::GetPlayerData()` 0127398C). Apply it in the client's `PostLogin`/pawn-spawn hook, before any client-side ES2 code queries `UMapLib::GetCurrentLocationData`.
7. **Host, arrival** — `AESGameModeBase::StartPlay` (02B3BF24) runs on the new map: it re-reads `PlayerData` for location/seed/docking, arms all the timers, calls `AGameMode::StartPlay` (which drives `PostLogin`/`RestartPlayer` for the rejoining client) and re-binds the jump-drive delegates. Hook it (post) to re-broadcast state and re-verify the net driver.
8. **Client-side safety**: block `ULocationLib::SaveLocationState` (05E69190) and `ChangeLocation_Internal` (05E21E60) when `NetMode == NM_Client` — both are static/free functions that write into the *local* `UPlayerData` and would corrupt the client's own save with the host's world.

**State that must be synchronised around the travel** (all in `UPlayerData`, all reflected UPROPERTYs so `ProcessEvent`/direct writes both work):
| offset | field | why |
|---|---|---|
| 0x14F0 | `PreviousLocation` (FName) | `GetCurrentLocationData` / HUD / map |
| 0x14F8 | `CurrentLocation` (FName) | **critical** — every client-side `UMapLib::GetCurrentLocationData` reads it |
| 0x1500 | `CurrentSystem` (FName) | travel-mode map name, region unlocks |
| 0x1508 | `CurrentDockingPoint` (FName) | pawn spawn point, `IsPlayerDockedToAnyStation` |
| 0x1518 | `PreviousDockingPoint` (FName) | undock logic |
| 0x1510 | `CurrentActivity` (FName) | activity/mission gating |
| 0x1694 | `NextRandomStreamSeed` (int) | procedural spawn parity |
| 0x14B8/0x14C0 | `TemporaryLocations` (TArray<FLocationData>, stride 0x1C0) | **required** for `ELocationType::Temporary` destinations — `GetLocationData` (0145A35C) searches GameData *and* this array; without the entry the client cannot resolve the location at all |
| 0x11A0 / 0x11B8 | `CurrentShipRotation` / `CurrentShipLocation` | spawn transform |
| 0x1640 | `ChangedLocationLevels` (TMap<FName,int>) | location difficulty overrides (`UMapLib::ChangeLocationLevel`) |
| 0x1198 | `Difficulty` | scaling |
| 0x2130 | `PlayingTimeSecondsDouble` | feeds `FLocationState::EnteredLocationTimestamp` |

`FLocationProgress` (`UPlayerData::GetLocationProgress(FName)` 05E027A0, 176 bytes returned **by hidden pointer in rcx**) is derived on the fly from `UGameData` spawn tables + `PlayerData->LocationProgression`; it is a UI/progress number, not something the destination map needs at load time — sync it after arrival if you want the client's "location cleared %" to match. `FLocationState` (296 bytes, `UPlayerData::FindOrAddLocationSaveState` 05E008A8) is the per-location actor snapshot; it is **host-only** — the client gets the world through actor replication, and pushing FLocationState to the client would be both huge and pointless.

## Key functions

- **UGameplayStatics::OpenLevel** @ `04A42D68` unique=True — Builds 'Level[?Options]', validates with UEngine::MakeSureMapNameIsValid (short names OK in shipping), then calls UEngine::SetClientTravel. THE single choke point for every ES2 level change - complete verified caller set: ESOpenLevel x2 (05E262EE, 05E26320), UUserFunctionsLib::LoadGame+12e, OpenLevelBySoftObjectPtr+cb, execOpenLevel+399, UBenchmarkHelper::HandleTick+3e5. It is a CLIENT travel and 
  - `public: static void __cdecl UGameplayStatics::OpenLevel(class UObject const *, class FName, bool, class FString)`
  - ABI: rcx = const UObject* WorldContextObject; rdx = FName LevelName BY VALUE (8 bytes, ComparisonIndex+Number); r8b = bool bAbsolute; r9 = const FString* Options (16-byte struct => HIDDEN POINTER). Returns void.
- **UGameplayLib::ESOpenLevel** @ `05E26144` unique=True — ResetPauseCounterAndUnPause + UUiLib::ClearWidgetStacks + UDialogManager::ClearDialogQueue(true) + UESTimelineMarkerSubsystem::SetESTimelineGameMode(2); then if GetCurrentLevelName(WCO,true)==LevelName it calls OpenLevel("EmptyTransitionMap", bAbsolute, LevelName+"?LevelName=") else OpenLevel(LevelName, bAbsolute, Options).
  - `public: static void __cdecl UGameplayLib::ESOpenLevel(class UObject const *, class FName, bool, class FString)`
  - ABI: rcx = WorldContextObject; rdx = FName LevelName by value; r8b = bAbsolute; r9 = FString* Options (hidden pointer). UFunction param order confirmed from PropPointers @0x148AA6A90: WorldContextObject, LevelName, bAbsolute, Options.
- **ChangeLocation_Internal** @ `05E21E60` unique=True — The whole location change. Saves location state, updates PlayerData (PreviousLocation 0x14F0, CurrentLocation 0x14F8, CurrentSystem 0x1500, CurrentDockingPoint 0x1508, PreviousDockingPoint 0x1518, CurrentShipRotation 0x11A0, NextRandomStreamSeed 0x1694), sets UESGameInstance::bTriggerAutoSave (+0x830), picks the destination level FName and calls ESOpenLevel (two sites: 05E223D0 travel-mode map, 05
  - `void __cdecl ChangeLocation_Internal(class UObject *, struct FLocationData const &, struct FLocationData const &, bool, bool, bool)`
  - ABI: rcx = UObject* WorldContextObject; rdx = const FLocationData* OldLocation; r8 = const FLocationData* NewLocation; r9b = bool bDontClearDockingPoint; [rsp+0x20] = bool bWithoutWritingLocationStateIntoPlayerData; [rsp+0x28] = bool bDontAutoSave. FLocationData is 448 bytes so both structs are by refere
- **UGameplayLib::ChangeLocation** @ `05E21E28` unique=True — 27-byte thunk: ChangeLocation_Internal(WCO, Old, New, bDontClearDockingPoint, /*bWithoutWritingState*/false, bDontAutoSave).
  - `public: static void __cdecl UGameplayLib::ChangeLocation(class UObject * WorldContextObject, struct FLocationData const & OldLocation, struct FLocationData const & NewLocation, bool bDontClearDockingPoint, bool bDontAutoSave)`
  - ABI: rcx/rdx/r8/r9b + [rsp+0x28] on entry. PARAM NAMES/ORDER verified by decoding Z_Construct_UFunction_UGameplayLib_ChangeLocation_Statics::PropPointers @0x148AA7BE0 -> [WorldContextObject, OldLocation, NewLocation, bDontClearDockingPoint, bDontAutoSave]. The PDB's positional 'bool,bool' is ambiguous - 
- **UGameplayLib::ChangeLocationWithoutWritingLocationStateIntoPlayerData** @ `05E21E44` unique=True — Same thunk but passes bWithoutWritingLocationState = true, so ULocationLib::SaveLocationState is skipped. Used by ReturnToMainMenu, ReturnFromLeviathanInnards, StartNewGame, LoadGameOnlyToBeUsedInEmptyTransitionMap.
  - `public: static void __cdecl UGameplayLib::ChangeLocationWithoutWritingLocationStateIntoPlayerData(class UObject *, struct FLocationData const &, struct FLocationData const &, bool bDontClearDockingPoint, bool bDontAutoSave)`
  - ABI: Identical shape to ChangeLocation.
- **UGameplayLib::execChangeLocationAndWriteLocationStateIntoPlayerData** @ `05B8A074` unique=True — Same as ChangeLocation but always writes the location state. Only reachable by name through ProcessEvent.
  - `public: static void __cdecl UGameplayLib::execChangeLocationAndWriteLocationStateIntoPlayerData(class UObject *, struct FFrame &, void *const)`
  - ABI: Standard exec stub (rcx=Context, rdx=FFrame&, r8=RESULT_PARAM). There is NO native symbol for this function - its body is folded into the exec stub, which calls ChangeLocation_Internal at 05B8A3F6 with arg5=0.
- **UMapLib::ChangeLocationLevel** @ `05E72350` unique=True — NOT travel. Changes a location's DIFFICULTY LEVEL: finds the FLocationData in UGameData::GetSingleton() (table +0x168, count +0x170, stride 0x1C0), writes FLocationData.StaticLevel (+0x110) and records the override in PlayerData->ChangedLocationLevels (TMap<FName,int> @0x1640). Callers: exec stub, UPlayerData::DoWorldLevelingEvent, UPlayerData::InitAfterLoad.
  - `public: static int __cdecl UMapLib::ChangeLocationLevel(class FName, int)`
  - ABI: rcx = FName LocationID by value; edx = int NewLevel; returns previous level in eax.
- **AESGameModeBase::PlayerJumpTriggered** @ `05DE7394` unique=True — if (ESPlayerPawn(+0x508) && ESPlayerController(+0x510)) { Pawn->DisableInput(PC) via AActor vtable slot 107 (+0x358); PC->DisableInput(PC); PC->bJumpTriggered (+0x894) = true; }. Ideal 'jump is starting' notification point on the host.
  - `private: void __cdecl AESGameModeBase::PlayerJumpTriggered(class TEnumAsByte<enum EJumpMethod::Type>)`
  - ABI: rcx = AESGameModeBase* this; dl = EJumpMethod byte. THE ARGUMENT IS NEVER READ. Bound at runtime by AESGameModeBase::StartPlay via __Internal_AddDynamic using the literal '&AESGameModeBase::PlayerJumpTriggered'.
- **AESGameModeBase::PlayerJumpCompleted** @ `05DE71DC` unique=True — Old = UMapLib::GetCurrentLocationData(this); New = GetLocationData(Pawn->JumpDrive(+0x3B0)->TargetLocationID(+0x378)) if pawn is AESPawn and Health(+0x350)->GetRatio() > 0; then ChangeLocation_Internal(this, Old, New, false, false, false) at 05DE735A.
  - `private: void __cdecl AESGameModeBase::PlayerJumpCompleted(class TEnumAsByte<enum EJumpMethod::Type>, class AActor *)`
  - ABI: rcx = this; dl = EJumpMethod; r8 = AActor*. BOTH ARGUMENTS ARE UNREAD - the destination comes only from JumpDrive->TargetLocationID. 448-byte FLocationData built on stack at [rsp+0x80].
- **AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors** @ `05DE9610` unique=True — ULocationLib::ResetMissionGroupSavedOrRestoredFlags, ULocationLib::SpawnOrRestoreLocationActors(WCO, RandomStream) (05E6A52C), SpawnActiveMissions, SpawnPerks, SpawnChallenges, then one BP call via FindFunctionChecked+ProcessEvent. This is where the host repopulates a location after a load.
  - `private: void __cdecl AESGameModeBase::RightBeforeBeginPlayIsCalledForAllActors(void)`
  - ABI: rcx = this, no args. Only caller is its own exec stub (05AF13AD) => invoked from Blueprint, almost certainly each location's Level BP.
- **AESGameModeBase::execRightBeforeBeginPlayIsCalledForAllActorsInTravelMode** @ `05AF13B4` unique=True — Calls only SpawnActiveMissions() then tail-jumps to SpawnChallenges(). The 'travel mode' variant deliberately skips SpawnOrRestoreLocationActors and SpawnPerks, because Map_TravelMode_S## maps hold no location content.
  - `public: static void __cdecl AESGameModeBase::execRightBeforeBeginPlayIsCalledForAllActorsInTravelMode(class UObject *, struct FFrame &, void *const)`
  - ABI: NO native implementation symbol exists - the entire body is these 0x28 bytes of exec stub.
- **UGameplayLib::IsInTravelMode** @ `01273FF4` unique=True — GetCurrentLevelName(WCO, bRemovePrefix=true).Find("TravelMode") != INDEX_NONE (literal at 0x147837A30). 'Travel mode' just means the loaded level is a Map_TravelMode_S## system map.
  - `public: static bool __cdecl UGameplayLib::IsInTravelMode(class UObject const *)`
  - ABI: rcx = WorldContextObject; returns bool in al.
- **UWorld::ServerTravel** @ `050F5104` unique=True — GM = this->AuthorityGameMode (UWorld+0x158); if (GM && !GM->CanServerTravel(URL,bAbsolute) [vtable +0x7F8]) return false; NextTravelType(UWorld+0x731) = bAbsolute?0:2; if (NextURL(UWorld+0x738) empty && !WorldContext[+0x88]) { NextURL = URL; if (GM) GM->ProcessServerTravel(URL,bAbsolute) [vtable +0x800]; else NextSwitchCountdown(UWorld+0x718)=0; } return true.
  - `public: bool __cdecl UWorld::ServerTravel(class FString const &, bool bAbsolute, bool bShouldSkipGameNotify)`
  - ABI: rcx = UWorld* this; rdx = const FString* URL (hidden pointer, 16-byte struct); r8b = bAbsolute; r9b = bShouldSkipGameNotify -- NEVER READ in this build (I disassembled all 0x114 bytes; r9 is untouched). Returns bool in al.
- **AGameModeBase::ProcessServerTravel** @ `04A466F0` unique=True — StartToLeaveMap (vtable +0x830; the ES2 override is the empty ICF stub), reads bUseSeamlessTravel as 'test byte ptr [this+0x328], 1' at 04A46745, calls ProcessClientTravel (vtable +0x910), sets World->NextURL, and if seamless calls UWorld::SeamlessTravel (050F31B8).
  - `public: virtual void __cdecl AGameModeBase::ProcessServerTravel(class FString const &, bool)`
  - ABI: Virtual, vtable slot 256 (byte offset 0x800). rcx=this, rdx=const FString*, r8b=bAbsolute.
- **AGameModeBase::ProcessClientTravel** @ `04A46384` unique=True — For every PC whose Player is a UNetConnection: copies the URL, FString::RemoveFromEnd("?listen", 7), then PC->ClientTravel(copy, TRAVEL_Relative /*hardcoded r8d=2*/, bSeamless, FGuid{0,0,0,0}) at 04A465AD. Local (host) PC is returned, not travelled.
  - `protected: virtual class APlayerController * __cdecl AGameModeBase::ProcessClientTravel(class FString &, bool bSeamless, bool bAbsolute)`
  - ABI: Virtual, vtable slot 290 (byte offset 0x910). Note this build's virtual takes 3 params, not the 4-param FGuid overload.
- **APlayerController::ClientTravel** @ `04E7F95C` unique=True — If bSeamless && type==TRAVEL_Relative bumps this+0x588; then ClientTravelInternal (04E7F994) which ProcessEvents the 'ClientTravelInternal' client RPC. Called on the server it sends the travel order to that client.
  - `public: void __cdecl APlayerController::ClientTravel(class FString const &, enum ETravelType, bool, struct FGuid)`
  - ABI: rcx = APlayerController* this; rdx = const FString* URL (hidden pointer); r8d = ETravelType (TRAVEL_Absolute=0, TRAVEL_Partial=1, TRAVEL_Relative=2); r9b = bool bSeamless; 5th arg FGuid (16 bytes) at [rsp+0x28] BY HIDDEN POINTER - you must pass the address of a real FGuid, zero-filled is fine.
- **APlayerController::ClientTravelInternal_Implementation** @ `04E7FA38` unique=True — The client-side end of a server travel; calls GEngine->SetClientTravel. Best read-only observation point on the client for 'the host is moving us'.
  - `public: virtual void __cdecl APlayerController::ClientTravelInternal_Implementation(class FString const &, enum ETravelType, bool, struct FGuid)`
  - ABI: Same shape as ClientTravel. Runs on the client after the RPC arrives.
- **UEngine::SetClientTravel** @ `05090B60` unique=True — Writes FWorldContext::TravelURL (+0xA8) and TravelType (+0xB8), then removes the 'Listen' option from Ctx.LastURL (+0xC0). UEngine::TickWorldTravel (0188CAAC) later Browses. Purely local - no client notification.
  - `public: void __cdecl UEngine::SetClientTravel(class UWorld *, wchar_t const *, enum ETravelType)`
  - ABI: rcx = UEngine* this (GEngine @RVA 09DA37B0); rdx = UWorld*; r8 = const TCHAR* URL; r9d = ETravelType.
- **UGameInstance::EnableListenServer** @ `04A2F090` unique=True — Sets LastURL.Port = PortOverride, LastURL.AddOption(L"Listen"), then World->Listen(FURL(LastURL)) if World->NetDriver (UWorld+0x38) is null. The 'Listen' option persists in LastURL, which is why RELATIVE travel keeps the host listening and ABSOLUTE travel does not.
  - `public: virtual bool __cdecl UGameInstance::EnableListenServer(bool, int)`
  - ABI: rcx = UGameInstance* this; dl = bEnable; r8d = PortOverride. WorldContext is at GameInstance+0x30, its LastURL at +0xC0, LastURL.Port at +0xE0.
- **UEngine::LoadMap** @ `02B44D2C` unique=True — Among much else, scans the incoming FURL's Op[] array for the option 'Listen' and calls UWorld::Listen(URL) at 02B45C4C. This is what re-establishes the listen server after a travel, provided the option survived into the new FURL.
  - `public: virtual bool __cdecl UEngine::LoadMap(struct FWorldContext &, struct FURL, class UPendingNetGame *, class FString &)`
  - ABI: Virtual. FURL passed by value (104 bytes) => hidden pointer.
- **AESGameModeBase::CheckForWorldOriginShifting** @ `0192A884` unique=True — Early-returns unless UESGameInstance::InstancePointer && WorldOriginShiftingStack (GI+0x1150) > 0. Otherwise: dist = |PlayerPawn->RootComponent(+0x1B8)->ComponentToWorld.Translation(+0x1F0)|; limit = IsInTravelMode ? GI+0x220 : GI+0x21C; if dist > limit -> UGameplayStatics::SetWorldOriginLocation(WCO, GetWorldOriginLocation() + int(pos)). Runs on the SERVER ONLY (GameMode is server-only), so it de
  - `private: void __cdecl AESGameModeBase::CheckForWorldOriginShifting(void)`
  - ABI: rcx = this, no args, no return. Body outlined to 02B181EA. Driven by a timer stored in AESGameModeBase::TimerHandleWorldOriginShift (+0x540) armed in StartPlay. Only caller is its exec stub (05AEFCBD).
- **UESGameInstance::SetWorldOriginShifting** @ `05DEB784` unique=True — WorldOriginShiftingStack (this+0x1150) += SetOn ? +1 : -1. A REFCOUNT, not a flag - ES2 pushes/pops it, so a single false is not a permanent disable. Called once from UESGameInstance::Init (05DE4804) with the config bool bUseWorldOriginShifting (GI+0x214).
  - `public: void __cdecl UESGameInstance::SetWorldOriginShifting(bool)`
  - ABI: rcx = UESGameInstance* this; dl = bool SetOn. 5 instructions, no prologue.
- **UESGameInstance::execGetWorldOriginShifting** @ `05ACA370` unique=True — *(bool*)RESULT_PARAM = (this->WorldOriginShiftingStack (+0x1150) > 0).
  - `public: static void __cdecl UESGameInstance::execGetWorldOriginShifting(class UObject *, struct FFrame &, void *const)`
  - ABI: Whole body is the exec stub; there is no native getter symbol.
- **UGameplayLib::IsPlayerDockedToAnyStation** @ `0138210C` unique=True — PD = UGameplayLib::GetPlayerData(); return PD && PD->CurrentDockingPoint (0x1508) != NAME_None. Proves docking is a UPlayerData FName, not a level or a travel.
  - `public: static bool __cdecl UGameplayLib::IsPlayerDockedToAnyStation(bool)`
  - ABI: cl = the bool arg (unused on the fast path); returns bool in al. 0x28 bytes total.
- **AESGameModeBase::DelayedUndockPlayerShip** @ `05DE24CC` unique=True — Reads PlayerData->CurrentDockingPoint (+0x1508), resolves it with UGameplayLib::GetDockableStationActor(FName) (0195704C), calls UInventoryLib::ReinitShipAfterPotentialChanges. No level change anywhere.
  - `private: void __cdecl AESGameModeBase::DelayedUndockPlayerShip(void)`
  - ABI: rcx = this. Timer handle AESGameModeBase::TimerDelayedUndockShip (+0x548). Only caller is its exec stub.
- **ADockableStation::DockPlayer** @ `05D94508` unique=True — Only arms a world timer onto ADockableStation::ImmediateDock (05D97410), which reads PlayerData, calls UInteractComponent::ConfirmInteract(bool,bool) and OnPlayerSkipDockingSequence. Never touches OpenLevel/ESOpenLevel.
  - `public: void __cdecl ADockableStation::DockPlayer(float)`
  - ABI: rcx = ADockableStation* this; xmm1 = float delay.
- **AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation** @ `05DEC54C` unique=True — Reads PlayerData->CurrentDockingPoint at 05DEC88B and, when set, resolves the ADockableStation and spawns there; otherwise uses FindPlayerStart / GetCurrentLocationData / IsFastTravelDestination. This is why a co-op partner materialises at the station when the host is docked.
  - `public: virtual class APawn * __cdecl AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation(class AController *, struct UE::Math::TTransform<double> const &)`
  - ABI: Virtual, vtable slot 231. rcx=this, rdx=AController*, r8=const FTransform* (96 bytes => hidden pointer). Returns APawn* in rax.
- **AESGameModeBase::StartPlay** @ `02B3BF24` unique=True — The host-side 'new location is ready' entry point after every map load. Arms all timers (incl. world-origin shift), caches PlayerData into GameMode+0x518, computes bWaitForMobileHomebaseLoad from CurrentDockingPoint/PreviousDockingPoint, seeds RandomStream from UMapLib::GetCurrentLocationSeed, spawns the jump target / POIs / wingmen / commentary manager, binds the jump-drive dynamic delegates, and
  - `public: virtual void __cdecl AESGameModeBase::StartPlay(void)`
  - ABI: Virtual (AGameModeBase vtable slot 246). rcx = this. Large (0x134C bytes).
- **ULocationLib::SaveLocationState** @ `05E69190` unique=True — Walks every ISavableInterface actor in the current world and writes FActorSaveState/FSpawnGroupSaveStateArray into PlayerData->FindLocationSaveStateAndMarkDirty(CurrentLocation). MUST be blocked on the client - it would overwrite the client's own save with the host's world.
  - `public: static void __cdecl ULocationLib::SaveLocationState(bool)`
  - ABI: cl = bool. Static - operates on the LOCAL GameInstance's UPlayerData via UGameplayLib::GetPlayerData().
- **UPlayerData::GetLocationProgress** @ `05E027A0` unique=True — Derives current/total progression counts per EProgressionType by walking UGameData's FLocationSpawns table for the location and PlayerData->LocationProgression, and fills CompletionPercentage (+0xA8). Derived data - not needed to make a destination map load correctly.
  - `public: struct FLocationProgress __cdecl UPlayerData::GetLocationProgress(class FName)`
  - ABI: RETURNS A 176-BYTE STRUCT: rcx = hidden return pointer to caller-allocated FLocationProgress, rdx = UPlayerData* this, r8 = FName LocationID by value. Construct the out-struct with FLocationProgress::FLocationProgress (05C5C200) first.
- **UPlayerData::FindOrAddLocationSaveState** @ `05E008A8` unique=True — Gets or creates the 296-byte per-location snapshot. ChangeLocation_Internal uses it at 05E22E50 to stamp EnteredLocationTimestamp (+0x120) = PlayerData->PlayingTimeSecondsDouble (0x2130).
  - `public: struct FLocationState & __cdecl UPlayerData::FindOrAddLocationSaveState(class FName const &)`
  - ABI: rcx = UPlayerData* this; rdx = const FName* (BY REFERENCE, note the & in the signature); returns FLocationState* in rax.
- **UMapLib::GetLocationData** @ `0145A35C` unique=True — Searches UGameData::GetSingleton()'s static location table (stride 0x1C0) AND PlayerData->TemporaryLocations (0x14B8/0x14C0). This is why the client needs the host's TemporaryLocations entries for Temporary destinations.
  - `public: static struct FLocationData const & __cdecl UMapLib::GetLocationData(class FName const &)`
  - ABI: rcx = const FName* BY REFERENCE; returns const FLocationData* in rax (reference to storage owned by UGameData or UPlayerData - do not free, do not cache across a travel).
- **UMapLib::GetCurrentLocationData** @ `01273E9C` unique=True — Checks GetCurrentLevelName for 'TravelMode' and otherwise resolves PlayerData->CurrentLocation through GetLocationData. Every client-side HUD/map/bounds query funnels through here, which is why PlayerData->CurrentLocation must be synced on the client.
  - `public: static struct FLocationData const & __cdecl UMapLib::GetCurrentLocationData(class UObject const *)`
  - ABI: rcx = WorldContextObject; returns const FLocationData* in rax.
- **UGameplayLib::GetPlayerData** @ `0127398C` unique=True — The single accessor for the local UPlayerData. Both host and client have their own instance - that is exactly the object whose location fields must be kept in sync.
  - `public: static class UPlayerData * __cdecl UGameplayLib::GetPlayerData(void)`
  - ABI: No args. 0x1C bytes: returns UESGameInstance::InstancePointer (static UESGameInstance** at RVA 09AC5E88) -> +0x1C0, or null.
- **UJumpDriveComponent::execSetTargetLocation** @ `05C5B3E0` unique=True — UJumpDriveComponent::SetTargetLocation(FName) simply assigns TargetLocationID (offset 0x378). You can set a co-op partner's jump destination either by ProcessEvent('SetTargetLocation') or by writing the FName at +0x378 directly.
  - `public: static void __cdecl UJumpDriveComponent::execSetTargetLocation(class UObject *, struct FFrame &, void *const)`
  - ABI: No native symbol exists - the body is the exec stub. Ends with 'mov qword ptr [rsi+0x378], rax'.
- **UJumpDriveComponent::ForceImmediateJump** @ `05D77934` unique=True — Reflected - the cleanest way to make the host jump on command from a co-op vote/sync, since it drives the same delegate chain that ends in PlayerJumpCompleted.
  - `public: void __cdecl UJumpDriveComponent::ForceImmediateJump(class FName, bool)`
  - ABI: rcx = this; rdx = FName by value; r8b = bool.
- **AESPlayerController::InputChargeTravelMode** @ `05E0D0F0` unique=True — The player-facing jump input. Gates on CanCurrentlyCallMobileHomebase / IsPlayerRemoteControlling / IsRacingActive / GetActivityBaseOfType / GetCurrentLocationData / IsManuallyLeavingLeviathanPossible, then calls UJumpDriveComponent::SetOwnerIsChargingJump(true) (05D7D508). Hook here if you want to veto a jump until both players agree.
  - `private: void __cdecl AESPlayerController::InputChargeTravelMode(void)`
  - ABI: rcx = this. Paired with InputChargeTravelModeReleased (05E0D204).
- **UGameplayLib::ReenterLocationAndKeepPosition** @ `05E37AC0` unique=True — Reloads the current location preserving the ship position - goes through the ESOpenLevel same-level path (EmptyTransitionMap + ?LevelName=). Useful as a co-op 'resync both players into the same level' primitive.
  - `public: static void __cdecl UGameplayLib::ReenterLocationAndKeepPosition(void)`
  - ABI: No args. Calls ChangeLocation_Internal at 05E37C86.

## Types

### FLocationData (size 448)
- 0x0030 LocationID FName — the level's short package name for non-Temporary locations; also the PlayerData key
- 0x003C SystemID FName — feeds PlayerData->CurrentSystem and the Map_TravelMode_S## name
- 0x0044 Seed int — procedural generation seed
- 0x0048 MapAssetString FString (ptr 0x48 / Num 0x50) — THE level name when LocationType==Temporary(4)
- 0x0058 MapAsset TSoftObjectPtr<UObject> (40 bytes) — soft ref to the map package
- 0x0080 LocationOnMap FVector
- 0x00B0 LocationTransform FTransform (96 bytes)
- 0x0110 StaticLevel int — the difficulty level UMapLib::ChangeLocationLevel writes
- 0x0118 ParentLocationID FName
- 0x0130 LocationType TEnumAsByte<ELocationType::Type> — Normal=0 LowOrbit=1 Surface=2 Composite=3 Temporary=4; ==4 switches the level-name source to MapAssetString
- 0x0131 LocationCategory TEnumAsByte<ELocationCategory::Type> — ==0x11 (Leviathan) is special-cased in ChangeLocation_Internal at 05E21F54
- 0x0138 Connections TArray<FLocationConnection>
- 0x0148 SpaceObjects TArray<FLocationSpaceObject> — Emptied/copied for Temporary locations
- 0x01B0 bLocked / 0x01B1 bTemporaryEvent / 0x01B2 bHidden / 0x01B3 bInvalidated bool

### UPlayerData (location/travel-relevant fields) (size None)
- 0x1198 Difficulty EDifficulty — must match on both sides
- 0x11A0 CurrentShipRotation FRotator (24B) — written by ChangeLocation_Internal when !bDontAutoSave
- 0x11B8 CurrentShipLocation FVector (24B) — reset when the system changes
- 0x14B8 TemporaryLocations TArray<FLocationData> (Num at 0x14C0, stride 0x1C0) — REQUIRED on the client for Temporary destinations; GetLocationData searches it
- 0x14D0 UnlockedSystemRegions TArray<TEnumAsByte<ESystemRegion::Type>>
- 0x14F0 PreviousLocation FName
- 0x14F8 CurrentLocation FName — the single most important field to sync; drives GetCurrentLocationData everywhere
- 0x1500 CurrentSystem FName
- 0x1508 CurrentDockingPoint FName — docking state AND pawn spawn override in SpawnDefaultPawnAtTransform_Implementation
- 0x1510 CurrentActivity FName
- 0x1518 PreviousDockingPoint FName
- 0x1520 WormholeReturnTransform FTransform (96B)
- 0x1640 ChangedLocationLevels TMap<FName,int> (80B) — location difficulty overrides
- 0x1690 WorldLevelingEventIndex int
- 0x1694 NextRandomStreamSeed int — procedural parity
- 0x2130 PlayingTimeSecondsDouble double — stamped into FLocationState::EnteredLocationTimestamp

### UESGameInstance (travel/origin fields) (size None)
- static InstancePointer at RVA 09AC5E88 (UESGameInstance**) — the singleton
- 0x01C0 PlayerData UPlayerData*
- 0x0214 bUseWorldOriginShifting bool — config default consumed by UESGameInstance::Init
- 0x0218 WorldOriginShiftingCheckInterval float
- 0x021C WorldOriginShiftingMaxDistance float
- 0x0220 WorldOriginShiftingMaxDistanceTravelMode float
- 0x0830 bTriggerAutoSave bool — set by ChangeLocation_Internal when !bDontAutoSave
- 0x0831 bWriteAutoSaveWhenUndocking bool
- 0x1150 WorldOriginShiftingStack int — REFCOUNT; >0 means origin rebasing is armed

### AESGameModeBase (travel/jump fields) (size 1536)
- 0x0328 bit0 bUseSeamlessTravel (AGameModeBase, uint32 bitfield) — false by default here; read as 'test byte [this+0x328],1'
- 0x0328 bit1 bStartPlayersAsSpectators (cleared in ctor) / bit2 bPauseable (set in ctor)
- 0x0410 LocationInfo ALocationInfo*
- 0x0500 TimerHandlePlayerJumping FTimerHandle
- 0x0508 ESPlayerPawn AESPawn* — used by PlayerJumpTriggered
- 0x0510 ESPlayerController AESPlayerController*
- 0x0518 PlayerData UPlayerData* — cached in StartPlay
- 0x0528 MobileHomebaseStreamingObject ULevelStreamingDynamic*
- 0x0530 bWaitForMobileHomebaseLoad bool — derived from CurrentDockingPoint in StartPlay
- 0x0540 TimerHandleWorldOriginShift FTimerHandle — drives CheckForWorldOriginShifting
- 0x0548 TimerDelayedUndockShip FTimerHandle
- 0x0584 RandomStream FRandomStream — seeded from UMapLib::GetCurrentLocationSeed

### UJumpDriveComponent (size 984)
- 0x017C TriggeredJumpMethod TEnumAsByte<EJumpMethod::Type>
- 0x0198 OnJumpInCompleted / 0x01B8 OnJumpChargeCompleted / 0x01C8 OnJumpCompleted — the multicast delegates AESGameModeBase::StartPlay binds PlayerJumpTriggered/PlayerJumpCompleted to
- 0x02F8 JumpInMethod TEnumAsByte<EJumpMethod::Type>
- 0x02FA bTriggeredJump bool
- 0x0368 CurrentJumpChargeCountdown float
- 0x0370 bIsCurrentlyJumpingAway bool
- 0x0374 CurrentlyJumpingCountDown float
- 0x0378 TargetLocationID FName — THE destination; the only input PlayerJumpCompleted uses
- 0x03A0 bOwnerIsChargingJump bool
- 0x03A2 bJumpDriveAllowed bool
- 0x03A8 bSuppressed bool

### UWorld (travel fields) (size None)
- 0x0038 NetDriver TObjectPtr<UNetDriver> — null after a fresh LoadMap unless the URL carried 'Listen'
- 0x0158 AuthorityGameMode TObjectPtr<AGameModeBase> — read directly by ServerTravel
- 0x0718 NextSwitchCountdown float
- 0x0731 NextTravelType TEnumAsByte<ETravelType> — set by ServerTravel
- 0x0738 NextURL FString (Data 0x738 / Num 0x740 / Max 0x744) — ServerTravel early-outs if Num > 1

### FWorldContext (size 712)
- 0x0008 SeamlessTravelHandler FSeamlessTravelHandler (152B); byte at 0x88 is the 'in seamless transition' test used by ServerTravel
- 0x00A8 TravelURL FString — written by SetClientTravel
- 0x00B8 TravelType uint8
- 0x00C0 LastURL FURL (104B) — holds the 'Listen' option and Port added by EnableListenServer; LastURL.Port at +0xE0 relative to the context
- 0x0190 PendingNetGame TObjectPtr<UPendingNetGame>
- 0x0208 OwningGameInstance TObjectPtr<UGameInstance>
- 0x0210 ActiveNetDrivers TArray<FNamedNetDriver>
- 0x02C0 ThisCurrentWorld TObjectPtr<UWorld>

### FLocationState (size 296)
- 0x0018 SpawnGroupSaveStates TMap<ESpawnGroup,FSpawnGroupSaveStateArray> (80B)
- 0x0068 MissionGroupSaveStates TMap<FName,FSpawnGroupSaveStateArray> (80B)
- 0x00B8 POISaveStates TArray<FPOISpawnInfo>
- 0x00C8 ResourcesMined TMap<FName,int>
- 0x0118 AdditionalTimePassedSeconds double
- 0x0120 EnteredLocationTimestamp double — stamped by ChangeLocation_Internal at 05E22E5C

### FLocationProgress (size 176)
- 0x0000 LocationID FName
- 0x0008 Current TMap<EProgressionType,int> (80B)
- 0x0058 Total TMap<EProgressionType,int> (80B)
- 0x00A8 CompletionPercentage float — returned BY VALUE, so the caller supplies a hidden 176-byte out pointer in rcx

### AESPawn / AESPlayerController (relevant offsets) (size None)
- AESPawn 0x0350 Health UHealthComponent* — PlayerJumpCompleted requires GetRatio() > 0
- AESPawn 0x03B0 JumpDrive UJumpDriveComponent*
- AESPlayerController 0x0588 SeamlessTravelCount (APlayerController) — bumped by ClientTravel when seamless+relative
- AESPlayerController 0x0894 bJumpTriggered bool — set true by PlayerJumpTriggered
- AActor 0x0060 RemoteRole / 0x0168 Role TEnumAsByte<ENetRole> — for authority checks in hooks
- AActor vtable slot 105 EnableInput / slot 107 DisableInput (byte offset 0x358)


## Hook plan

- **UGameplayStatics::OpenLevel** @ `04A42D68` [both] — Convert every ES2 level change into a real UE server travel so the client is carried along instead of dropped. This is the single verified choke point for all ES2 travel (ESOpenLevel x2, LoadGame, OpenLevelBySoftObjectPtr, execOpenLevel, BenchmarkHelper).
  - behaviour: Detour, do not call the original on the host. rcx=WorldContextObject, rdx=FName LevelName (by value), r8b=bAbsolute, r9=FString* Options (hidden ptr). (a) Resolve World; if InternalGetNetMode() != NM_ListenServer (i.e. no authority) call the original unchanged. On a pure client, swallow the call entirely and log — the client must only travel when the server orders it. (b) Build URL = LevelName.ToString() + (Options.Len() ? "?" + Options : ""). (c) If LevelName == "EmptyTransitionMap" and Options matches "<Real>?LevelName=", collapse it: travel straight to <Real> and skip the stub-map hop. (d) Call UWorld::ServerTravel(World, URL, /*bAbsolute=*/false, /*bShouldSkipGameNotify=*/false) and return. bAbsolute=false makes FURL inherit Port and the 'Listen' option from FWorldContext::LastURL, so UEngine::LoadMap re-Listens automatically and the host stays a server.
  - risk: Medium-high. This intercepts main-menu / save-load / new-game opens too, so the net-mode guard is load-bearing. FString/FName ABI must be exact (FString is a hidden pointer, FName is by value in rdx). If ServerTravel returns false (CanServerTravel veto or NextURL already set) you must fall back to calling the original, otherwise the jump silently does nothing and the player is stuck with input disabled by PlayerJumpTriggered.
- **AESGameModeBase::CheckForWorldOriginShifting** @ `0192A884` [host] — Stop the host rebasing its world origin. GameMode is server-only, so the client never rebases; every replicated absolute coordinate would drift apart by the host's accumulated origin offset.
  - behaviour: Detour to an immediate `ret` (void, no args, rcx=this). Optionally also call UESGameInstance::SetWorldOriginShifting(GI, false) (05DEB784) once after Init so ES2's own GetWorldOriginShifting() queries report false — but do not rely on that alone, WorldOriginShiftingStack (GI+0x1150) is a refcount that ES2 pushes and pops.
  - risk: Low. The function's only effect is UGameplayStatics::SetWorldOriginLocation. Suppressing it means very distant play may hit float precision in rendering/physics, which is the normal cost of disabling origin rebasing.
- **AESGameModeBase::PlayerJumpTriggered** @ `05DE7394` [host] — Earliest reliable 'a jump has committed' signal on the host — fire the co-op JUMP_BEGIN notification so the client can fade out, lock input and pre-load.
  - behaviour: Observe-only detour, then call the original. rcx=this (the byte argument is never read by the original, so do not trust it). Read the destination yourself: GameMode->ESPlayerPawn(+0x508)->JumpDrive(+0x3B0)->TargetLocationID(+0x378). Also mirror the original's effect on the remote player if you want symmetric input lock: remote PC->DisableInput(PC) via AActor vtable slot 107 and set remote PC->bJumpTriggered(+0x894).
  - risk: Low. Pure observation. Note the original only disables input on the LOCAL host pawn/controller (GameMode+0x508/+0x510), so the co-op partner keeps flying unless you replicate the lock.
- **ChangeLocation_Internal** @ `05E21E60` [host] — Capture the exact FLocationData the host is travelling to, including Temporary-location data that exists only in the host's PlayerData and cannot be looked up on the client.
  - behaviour: Observe-only detour, call the original. rcx=WCO, rdx=const FLocationData* Old, r8=const FLocationData* New, r9b=bDontClearDockingPoint, [rsp+0x20]=bWithoutWritingLocationState, [rsp+0x28]=bDontAutoSave. Snapshot New->LocationID(0x30), SystemID(0x3C), Seed(0x44), StaticLevel(0x110), LocationType(0x130), LocationCategory(0x131), MapAssetString(0x48/0x50). If LocationType == 4 (Temporary), copy the whole 448-byte struct (or at minimum enough to re-add it to the client's PlayerData->TemporaryLocations at 0x14B8) — GetLocationData will not find it otherwise. Queue this as the post-travel state payload.
  - risk: Low if read-only. Do NOT try to reorder or suppress the original — it performs the host's save bookkeeping (SaveLocationState, seeds, docking-point clear) that AESGameModeBase::StartPlay depends on after the load. Never dereference the returned FLocationData& across the travel; the storage belongs to UGameData/UPlayerData.
- **ULocationLib::SaveLocationState** @ `05E69190` [client] — Prevent the client from writing the host's world into its own save file during a co-op session.
  - behaviour: Detour; if the local world's net mode is NM_Client, return immediately without calling the original. On the host, always call the original.
  - risk: Medium. If ES2 client code later reads back a location state it expected to have written, it will see stale data. Mitigate by also blocking client-side ChangeLocation_Internal (05E21E60) so the client never enters the save path at all.
- **ChangeLocation_Internal (client-side guard)** @ `05E21E60` [client] — A client must never run the authoritative location-change bookkeeping locally; it must only follow the server's travel order.
  - behaviour: In the same detour used for observation: if net mode == NM_Client, return without calling the original. The client's PlayerData location fields are instead set from the host's synced payload just before/after the map load.
  - risk: Medium. Some client-side ES2 UI paths (rift entry, leviathan return, ship-viewer) route through ChangeLocation wrappers; blocking them makes those UI actions inert on the client, which is the intended co-op behaviour but must be verified per-feature.
- **APlayerController::ClientTravelInternal_Implementation** @ `04E7FA38` [client] — Observe the incoming server travel order on the client so the mod can pause its own tickers, apply the pending PlayerData payload, and fade the screen before the world is torn down.
  - behaviour: Observe-only detour then call the original. rcx=this, rdx=const FString* URL, r8d=ETravelType, r9b=bSeamless, [rsp+0x28]=FGuid* (hidden pointer — do not read it as an inline value).
  - risk: Low. Never block this call; blocking it desynchronises the session permanently.
- **AESGameModeBase::StartPlay** @ `02B3BF24` [host] — Post-travel host re-arm: confirm the listen server survived the LoadMap and re-broadcast the authoritative location state to the (re)connecting client.
  - behaviour: Post-detour (call the original first, then run your code). rcx=this. Check World->NetDriver (UWorld+0x38); if null, call UGameInstance::EnableListenServer(GI, true, 7777) (04A2F090). Then push the PlayerData payload captured in the ChangeLocation_Internal hook. Note PlayerData is cached into GameMode+0x518 by the original, and AGameMode::StartPlay (called at 02B3C89C) is what drives PostLogin/RestartPlayer for the rejoining client — so re-arming must happen before that if you post-detour at the very end, otherwise arrange the re-listen in a pre-detour instead.
  - risk: Medium. Ordering matters: the original calls AGameMode::StartPlay part-way through, so a pure post-detour runs after the match has already started. Prefer a pre-detour for the EnableListenServer check and a separate post point (or a one-shot timer) for the state broadcast.
- **AESPlayerController::InputChargeTravelMode** @ `05E0D0F0` [host] — Optional: gate the jump on both players being ready (co-op 'ready to jump' vote) instead of letting either player yank the other out of a fight.
  - behaviour: Pre-detour, rcx=this. If the co-op partner has not confirmed, return without calling the original (the jump charge simply never starts). Otherwise call through. Pair with InputChargeTravelModeReleased (05E0D204).
  - risk: Low mechanically, but it changes game feel and can soft-lock the session if the ready flag is never cleared. Add a timeout.
- **UJumpDriveComponent::SetTargetLocation / ForceImmediateJump** @ `05D77934` [host] — Drive the partner's (or the host's) jump programmatically so both ships leave through ES2's own delegate chain rather than a synthetic travel.
  - behaviour: Call, do not hook. SetTargetLocation is reflected and can be invoked via ProcessEvent by name, or just write the FName at UJumpDriveComponent+0x378. ForceImmediateJump(FName, bool) is rcx=this, rdx=FName by value, r8b=bool — it drives OnJumpCompleted, which reaches AESGameModeBase::PlayerJumpCompleted and therefore the normal ChangeLocation path.
  - risk: Low. Only ever do this on the host; on a client the jump drive is a simulated proxy and the game mode does not exist.

## Open questions

- Is bUseSeamlessTravel actually false at runtime? The C++ CDO leaves bit0 of AGameModeBase+0x328 clear (verified in both constructors), but ES2's real game mode is a Blueprint subclass whose CDO could set it. Read `test byte [GameModeCDO+0x328], 1` live before relying on non-seamless behaviour.
- Is bUseWorldOriginShifting (UESGameInstance+0x214) true in the shipped CDO/config? UESGameInstance::Init only pushes the refcount when it is. Read UESGameInstance::InstancePointer(+0x214) and (+0x1150) live to confirm rebasing is actually armed in-game before deciding how aggressively to suppress it.
- Does UWorld::ServerTravel with bAbsolute=false actually preserve the listen socket in this build? The evidence chain is solid (FURL copies Base only for TRAVEL_Relative at 0509C1ED; EnableListenServer adds 'Listen' to LastURL; LoadMap re-Listens at 02B45C4C) but it must be confirmed by watching World->NetDriver (UWorld+0x38) after the first travel.
- What exactly is FWorldContext+0x88 (inside FSeamlessTravelHandler) that ServerTravel tests? If it is ever non-zero spuriously, ServerTravel silently returns true without travelling. Worth logging on the first few jumps.
- Does UWorld::ServerTravel's CanServerTravel (vtable slot 255) override in AESGameModeBase reject anything? I did not disassemble the ES2 override — if it vetoes, the hook must fall back to the original OpenLevel path.
- Which Blueprint sets UPlayerData::CurrentDockingPoint (0x1508)? No native symbolised function writes it (I scanned every .text occurrence of the 0x1508 displacement) — it is set from Blueprint. That means docking-state sync has to be done by reading/writing the property, and the co-op partner's docking is entirely BP-driven, which needs live observation.
- Does the EmptyTransitionMap collapse optimisation break anything? ES2 uses the stub map specifically to force a same-level reload; ServerTravel to the identical map name should reload just as well, but the assumption should be tested on a 'reenter current location' action (UGameplayLib::ReenterLocationAndKeepPosition, 05E37AC0).
- How much of PlayerData must be synced before the client's map BeginPlay runs versus after? The ordering window between the client's LoadMap and the first client-side UMapLib::GetCurrentLocationData call is unverified; if it is too tight, the payload may need to be pushed before the ClientTravel RPC rather than after.
- Are Map_TravelMode_S## levels reachable at all in co-op? They are entered when NewLocation.LocationID == None, and the system-map flight mode has its own AESGameModeBase path (RightBeforeBeginPlayIsCalledForAllActorsInTravelMode spawns only missions and challenges). Untested whether replication behaves there.
