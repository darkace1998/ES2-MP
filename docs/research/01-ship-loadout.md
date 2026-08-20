# ES2 co-op research: ship

_Auto-generated from the PDB research workflow (2026-08-19)._

## Summary

HOW ES2 SPAWNS A PLAYER SHIP
`AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation` (RVA 05DEC54C, ~0x2800 bytes) does NOT route through `UGameplayLib::SpawnPlayerShip`. Its flow (from disassembly): (1) `this->PlayerData(+0x518) = UGameplayLib::GetPlayerData()`, then `UPlayerData::ReinitGlobalAttributesWithCurrentShip()`; (2) a very long block that resolves the spawn transform from ALocationInfo / player start / docking station / jump gate / mission-task data (host-global state, all keyed off the single process UPlayerData — it completely ignores the passed-in `AController*` except for `AGameModeBase::FindPlayerStart`); (3) `FActorSpawnParameters` ctor, sets Owner=`this->InstigatorPawn?`(+0x1a0), sets bDeferConstruction, collision-handling=2; (4) `UWorld::SpawnActor(this->DefaultPawnClass /*AGameModeBase+0x2E0*/, &Transform, Params)` → cast to AESPawn, stored in `this->ESPlayerPawn(+0x508)`; (5) **`FShipData tmp = UInventoryLib::GetCurrentShip(); pawn->ShipData(+0x4B8) = tmp; tmp.~FShipData();`** ← this is the entire reason clients get an empty ship: `GetCurrentShip()` reads the single process-global `UPlayerData` (`*(void**)RVA 09AC5E88` → `+0x1C0`) at index `UPlayerData::CurrentShip(+0x119C)` from `UPlayerData::Ships(+0x468, TArray<FShipData>, stride 0x3D0)`; (6) `ISavableInterface::StaticRestoreState(pawn, {&pawn->ShipData.GenericSaveData(+0x830), 0x20A}, 0.0f, true)`; (7) `AActor::FinishSpawning(pawn, Transform, false, nullptr, 0)`; (8) `UGameplayLib::GetESPlayerController()->UpdateDualSenseBoostTriggerFunctionality()` + a timer.

So the pawn CLASS is always `AGameModeBase::DefaultPawnClass` (BP_Ship_Player_C) — ship identity/appearance/equipment are 100% carried by the `FShipData` written into `AESPawn+0x4B8` BEFORE `FinishSpawning`. `UGameplayLib::SpawnPlayerShip(UObject*, int, const FTransform&, bool)` (05E3EE5C) is the same pattern in miniature: GetGameMode → GetWorld → DefaultPawnClass(+0x2E0) → deferred SpawnActor → `pawn->ShipData = UInventoryLib::GetPlayerShip(PlayerShipIndex)` → optional SetActorEnableCollision(false) → FinishSpawning. The `int` is an index into the LOCAL `UPlayerData::Ships` array (verified in `UInventoryLib::GetPlayerShip`: bounds-checks against `+0x470` then copy-constructs `Ships[i]`); the `bool` is `bDisableCollision`. `UGameplayLib::SpawnPawnFromClass` (05E3EB1C) is unrelated to loadout — BeginDeferredActorSpawnFromClass + FinishSpawningActor, never touches ShipData.

THE SERIALIZABLE LOADOUT BLOB
`FShipData` (976 = 0x3D0 bytes, `FShipData::StaticStruct` 05BC6690) is the LIVE form and contains raw UObject pointers (`+0x10 UInventory* Inventory`, `+0x18 UItem* ShipItemInstance`, `+0x20 UItem* UltimateDevice`) — NOT transportable. Its serializable twin is **`FShipDataState` (1592 = 0x638 bytes, `FShipDataState::StaticStruct` 05BC66C8)** which replaces those with `FInventoryState Inventory(+0x10, 176B)` and `FItemState ShipItemState(+0xC0, 464B)`. `FInventoryState` is 11 `TArray<FItemState>` (PrimaryWeapons, SecondaryWeapons, EnergyCores, Sensors, Shields, CargoUnits, Platings, Thrusters, Devices, Consumables, Cargo). `FItemState` is a pure-value record: `FName ItemTemplateID(+0x0)`, Seed, NameSeed, ItemLevel, Rarity, Amount, Ammo, Condition, `TArray<FName> InstalledChipIDs`, `TArray<FName> Catalysts`, `FName AffixID`, Grade, `TArray<FItemAttributeState> AttributeStates`, `TMap<FName,float>` original values, `TMap<FName,FESVariant> CustomData`. So an item = template FName + seed + level/rarity + explicit attribute states — fully reconstructible on any machine that has the same data tables. Ship type is `ShipItemState.ItemTemplateID` (`UGameplayLib::GetPlayerShipType` reads `pawn->ShipData.ShipItemInstance->ItemTemplateID(UItem+0x28)` → `UItemTemplates::GetShipTypeFromShipItemID`).

The exact round-trip pair exists and is what the save system uses:
- `FShipData::GetShipDataState()` — 05D968B8 — live → blob (used by `UPlayerData::CreateSaveableState` 05DFF7EC to fill `UPlayerData::ShipsSaveState(+0x478, TArray<FShipDataState>)`).
- `UInventory::CreateShipDataFromState(FShipDataState&)` — 05E98CF0 — blob → live. Internally: default-ctor FShipData, `Inventory = UInventory::CreateInventoryFromState(state.Inventory)` (which NewObjects a UInventory under a global outer and `UItem::CreateItemFromState` per item, `UItemLib::UpdateItemWithTemplate`, affix/catalyst rebuild), `ShipItemInstance = UItem::CreateItemFromState(state.ShipItemState)`, `UltimateDevice = UItemLib::CreateUltimateDeviceForShip(...)`, then copies ShipModules / ShipColors / appearances / decal & particle FNames / GenericSaveData.

All 21 `FShipDataState` members are reflected UPROPERTYs (verified against `Z_Construct_UScriptStruct_FShipDataState_Statics::NewProp_*`), and its `TCppStructOps<FShipDataState>` vtable entries are all ICF-folded onto the generic default impls (Serialize/NetSerialize/ExportTextItem all live at 01300270, the `return false` stub) — i.e. **no custom NetSerialize; it serializes purely through UE's generic tagged-property path**. That means it can be shipped as a blob using `FMemoryWriter` + `FObjectAndNameAsStringProxyArchive` + `UScriptStruct::SerializeItem(FArchive&, void*, const void*)` (name-as-string proxy is MANDATORY — raw FName indices are per-process), or as text via `UScriptStruct::ExportText`/`ImportText`.

RE-EQUIPPING AN ALREADY-SPAWNED PAWN
`AESPawn::UpdateShipModules(bool bRespawnWeapons)` (05DEF910) is the "apply my ShipData to myself" routine: early-returns if `this->ShipData.ShipItemInstance(+0x4D0)` is null; loads module meshes/animations via `UItemLib::GetShipModule` + `StaticLoadObject`; and if the bool is TRUE calls `PrimaryWeapons(+0x380)->SpawnWeapons()` AND `SecondaryWeapons(+0x388)->SpawnWeapons()` (0131FA98, which does DestroyWeapons → SpawnWeaponInstancesForSlot → EquipWeapon). The reflected UFunction `AESPawn::ApplyShipData` (0 params) calls `UpdateShipModules(false)` — i.e. visuals only, NOT weapons — then applies colors/decals/skin. `AESPawn::GetCurrentShipData` is a reflected 0-param UFunction returning a copy of `AESPawn+0x4B8`.

HOST-LOCAL TRAPS (do not call these for a remote client's pawn)
`UGameplayLib::GetPlayerData()` reads one process-global pointer — there is exactly ONE UPlayerData per process, it can never hold a second player's ships. `UInventoryLib::GetCurrentShip/GetPlayerShip/GetCurrentShipIndex/SetCurrentShip/ReplaceShipData`, `UGameplayLib::RefreshPlayerShipData`, `SaveShipState`, `RestoreShipState`, `IsPlayerCurrentlyFlyingShipWithIndex`, `GetEquippedPlayerItemOfCategory`, `UInventoryLib::ReinitShipAfterPotentialChanges`, `UGameplayLib::ReloadLocationAfterShipChange` ALL operate on `GetPlayerData()` and/or `UGameplayStatics::GetPlayerPawn(world, 0)` — on the listen server that is the HOST's own ship. Calling any of them to "fix" a client pawn will corrupt the host's save state. `UGameplayLib::SaveShipState/RestoreShipState` are about volatile combat state only (shield/armor/hull/ULT ratios, weapon energy, consumables, secondary ammo, device cooldowns → `FShipSaveState`, 88 bytes) — they are NOT the loadout.

REPLICATION
There is no `AESPawn::GetLifetimeReplicatedProps` symbol, so `AESPawn::ShipData` does not replicate. Setting it server-side fixes the server-authoritative ship (weapons, stats, hitpoints, devices) but the joining client's local proxy will still look/behave default unless the mod applies the same FShipData locally on the client too — which is free, because the client already owns the data.

## Key functions

- **AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation** @ `05DEC54C` unique=True — Resolves spawn transform from host-global location/docking/mission state, spawns DefaultPawnClass (gamemode+0x2E0) deferred via UWorld::SpawnActor, stores it in AESGameModeBase::ESPlayerPawn(+0x508), copies UInventoryLib::GetCurrentShip() into pawn->ShipData(+0x4B8), StaticRestoreState, FinishSpawning. Ignores which controller it is spawning for as far as loadout is concerned.
  - `public: virtual class APawn * __cdecl AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation(class AController *, struct UE::Math::TTransform<double> const &)`
  - ABI: RCX = AESGameModeBase* this; RDX = AController* NewPlayer; R8 = const FTransform* (96-byte struct, by pointer). Returns APawn* in RAX. Function body spans 0x5DEC54C..~0x5DEF030 (~0x2800 bytes), so MinHook needs enough prologue room (prologue is `mov [rsp+20],rbx; push rbp; push rsi; push rdi; push r
- **UGameplayLib::SpawnPlayerShip** @ `05E3EE5C` unique=True — GetGameMode(ctx) -> GetWorld() -> uses gameMode->DefaultPawnClass(+0x2E0) checked IsChildOf(APawn); UWorld::SpawnActor deferred; casts to AESPawn; `pawn->ShipData(+0x4B8) = UInventoryLib::GetPlayerShip(PlayerShipIndex)`; SetActorEnableCollision(false) if bDisableCollision; AActor::FinishSpawning. The int indexes the LOCAL UPlayerData::Ships array, so it is useless for a remote client unless the cl
  - `public: static class APawn * __cdecl UGameplayLib::SpawnPlayerShip(class UObject const * WorldContextObject, int PlayerShipIndex, struct UE::Math::TTransform<double> const & Transform, bool bDisableCollision)`
  - ABI: Plain fastcall, no sret: RCX = UObject* WorldContextObject, EDX = int PlayerShipIndex, R8 = const FTransform*, R9B = bool bDisableCollision. Returns APawn* in RAX. Reflected: Z_Construct_UFunction_UGameplayLib_SpawnPlayerShip exists, params WorldContextObject/PlayerShipIndex/Transform/bDisableCollis
- **UGameplayLib::SpawnPawnFromClass** @ `05E3EB1C` unique=True — UGameplayStatics::BeginDeferredActorSpawnFromClass(ctx, class, transform, collisionHandling=?, owner=null, scaleMethod=2) then immediately UGameplayStatics::FinishSpawningActor. Never touches ShipData and gives no window between Begin and Finish — NOT usable for loadout injection.
  - `public: static class AESPawn * __cdecl UGameplayLib::SpawnPawnFromClass(class UObject const * WorldContextObject, class TSubclassOf<class AActor> PawnClass, struct UE::Math::TTransform<double> const & SpawnTransform)`
  - ABI: RCX = UObject* WorldContextObject, RDX = TSubclassOf<AActor>* (BY HIDDEN POINTER — verified: it copy-constructs from [RDX] into a stack slot and forwards &copy), R8 = const FTransform*. Returns AESPawn* (or null) in RAX.
- **FShipData::GetShipDataState** @ `05D968B8` unique=True — Converts the live FShipData (with UInventory*/UItem*) into the fully value-typed FShipDataState: Inventory->GetInventoryState(), ShipItemInstance->GetItemState(), plus ShipModules/ShipColors/appearances/decal+particle FNames/GenericSaveData. This is exactly what UPlayerData::CreateSaveableState uses to build the .sav's ShipsSaveState array.
  - `public: struct FShipDataState __cdecl FShipData::GetShipDataState(void)`
  - ABI: MSVC member-fn sret order: RCX = FShipData* this, RDX = FShipDataState* return buffer (1592 bytes, RAW UNINITIALIZED — the function default-constructs into it via FShipDataState::FShipDataState(void) 05BC57C8). Returns RDX in RAX. Caller must later call FShipDataState::~FShipDataState (05AA10D8).
- **UInventory::CreateShipDataFromState** @ `05E98CF0` unique=True — Rebuilds a fully live ship: UInventory::CreateInventoryFromState (NewObject<UInventory> under a global outer, then UItem::CreateItemFromState per FItemState + UItemLib::UpdateItemWithTemplate + price/affix/catalyst regeneration), UItem::CreateItemFromState for the ship item, UItemLib::CreateUltimateDeviceForShip, then copies modules/colors/appearances/GenericSaveData. THIS IS THE FUNCTION THAT TUR
  - `public: static struct FShipData __cdecl UInventory::CreateShipDataFromState(struct FShipDataState & State)`
  - ABI: Static sret: RCX = FShipData* return buffer (976 bytes, RAW UNINITIALIZED — it calls FShipData::FShipData(void) 012D2B6C on it), RDX = FShipDataState* (non-const ref; it MOVES/steals the Name FText and may mutate the state, so pass a scratch copy you own). Returns RCX in RAX. Caller owns the FShipDa
- **FShipData::operator=** @ `01805FD8` unique=True — Deep-ish assigns FShipData (copies UObject pointers, deep-copies TArrays/FText/FShipColors). Use to write a rebuilt FShipData into AESPawn::ShipData at +0x4B8.
  - `public: struct FShipData & __cdecl FShipData::operator=(struct FShipData const &)`
  - ABI: RCX = destination FShipData* (e.g. pawn+0x4B8), RDX = source FShipData*. This is the exact call the game itself uses at 05DEEEA6 (gamemode) / 05E3F0AE (SpawnPlayerShip) / 05DDADE5 (DockableStation::SpawnShip).
- **FShipData::FShipData(void) / ::~FShipData / copy ctor** @ `012D2B6C` unique=True — Lifetime management for the 976-byte FShipData scratch buffers the mod will need.
  - `public: __cdecl FShipData::FShipData(void)  [dtor 0121B020 unique=1; copy ctor 0121A478 unique=1]`
  - ABI: RCX = this. sizeof(FShipData)=976 (0x3D0), confirmed by the stride `imul rdx, rax, 0x3d0` in UInventoryLib::GetPlayerShip.
- **FShipDataState::FShipDataState(void) / ::~FShipDataState / ::StaticStruct** @ `05BC57C8` unique=True — Lifetime + reflection handle for the transport blob. StaticStruct() is what you feed to UScriptStruct::SerializeItem / ExportText / ImportText / InitializeStruct / DestroyStruct.
  - `public: __cdecl FShipDataState::FShipDataState(void)  [dtor 05AA10D8 unique=1; copy-assign 05BC5A34 unique=1; move ctor 05D8FE40 unique=1; StaticStruct 05BC66C8 unique=1]`
  - ABI: RCX = this. sizeof(FShipDataState)=1592 (0x638), confirmed by the stride `imul rcx, r15, 0x638` in UPlayerData::CreateSaveableState. StaticStruct() takes no args and returns UScriptStruct* in RAX.
- **AESPawn::UpdateShipModules** @ `05DEF910` unique=True — Re-reads this->ShipData(+0x4B8): loads the ship module meshes/anims (UItemLib::GetShipModule + StaticLoadObject of FSoftObjectPath), rebuilds socket-driven sub-meshes, and when bRespawnWeapons is TRUE calls PrimaryWeapons(+0x380)->SpawnWeapons() and SecondaryWeapons(+0x388)->SpawnWeapons(). This is the correct post-hoc 'make the pawn actually be this ship' call.
  - `public: void __cdecl AESPawn::UpdateShipModules(bool bRespawnWeapons)`
  - ABI: RCX = AESPawn* this, DL = bool. No return. Early-returns immediately if this->ShipData.ShipItemInstance (this+0x4D0) is null, so it is safe to call blind.
- **AESPawn::ApplyShipData (UFunction, native thunk in execApplyShipData)** @ `01946E98` unique=True — if (pawn->bGetShipModulesFromShipData /*+0x8B8*/) UpdateShipModules(FALSE); then conditionally applies ship colors (+0x9D8 flag), decals (+0xA78), projection decal (+0xAE4) via ApplyShipDataShipColors/ApplyShipDataShipDecals/ApplyShipSkin. NOTE: passes false, so it does NOT respawn weapons — for weapons you must call UpdateShipModules(true) directly.
  - `public: static void __cdecl AESPawn::execApplyShipData(class UObject *, struct FFrame &, void *const)  — UFunction 'ApplyShipData', 0 params, no return`
  - ABI: Call via ProcessEvent(pawn, FindFunction("ApplyShipData"), nullptr) — zero-size param frame. There is no separate native AESPawn::ApplyShipData symbol; the body is inlined into the exec stub.
- **UWeaponComponent::SpawnWeapons** @ `0131FA98` unique=True — DestroyWeapons() then, per slot, SpawnWeaponInstancesForSlot(i) and EquipWeapon(firstNonEmptySlot,...). This is what materialises actual AWeaponBase actors from the ship's inventory. Called by ADockableStation::SpawnShip and by UpdateShipModules(true).
  - `public: void __cdecl UWeaponComponent::SpawnWeapons(void)`
  - ABI: RCX = UWeaponComponent* this. Get the two instances from AESPawn+0x380 (PrimaryWeapons) and AESPawn+0x388 (SecondaryWeapons).
- **AESPawn::ApplyShipSkin / AESPawn::ApplyShipDataShipDecals** @ `05DE03DC` unique=True — Cosmetic application of ShipData.ShipSkinID / DecalsID / ShipColors onto the pawn's materials.
  - `public: void __cdecl AESPawn::ApplyShipSkin(void)   [ApplyShipDataShipDecals = 05DE039C]`
  - ABI: RCX = this, no args, no return. Both also exist as reflected UFunctions (Z_Construct_UFunction_AESPawn_ApplyShipSkin / _ApplyShipDataShipDecals).
- **UInventory::CreateInventoryFromState** @ `05E9858C` unique=True — Per-category loop: FInventoryState::GetItemStatesOfCategory -> UItem::CreateItemFromState -> UItemLib::UpdateItemWithTemplate/GenerateItemPrice/DamageItem/OverheatItem, pushing into UInventory::GetItemsOfCategory_Internal. It does consult UGameplayLib::GetPlayerData() (host's) for item-replacement/level bookkeeping — a soft dependency worth watching.
  - `public: static class UInventory * __cdecl UInventory::CreateInventoryFromState(struct FInventoryState & State)`
  - ABI: RCX = FInventoryState* (non-const ref). Returns UInventory* in RAX. Internally NewObject<UInventory>(*(UObject**)RVA 09D8BEB8) — a global outer, NOT the player data, so no host-save coupling.
- **UItem::CreateItemFromState** @ `05E98820` unique=True — NewObject<UItem>, TransformDeprecatedIDs, copies template ID/seed/level/rarity/grade/affix/chips/catalysts/CustomData, rebuilds attributes via UItemLib::MakeAttributeInstanceFromState, CreateAffixBaseValuesForItem, CreateCatalystBaseValuesForItem, GenerateItemName. Proves an item is fully reconstructible from FItemState alone.
  - `public: static class UItem * __cdecl UItem::CreateItemFromState(struct FItemState const & State)`
  - ABI: RCX = const FItemState*. Returns UItem* in RAX.
- **UInventory::GetInventoryState / UItem::GetItemState** @ `05E9ADE4` unique=True — Live -> value-state conversion for the sub-objects; used by FShipData::GetShipDataState.
  - `public: struct FInventoryState __cdecl UInventory::GetInventoryState(void)   [UItem::GetItemState = 05E9B940]`
  - ABI: Member sret: RCX = this, RDX = FInventoryState*/FItemState* return buffer.
- **UGameplayLib::GetPlayerData** @ `0127398C` unique=True — Fetches THE single per-process UPlayerData off the ES2 game-instance global. Proof that the host can never source a second player's ships locally, and that the client can always read its own ships after `open <ip>` (the game instance survives travel).
  - `public: static class UPlayerData * __cdecl UGameplayLib::GetPlayerData(void)`
  - ABI: No args. Body: `rax = *(void**)(image+0x09AC5E88); if (rax) rax = *(void**)(rax+0x1C0);` Returns UPlayerData* in RAX (null-safe).
- **UInventoryLib::GetCurrentShip / GetPlayerShip / GetCurrentShipIndex** @ `0121A1E0` unique=True — GetCurrentShip returns a copy of PlayerData->Ships[PlayerData->CurrentShip(+0x119C)]; GetPlayerShip(i) returns a bounds-checked copy of Ships[i] (Ships data ptr +0x468, count +0x470, stride 0x3D0); GetCurrentShipIndex returns +0x119C. These are the CLIENT-side entry points for grabbing 'my ship'.
  - `public: static struct FShipData __cdecl UInventoryLib::GetCurrentShip(void)   [GetPlayerShip(int) = 01805EE0; GetCurrentShipIndex(void) = 01954FB8]`
  - ABI: Static sret: GetCurrentShip -> RCX = FShipData* retbuf (raw memory). GetPlayerShip -> RCX = FShipData* retbuf, EDX = int index. GetCurrentShipIndex -> no args, int in EAX. All three are reflected UFunctions.
- **UScriptStruct::SerializeItem (FArchive overload)** @ `01688C48` unique=True — Generic tagged-property (de)serialization of any UScriptStruct. Since TCppStructOps<FShipDataState> has no custom Serialize (all folded to the return-false stub at 01300270), this is the full and only serialization path for FShipDataState.
  - `public: void __cdecl UScriptStruct::SerializeItem(class FArchive & Ar, void * Value, void const * Defaults)`
  - ABI: RCX = UScriptStruct* this (= FShipDataState::StaticStruct()), RDX = FArchive* (pass the FObjectAndNameAsStringProxyArchive), R8 = void* struct instance, R9 = const void* defaults (pass nullptr). There is a second, distinct FStructuredArchiveSlot overload at 0121943C — do not confuse them.
- **FMemoryWriter / FMemoryReader / FObjectAndNameAsStringProxyArchive constructors** @ `0225F014` unique=True — The standard UE memory-archive stack. THE NAME-AS-STRING PROXY IS MANDATORY: FName indices are per-process, so serializing FShipDataState through a bare FMemoryWriter would produce garbage item IDs on the other machine.
  - `public: __cdecl FMemoryWriter::FMemoryWriter(class TArray<unsigned char,TSizedDefaultAllocator<32>> & InBytes, bool bIsPersistent, bool bSetOffset, class FName InArchiveName)   [FMemoryReader::FMemoryReader(const TArray<uint8>&, bool) = 015ADFC4; FObjectAndNameAsStringProxyArchive::FObjectAndNameAsStringProxyArchive(FArchive&, bool bLoadIfFindFails) = 01685A30]`
  - ABI: FMemoryWriter: RCX = this (168-byte stack buffer), RDX = TArray<uint8>*, R8B = bIsPersistent, R9B = bSetOffset, [rsp+0x20] = FName (8 bytes by value). FMemoryReader: RCX = this (168 bytes), RDX = const TArray<uint8>*, R8B = bIsPersistent. FObjectAndNameAsStringProxyArchive: RCX = this (160 bytes), R
- **UScriptStruct::ExportText / UScriptStruct::ImportText** @ `02D01634` unique=True — Text round-trip of any UScriptStruct. Directly usable over a string RPC channel with no base64 step; larger payload than the binary route but trivially debuggable.
  - `public: void __cdecl UScriptStruct::ExportText(class FString & ValueStr, void const * Value, void const * Defaults, class UObject * OwnerObject, int PortFlags, class UObject * ExportRootScope, bool bAllowNativeOverride) const   [ImportText(const TCHAR*, void*, UObject*, int, FOutputDevice*, const FString& StructName, bool) = 02D06F3C]`
  - ABI: ExportText: RCX = UScriptStruct*, RDX = FString* out, R8 = const void* value, R9 = const void* defaults, then stack args OwnerObject/PortFlags/ExportRootScope/bAllowNativeOverride. ImportText returns const TCHAR* (null on failure) in RAX. There is a second ImportText overload taking TFunctionRef at 
- **UScriptStruct::InitializeStruct / DestroyStruct** @ `011FC20C` unique=True — Alternative to calling FShipDataState's ctor/dtor by RVA — reflection-driven zero-init/teardown of a struct buffer, useful for the receive side before ImportText/SerializeItem.
  - `public: virtual void __cdecl UScriptStruct::InitializeStruct(void * Dest, int ArrayDim) const   [DestroyStruct(void*, int) = 011FC060]`
  - ABI: RCX = UScriptStruct*, RDX = void* memory, R8D = count (1). Virtual — but these RVAs are the UScriptStruct implementations, safe to call directly since we always pass the concrete FShipDataState::StaticStruct().
- **UWorld::SpawnActor / AActor::FinishSpawning / FActorSpawnParameters ctor** @ `014DB504` unique=True — The deferred-spawn pair. Replicating SpawnPlayerShip's exact pattern (SpawnActor with bDeferConstruction, write ShipData, FinishSpawning) is the cleanest way to get a client's loadout in before the construction script/BeginPlay runs.
  - `public: class AActor * __cdecl UWorld::SpawnActor(class UClass *, struct UE::Math::TTransform<double> const *, struct FActorSpawnParameters const &)   [AActor::FinishSpawning(const FTransform&, bool, const FComponentInstanceDataCache*, ESpawnActorScaleMethod) = 01569FCC; FActorSpawnParameters::FActorSpawnParameters(void) = 014DB4C8]`
  - ABI: SpawnActor: RCX = UWorld*, RDX = UClass*, R8 = const FTransform*, R9 = const FActorSpawnParameters* (128 bytes). FinishSpawning: RCX = AActor*, RDX = const FTransform*, R8B = bIsDefaultTransform, R9 = const FComponentInstanceDataCache* (null), [rsp+0x20] = ESpawnActorScaleMethod (0). FActorSpawnPara
- **ADockableStation::SpawnShip** @ `05DDAC58` unique=True — The one place in the game that spawns an AESPawn from an EXPLICIT FShipData rather than from player data: SpawnActor(deferred) -> `pawn->ShipData(+0x4B8) = *InShipData` -> USaveGameComponent::SetSpawnMethod -> FinishSpawning -> AttachToActor -> UFactionComponent::SetFaction -> AESPawn::InitSecondaryWeaponsVisibility(01516780) -> SecondaryWeapons(+0x388)->SpawnWeapons(). Copy this sequence.
  - `private: void __cdecl ADockableStation::SpawnShip(class TSubclassOf<class AESPawn>, struct FShipData *, struct UE::Math::TTransform<double> const &, bool, bool, int)`
  - ABI: Private and takes 6 args (2 on the stack) — not recommended as a call target, but it is the canonical reference implementation.
- **UGameplayLib::GetPlayerShipType** @ `01804388` unique=True — pawn->ShipData.ShipItemInstance(+0x4D0)->ItemTemplateID(UItem+0x28) -> UItemTemplates::GetShipTypeFromShipItemID(FName). Confirms ship identity is an FName template ID on the ship UItem, not a pawn class. Good verification/telemetry call on the host after injecting a client loadout.
  - `public: static class TEnumAsByte<enum EShip::Type> __cdecl UGameplayLib::GetPlayerShipType(class AESPawn *)`
  - ABI: UNUSUAL: returns a class type by hidden pointer even though it is 1 byte. RCX = TEnumAsByte<EShip::Type>* retbuf, RDX = AESPawn*. Writes 0 to *RCX if the pawn or its ship item is null.
- **UGameplayLib::SaveShipState / RestoreShipState** @ `05E3A564` unique=True — NOT the loadout. They read/write FShipSaveState (88 bytes: ShieldRatio/ArmorRatio/HullRatio/ULTRatio/BoostEnergy, TArray<float> WeaponEnergyRatios, TArray<FItemContainerContent> Consumables, TArray<int> SecondaryAmmos, TArray<FFloatArrayHelper> DeviceCooldowns) against UGameplayLib::GetESPlayerPawn(ctx) + GetPlayerData() — i.e. host-local volatile combat state only. Listed here so nobody mistakes 
  - `public: static void __cdecl UGameplayLib::SaveShipState(class UObject const *)   [RestoreShipState(class UObject const *, bool bFullyRestoreEverything) = 05E3986C]`
  - ABI: SaveShipState: RCX = UObject* WorldContext. RestoreShipState: RCX = UObject*, DL = bool. Both reflected.
- **UGameplayLib::RefreshPlayerShipData / UInventoryLib::ReinitShipAfterPotentialChanges / UInventoryLib::ReplaceShipData / UInventoryLib::SetCurrentShip** @ `05E37DA4` unique=True — DANGER LIST — all of these hard-code the local player: RefreshPlayerShipData uses UGameplayStatics::GetPlayerController(ctx,0)->GetPawnOrSpectator() and writes back into GetPlayerData(); ReinitShipAfterPotentialChanges uses GetPlayerPawn(0) and calls UInventoryLib::PlayerEquipmentChanged / UDeviceComponent::ReInitNewDevices / AESGameModeBase::SpawnPerks; ReplaceShipData writes GetPlayerData()->Shi
  - `public: static void __cdecl UGameplayLib::RefreshPlayerShipData(void)   [UInventoryLib::ReinitShipAfterPotentialChanges(void) = 0178F6DC; UInventoryLib::ReplaceShipData(const FShipData&, int) = 05E4E740; UInventoryLib::SetCurrentShip(int) = 05E5059C; UPlayerData::SetCurrentShip(int) = 05E16CD8]`
  - ABI: RefreshPlayerShipData: no args. ReinitShipAfterPotentialChanges: no args. ReplaceShipData: RCX = const FShipData*, EDX = int index. SetCurrentShip: EDX/ECX = int.
- **UPlayerData::CreateSaveableState** @ `05DFF7EC` unique=True — The save-prep pass: ULocationLib::SaveLocationState, UMissionLib::SaveStateOfCurrentlySpawnedMissions, UGameplayLib::RefreshPlayerShipData, SaveRiftRewardItems, StationCargo/QuestItems/CraftingResources -> FInventoryState, then rebuilds ShipsSaveState(+0x478) by calling FShipData::GetShipDataState() over Ships(+0x468). Documents the canonical FShipData->FShipDataState direction, but is too side-ef
  - `public: void __cdecl UPlayerData::CreateSaveableState(void)`
  - ABI: RCX = UPlayerData* this. Heavy side effects.
- **UGameplayStatics::SaveGameToMemory / LoadGameFromMemory** @ `04A4A420` unique=True — Whole-save transport fallback: client CreateSaveableState + SaveGameToMemory -> bytes -> host LoadGameFromMemory -> a SEPARATE UPlayerData whose ShipsSaveState/SavedPlayerShip(+0x488, FShipDataState) can be read without touching the host's. Correct but heavy (megabytes) and CreateSaveableState has bad side effects on a live client. Use only as a fallback if per-struct serialization misbehaves.
  - `public: static bool __cdecl UGameplayStatics::SaveGameToMemory(class USaveGame *, class TArray<unsigned char,TSizedDefaultAllocator<32>> &)   [LoadGameFromMemory(const TArray<uint8>&) = 04A3E85C]`
  - ABI: SaveGameToMemory: RCX = USaveGame*, RDX = TArray<uint8>* out, bool in AL. LoadGameFromMemory: RCX = const TArray<uint8>*, returns USaveGame* in RAX. Note UPlayerData IS a USaveGame (layout says bases=[('USaveGame',0)], sizeof 8624).
- **UGameplayLib::GetESPlayerPawn / UGameplayLib::IsPlayerPawn** @ `013F0CC0` unique=True — Local-player pawn accessor. On the CLIENT this is the right way to grab 'my ship' pawn to read its ShipData(+0x4B8) — more accurate than GetCurrentShip() because it reflects in-flight equipment changes.
  - `public: static class AESPawn * __cdecl UGameplayLib::GetESPlayerPawn(class UObject const *)   [UGameplayLib::IsPlayerPawn(const AActor*) = 012A1810]`
  - ABI: RCX = UObject* WorldContext, returns AESPawn* in RAX.

## Types

### FShipData (size 976)
- 0x0000 Name FText — display name
- 0x0010 Inventory UInventory* — THE EQUIPMENT: live inventory holding all equipped weapons/devices/modules/cargo (not transportable)
- 0x0018 ShipItemInstance UItem* — the ship itself; its ItemTemplateID (UItem+0x28) is the ship type; null here makes UpdateShipModules bail out
- 0x0020 UltimateDevice UItem* — ULT device instance
- 0x0028 ShipModules TArray<FShipModuleState> — cosmetic/functional module slot assignments (FName ID + EShipModule::Type)
- 0x0038 AppearanceID FName
- 0x0040 SpecialBackerShipID FName
- 0x0048 ShipColors FShipColors (284B)
- 0x0164 DecalsID FName
- 0x016C ShipSkinID FName
- 0x0174 ThrustParticlesID FName
- 0x017C BoostParticlesID FName
- 0x0184 ProjectionDecal FProjectionDecal (60B)
- 0x01C0 CustomAppearances TArray<FShipAppearance>
- 0x01D0 OriginalAppearance FShipAppearance (400B)
- 0x0360 HealthRatio float
- 0x0364 ArmorRatio float
- 0x0368 UltimateRatio float
- 0x036C SelectedPrimaryIndex int — which primary weapon slot is active
- 0x0370 SelectedSecondaryIndex int
- 0x0374 SelectedConsumableIndex int
- 0x0378 GenericSaveData FGenericSaveDataMapV2 (88B) — what StaticRestoreState is fed from pawn+0x830

### FShipDataState (size 1592)
- 0x0000 Name FText
- 0x0010 Inventory FInventoryState (176B) — THE SERIALIZABLE EQUIPMENT LIST
- 0x00C0 ShipItemState FItemState (464B) — SERIALIZABLE SHIP IDENTITY (ItemTemplateID + seed + level + rarity)
- 0x0290 ShipModules TArray<FShipModuleState>
- 0x02A0 ShipColors FShipColors (284B)
- 0x03BC SpecialBackerShipID FName
- 0x03C4 AppearanceID FName
- 0x03CC DecalsID FName
- 0x03D4 ThrustParticlesID FName
- 0x03DC BoostParticlesID FName
- 0x03E4 ShipSkinID FName
- 0x03EC ProjectionDecal FProjectionDecal
- 0x0428 OriginalAppearance FShipAppearance (400B)
- 0x05B8 CustomAppearances TArray<FShipAppearance>
- 0x05C8 Health float
- 0x05CC ArmorRatio float
- 0x05D0 UltimateRatio float
- 0x05D4 SelectedPrimaryIndex int
- 0x05D8 SelectedSecondaryIndex int
- 0x05DC SelectedConsumableIndex int
- 0x05E0 GenericSaveData FGenericSaveDataMapV2 (88B)
- ALL 21 members are reflected UPROPERTYs; no custom TCppStructOps Serialize/NetSerialize (folded to the return-false stub at 01300270) => generic tagged-property serialization is complete and is the only path

### FInventoryState (size 176)
- 0x0000 PrimaryWeapons TArray<FItemState>
- 0x0010 SecondaryWeapons TArray<FItemState>
- 0x0020 EnergyCores TArray<FItemState>
- 0x0030 Sensors TArray<FItemState>
- 0x0040 Shields TArray<FItemState>
- 0x0050 CargoUnits TArray<FItemState>
- 0x0060 Platings TArray<FItemState>
- 0x0070 Thrusters TArray<FItemState>
- 0x0080 Devices TArray<FItemState>
- 0x0090 Consumables TArray<FItemState>
- 0x00A0 Cargo TArray<FItemState>
- All 11 arrays are reflected UPROPERTYs — this IS the client's loadout

### FItemState (size 464)
- 0x0000 ItemTemplateID FName — item identity (data-table row); the only cross-machine key needed
- 0x0008 Seed int / 0x000C NameSeed int — deterministic roll seeds
- 0x0010 ItemLevel int / 0x0014 VirtualLevelOffset float
- 0x0018 Rarity TEnumAsByte<EItemRarity::Type> / 0x0068 Grade TEnumAsByte<EItemGrade::Type>
- 0x001C Amount int / 0x0020 Ammo int
- 0x002C Condition float / 0x0030 CooldownRemaining float
- 0x0040 InstalledChipIDs TArray<FName> / 0x0050 Catalysts TArray<FName> / 0x0060 AffixID FName
- 0x0070 AttributeStates TArray<FItemAttributeState> — explicit rolled stats, so the receiving side does not need to re-roll
- 0x0080 AttributeOriginalPositionInRange TMap<FName,float> / 0x00D0 AttributeOriginalValue TMap<FName,float>
- 0x0128 CustomData TMap<FName,FESVariant> / 0x0178 CraftingActionCounts TMap<ECraftingType,int>
- 46 reflected properties total

### AESPawn (relevant slice) (size 0)
- 0x0338 CollisionRoot UMovementRootComponent*
- 0x0348 Savegame USaveGameComponent*
- 0x0350/0x0358/0x0360 Health/Armor/Shield components
- 0x0370 EnergyCore UEnergyCoreComponent*
- 0x0380 PrimaryWeapons UWeaponComponent* — call SpawnWeapons() here
- 0x0388 SecondaryWeapons UWeaponComponent* — and here
- 0x0390 Devices UDeviceComponent* / 0x0398 Consumables UConsumableComponent*
- 0x04B8 ShipData FShipData (976B) — **THE TARGET**; reflected UPROPERTY so its UObject pointers are GC-rooted by the pawn
- 0x04D0 (= ShipData+0x18) ShipItemInstance UItem* — UpdateShipModules early-outs if null
- 0x0830 (= ShipData+0x378) GenericSaveData — fed to ISavableInterface::StaticRestoreState
- 0x08B8 bGetShipModulesFromShipData bool — gates UpdateShipModules inside ApplyShipData
- 0x08BC ShipColors FShipColors
- 0x09D8 / 0x0A78 / 0x0AE4 — bGetShipColorsFromShipData / bGetDecalsFromShipData / bGetProjectionDecalFromShipData flags read by execApplyShipData
- No AESPawn::GetLifetimeReplicatedProps symbol exists => ShipData does NOT replicate

### UPlayerData (USaveGame subclass, relevant slice) (size 8624)
- 0x0468 Ships TArray<FShipData> — owned ships (data ptr +0x468, count +0x470, stride 0x3D0)
- 0x0478 ShipsSaveState TArray<FShipDataState> — the serializable mirror written by CreateSaveableState (stride 0x638)
- 0x0488 SavedPlayerShip FShipDataState (1592B) — the currently-flown ship's serializable snapshot
- 0x0120 Credits int / 0x0128 PlayerLevel int / 0x012C XP float
- 0x0130 StationCargo UInventory* / 0x0138 StationCargoSaveState FInventoryState
- 0x119C CurrentShip int — index into Ships
- 0x11A0 CurrentShipRotation FRotator / 0x11B8 CurrentShipLocation FVector
- 0x1508 CurrentDockingPoint FName / 0x1518 PreviousDockingPoint FName — read by SpawnDefaultPawnAtTransform to place the pawn
- 0x1850 UnlockedShipModules TMap<EShipClass,FShipModuleInfo>
- 0x1D88 AncientDeviceShipSaveState FShipSaveState
- Accessed process-wide as *(UPlayerData**)((*(char**)(image+0x09AC5E88)) + 0x1C0)

### FShipSaveState (size 88)
- 0x0000 ShieldRatio / 0x0004 ArmorRatio / 0x0008 HullRatio / 0x000C ULTRatio / 0x0010 BoostEnergy floats
- 0x0018 WeaponEnergyRatios TArray<float>
- 0x0028 Consumables TArray<FItemContainerContent>
- 0x0038 SecondaryAmmos TArray<int>
- 0x0048 DeviceCooldowns TArray<FFloatArrayHelper>
- This is VOLATILE COMBAT STATE, not the loadout — the SaveShipState/RestoreShipState pair works on this

### FShipModuleState / FShipModule / FShipAppearance / FShipColors / FModuleExtension / FPlayerDroneInfo (size 12)
- FShipModuleState (12B): 0x0000 ID FName, 0x0008 Type TEnumAsByte<EShipModule::Type> — pure value, transports fine inside FShipDataState
- FShipModule (168B, FTableRowBase): data-table row (ModuleName, TSoftObjectPtr Asset, Type, ShipsWhiteList TSet<EShip::Type>, Extensions TArray<FModuleExtension>) — STATIC CONTENT, identical on both machines, never transported
- FShipAppearance (400B): AppearanceID + FShipColors + DecalsID/ThrustParticlesID/BoostParticlesID + TArray<FShipModuleState> + FProjectionDecal — pure value
- FShipColors (284B): 12 FLinearColors + roughness/metal flags — pure value
- FModuleExtension (56B): ID FName + TSoftObjectPtr Asset + bExludeFromGame — static content
- FPlayerDroneInfo (8B): DroneType + HullRatio — lives in UPlayerData::PlayerDrones(+0x1960), NOT part of FShipData, so drones are a separate transport problem


## Hook plan

- **AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation** @ `05DEC54C` [host] — Give a joining client's pawn that client's real ship + equipment instead of the host's GetCurrentShip() copy.
  - behaviour: Detour, call the original trampoline first, then post-process. In the detour: (1) capture the AController* (RDX) before calling the original; (2) call the original — it returns an AESPawn* already carrying the HOST's FShipData and already FinishSpawning'd; (3) if the controller is a remote client we have a stored FShipDataState blob for, build a live ship: `alignas(16) uint8 stateBuf[1592]` -> deserialize into it -> `alignas(16) uint8 shipBuf[976]; UInventory::CreateShipDataFromState(shipBuf, stateBuf)` (RCX=shipBuf raw, RDX=stateBuf) -> `FShipData::operator=(pawn+0x4B8, shipBuf)` (RCX=pawn+0x4B8, RDX=shipBuf) -> `FShipData::~FShipData(shipBuf)` and `FShipDataState::~FShipDataState(stateBuf)`; (4) re-materialise: `AESPawn::UpdateShipModules(pawn, /*bRespawnWeapons=*/true)` (RVA 05DEF910, RCX=pawn, DL=1), then optionally `AESPawn::ApplyShipSkin(pawn)` (05DE03DC) and `AESPawn::ApplyShipDataShipDecals(pawn)` (05DE039C). Preferred refinement once the post-hoc path is proven: skip the original entirely for remote controllers and reimplement SpawnPlayerShip's deferred pattern (FActorSpawnParameters ctor 014DB4C8 -> set bDeferConstruction bit2 at +0x32 and Owner at +0x10 -> UWorld::SpawnActor 014DB504 with gamemode->DefaultPawnClass at +0x2E0 -> write ShipData at +0x4B8 -> AActor::FinishSpawning 01569FCC) so the loadout is present before the construction script and BeginPlay run.
  - risk: MEDIUM-HIGH. (a) The original writes the spawned pawn into AESGameModeBase::ESPlayerPawn(+0x508) — for a client join this overwrites the host's cached pawn pointer; snapshot and restore +0x508 around the call. (b) The original also calls UPlayerData::ReinitGlobalAttributesWithCurrentShip() (05E142BC) on the host's player data at entry — harmless but re-runs host attribute init per join. (c) Calling UpdateShipModules(true) destroys and respawns weapon actors after FinishSpawning; if this proves unstable, use the deferred reimplementation instead. (d) Function is huge (~0x2800B) but the prologue is a normal 5-push+lea sequence, safe for MinHook. (e) RVA 05DEC54C is unique (grep -c returned 1) and is AESGameModeBase's own override, distinct from AGameModeBase's at 04A4C2DC.
- **Client-side loadout capture (no hook — mod-driven call sequence)** @ `05D968B8` [client] — Produce the transportable blob describing 'my ship with my equipment'.
  - behaviour: On the client, after its own save is loaded and before/at `open <ip>`: get the live ship via `AESPawn* p = UGameplayLib::GetESPlayerPawn(worldCtx)` (013F0CC0) and use `FShipData* sd = (FShipData*)((uint8*)p + 0x4B8)`; if there is no pawn yet, fall back to `UInventoryLib::GetCurrentShip()` (0121A1E0, RCX = raw 976-byte buffer) and remember to destruct it. Then `alignas(16) uint8 stateBuf[1592]; FShipData::GetShipDataState(sd, stateBuf)` — ABI is RCX = FShipData* this, RDX = FShipDataState* retbuf (raw, it default-constructs). Serialize stateBuf and send over the existing string RPC channel together with the client's identity, then `FShipDataState::~FShipDataState(stateBuf)` (05AA10D8).
  - risk: LOW. Read-only on the client. Only caveat: the blob must be captured while the client still has its own UPlayerData/pawn — capture BEFORE issuing `open <ip>` and cache it, since travel destroys the pawn (UPlayerData itself survives on the game instance, so GetCurrentShip() remains valid as a fallback after travel).
- **FShipDataState blob (de)serialization** @ `01688C48` [both] — Turn the 1592-byte struct into bytes/text that survive the process boundary, and back.
  - behaviour: WRITE: `TArray<uint8> bytes; FMemoryWriter w(bytes, /*bIsPersistent=*/true, false, NAME_None)` (ctor 0225F014, RCX=168-byte stack buffer, RDX=&bytes, R8B=1, R9B=0, [rsp+0x20]=FName{0,0}); `FObjectAndNameAsStringProxyArchive ar(w, /*bLoadIfFindFails=*/true)` (ctor 01685A30, RCX=160-byte stack buffer, RDX=&w, R8B=1); `UScriptStruct::SerializeItem(FShipDataState::StaticStruct(), ar, stateBuf, nullptr)` (01688C48 — the FArchive overload, NOT the FStructuredArchiveSlot one at 0121943C; RCX=UScriptStruct*, RDX=&ar, R8=stateBuf, R9=nullptr). Base64 the bytes for the string channel. READ: `FMemoryReader r(bytes, true)` (015ADFC4), same proxy wrap, same SerializeItem call into a freshly default-constructed FShipDataState (05BC57C8) or an InitializeStruct'd (011FC20C) buffer. ALTERNATIVE, no base64: `UScriptStruct::ExportText` (02D01634) / `ImportText` (02D06F3C) straight to/from FString.
  - risk: MEDIUM. THE NAME-AS-STRING PROXY IS NOT OPTIONAL — every item identity in the blob is an FName, and FName indices are per-process; a bare FMemoryWriter would ship garbage item IDs. Archives have virtual destructors: tear them down through vtable slot 0 with the do-not-free flag, in reverse construction order. Both sides must be the exact same game build (identical UScriptStruct property layout); guard the payload with a build/version tag.
- **Client-side local application of its own ship to its proxy pawn** @ `05DEF910` [client] — Make the client SEE and feel its own ship, since AESPawn::ShipData does not replicate.
  - behaviour: After the client's pawn is assigned post-travel, observe AESPawn spawn/possession on the client, and for the pawn the client controls: write the client's own FShipData into pawn+0x4B8 (rebuild locally via UInventory::CreateShipDataFromState from the same blob, or simply reuse the client's own UPlayerData through UInventoryLib::GetCurrentShip()), then `AESPawn::UpdateShipModules(pawn, true)` (05DEF910) and `AESPawn::ApplyShipSkin` / `ApplyShipDataShipDecals`. Optionally call the reflected 0-param UFunction `AESPawn::ApplyShipData` via ProcessEvent for the colour/decal/projection path (it internally does UpdateShipModules(false), so it is NOT a substitute for the weapons call).
  - risk: LOW-MEDIUM. Purely cosmetic/local; worst case is a visual mismatch. Do not call UInventoryLib::ReinitShipAfterPotentialChanges or UGameplayLib::RefreshPlayerShipData here — they write back into the client's own UPlayerData and would mutate its save.
- **Guard rails — functions to explicitly NOT call on the host for a client pawn** @ `05E37DA4` [host] — Prevent corrupting the host's own save/ship while fixing up a client pawn.
  - behaviour: Add an assertion/allowlist in the mod: never invoke UGameplayLib::RefreshPlayerShipData (05E37DA4), UInventoryLib::ReinitShipAfterPotentialChanges (0178F6DC), UInventoryLib::ReplaceShipData (05E4E740), UInventoryLib::SetCurrentShip (05E5059C), UPlayerData::SetCurrentShip (05E16CD8), UGameplayLib::SaveShipState (05E3A564), UGameplayLib::RestoreShipState (05E3986C), UGameplayLib::ReloadLocationAfterShipChange (05E387C4) or UPlayerData::CreateSaveableState (05DFF7EC) as part of the client-join fixup. If UGameplayLib::RefreshPlayerShipData is observed being called by the engine while a client pawn is the local player-0 pawn, consider hooking it to no-op for non-host pawns.
  - risk: N/A (this is a defensive measure). Every one of these was verified by disassembly to route through UGameplayLib::GetPlayerData() (single process global at RVA 09AC5E88 -> +0x1C0) and/or UGameplayStatics::GetPlayerPawn(world, 0), i.e. the HOST's ship on a listen server.

## Open questions

- Does BP_Ship_Player_C's construction script / BeginPlay read ShipData in a way that makes the post-hoc (assign-after-FinishSpawning + UpdateShipModules(true)) path insufficient? The native code does not, but the Blueprint might. Needs a live test: join, inject, then read back UGameplayLib::GetPlayerShipType(pawn) (01804388) and enumerate the spawned AWeaponBase actors under the pawn.
- What is the runtime value of AESPawn::bGetShipModulesFromShipData (+0x8B8) on BP_Ship_Player_C? It gates the module path inside execApplyShipData. If false on the default object, ApplyShipData is a no-op for modules and only the direct UpdateShipModules(true) call will work.
- AESGameModeBase::ESPlayerPawn (+0x508) is unconditionally overwritten by SpawnDefaultPawnAtTransform_Implementation with whatever pawn it just spawned, including a client's. Does anything on the host depend on that pointer still being the host's own pawn? Needs a live check (and probably a save/restore around the trampoline call).
- UInventory::CreateInventoryFromState (05E9858C) consults UGameplayLib::GetPlayerData() (the HOST's) plus a bool at gameinstance+0xBF6 while rebuilding items. It is not obvious from the disassembly whether that affects item level scaling / item replacement for the reconstructed client ship. Compare the client's original FItemState levels against the host-side reconstructed UItem levels after a join.
- Does the joining client still have a valid UPlayerData with a populated Ships array after UEngine::Browse / `open <ip>`? GetPlayerData() reads the game-instance global which normally survives travel, but this must be confirmed live before relying on the GetCurrentShip() fallback.
- What outer does NewObject<UInventory> use (global at RVA 09D8BEB8) and does the resulting UInventory get GC'd if the pawn dies and respawns? FShipData is a UPROPERTY on AESPawn so it is rooted while the pawn lives, but respawn/death flows for a client pawn are untested.
- Drones (UPlayerData::PlayerDrones, TArray<FPlayerDroneInfo> at +0x1960), perks (SelectedPerks +0x12F8), companion perks, device mastery levels and global attribute augments are NOT part of FShipData/FShipDataState. If clients should also get their own perks/drones, a second transport channel and a second injection point are needed (UPlayerData::ReinitGlobalAttributesWithShip 05E142CC takes an explicit TMap& output and may be usable per-pawn — worth investigating).
- FShipDataState carries GenericSaveData (FGenericSaveDataMapV2). The host calls ISavableInterface::StaticRestoreState (05E3F5B0) with a 16-byte stack struct {ptr = pawn+0x830, int32 0x20A, int32 = *(pawn+0x880)} — the meaning of the 0x20A version constant and whether a client-authored GenericSaveData blob is safe to restore on the host is unverified.
- Transport size: a full FShipDataState with ~10 populated FItemStates is likely tens of KB after base64. Whether the existing string RPC channel can carry that in one message, or needs chunking, is unknown from static analysis.
