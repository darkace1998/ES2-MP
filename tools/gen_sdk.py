#!/usr/bin/env python3
"""Generate sdk/gen/rvas.h (function/global RVAs) and sdk/gen/offsets.h (class member offsets)
from the PDB-derived tables.  Edit the SPEC dicts below to add symbols.

Usage: gen_sdk.py <symbols.tsv> <types.txt> <outdir>
"""
import sys, os, json, subprocess, re

# identifier -> exact demangled signature (as in symbols.tsv column 4).  Use 'regex:' prefix for a regex
# that must match exactly one symbol.
RVAS = {
    # ---- globals ----
    'GEngine':            'class UEngine *GEngine',
    'GWorld':             'class UWorldProxy GWorld',
    'GUObjectArray':      'class FUObjectArray GUObjectArray',
    'GMalloc':            'class FMalloc *GMalloc',
    'GFrameCounter':      'unsigned __int64 GFrameCounter',
    'GIsRunning':         'bool GIsRunning',
    # ---- core object / name / memory ----
    'UObject_ProcessEvent':     'public: virtual void __cdecl UObject::ProcessEvent(class UFunction *, void *)',
    'FName_ToString':           'public: void __cdecl FName::ToString(class FString &) const',
    'FName_Ctor_Wide':          'public: __cdecl FName::FName(wchar_t const *, enum EFindName)',
    'FMemory_Free':             'public: static void __cdecl FMemory::Free(void *)',
    'FMemory_Malloc':           'public: static void * __cdecl FMemory::Malloc(unsigned __int64, unsigned int)',
    'FMemory_Realloc':          'public: static void * __cdecl FMemory::Realloc(void *, unsigned __int64, unsigned int)',
    'StaticFindObject':         'class UObject * __cdecl StaticFindObject(class UClass *, class UObject *, wchar_t const *, bool)',
    'StaticLoadObject':         'class UObject * __cdecl StaticLoadObject(class UClass *, class UObject *, wchar_t const *, wchar_t const *, unsigned int, class UPackageMap *, bool, class FLinkerInstancingContext const *)',
    'UObjectBaseUtility_GetPathName': 'public: void __cdecl UObjectBaseUtility::GetPathName(class UObject const *, class FString &) const',
    'UStruct_IsChildOf':        'regex:^public: bool __cdecl UStruct::IsChildOf\\(class UStruct const \\*\\) const$',
    'UClass_FindFunctionByName':'regex:^public: class UFunction \\* __cdecl UClass::FindFunctionByName\\(class FName, enum EIncludeSuperFlag::Type\\) const$',
    'UObject_FindFunctionChecked': 'public: class UFunction * __cdecl UObject::FindFunctionChecked(class FName) const',
    'UStruct_FindPropertyByName':'regex:^public: class FProperty \\* __cdecl UStruct::FindPropertyByName\\(class FName\\) const$',
    # ---- engine / world ----
    'UGameEngine_Tick':         'public: virtual void __cdecl UGameEngine::Tick(float, bool)',
    'UEngine_Exec':             'public: virtual bool __cdecl UEngine::Exec(class UWorld *, wchar_t const *, class FOutputDevice &)',
    'UEngine_Browse':           'public: virtual enum EBrowseReturnVal::Type __cdecl UEngine::Browse(struct FWorldContext &, struct FURL, class FString &)',
    'UEngine_GetWorldContextFromWorld': 'public: struct FWorldContext * __cdecl UEngine::GetWorldContextFromWorld(class UWorld const *)',
    'UEngine_GetFirstLocalPlayerController': 'public: class APlayerController * __cdecl UEngine::GetFirstLocalPlayerController(class UWorld const *)',
    'UKismetSystemLibrary_ExecuteConsoleCommand': 'public: static void __cdecl UKismetSystemLibrary::ExecuteConsoleCommand(class UObject const *, class FString const &, class APlayerController *)',
    'APlayerController_ConsoleCommand': 'public: virtual class FString __cdecl APlayerController::ConsoleCommand(class FString const &, bool)',
    'UWorld_SpawnActor':        'public: class AActor * __cdecl UWorld::SpawnActor(class UClass *, struct UE::Math::TVector<double> const *, struct UE::Math::TRotator<double> const *, struct FActorSpawnParameters const &)',
    # THE spawn funnel: both the vector/rotator overload and SpawnActorAbsolute forward into this one.
    'UWorld_SpawnActor_Transform': 'public: class AActor * __cdecl UWorld::SpawnActor(class UClass *, struct UE::Math::TTransform<double> const *, struct FActorSpawnParameters const &)',
    'UWorld_SpawnActorAbsolute': 'public: class AActor * __cdecl UWorld::SpawnActorAbsolute(class UClass *, struct UE::Math::TTransform<double> const &, struct FActorSpawnParameters const &)',
    'UWorld_Listen':            'public: bool __cdecl UWorld::Listen(struct FURL &)',
    'UWorld_ServerTravel':      'public: bool __cdecl UWorld::ServerTravel(class FString const &, bool, bool)',
    'UWorld_SetGameMode':       'public: bool __cdecl UWorld::SetGameMode(struct FURL const &)',
    'UWorld_GetFirstPlayerController': 'public: class APlayerController * __cdecl UWorld::GetFirstPlayerController(void) const',
    'UGameInstance_EnableListenServer': 'public: virtual bool __cdecl UGameInstance::EnableListenServer(bool, int)',
    'UGameInstance_CreateGameModeForURL': 'public: virtual class AGameModeBase * __cdecl UGameInstance::CreateGameModeForURL(struct FURL, class UWorld *)',
    'FURL_Ctor':                'public: __cdecl FURL::FURL(struct FURL *, wchar_t const *, enum ETravelType)',
    'UGameplayStatics_GetAllActorsOfClass': 'public: static void __cdecl UGameplayStatics::GetAllActorsOfClass(class UObject const *, class TSubclassOf<class AActor>, class TArray<class AActor *, class TSizedDefaultAllocator<32>> &)',
    'UGameplayStatics_GetGameMode': 'public: static class AGameModeBase * __cdecl UGameplayStatics::GetGameMode(class UObject const *)',
    'UGameplayStatics_GetPlayerController': 'public: static class APlayerController * __cdecl UGameplayStatics::GetPlayerController(class UObject const *, int)',
    'UGameplayStatics_OpenLevel': 'public: static void __cdecl UGameplayStatics::OpenLevel(class UObject const *, class FName, bool, class FString)',
    'AActor_SetReplicates':     'public: void __cdecl AActor::SetReplicates(bool)',
    'AActor_SetReplicateMovement': 'public: virtual void __cdecl AActor::SetReplicateMovement(bool)',
    'AActor_SetActorLocationAndRotation': 'regex:^public: bool __cdecl AActor::SetActorLocationAndRotation\\(struct UE::Math::TVector<double>, struct UE::Math::TRotator<double>, bool, struct FHitResult \\*, enum ETeleportType\\)$',
    'AActor_GetTransform':      'public: struct UE::Math::TTransform<double> const & __cdecl AActor::GetTransform(void) const',
    'AActor_SetActorTransform': 'public: bool __cdecl AActor::SetActorTransform(struct UE::Math::TTransform<double> const &, bool, struct FHitResult *, enum ETeleportType)',
    'UWorld_InternalGetNetMode': 'private: enum ENetMode __cdecl UWorld::InternalGetNetMode(void) const',
    'UEngine_GetNetMode':       'public: enum ENetMode __cdecl UEngine::GetNetMode(class UWorld const *) const',
    'UNetDriver_IsServer':      'public: virtual bool __cdecl UNetDriver::IsServer(void) const',
    'UNetConnection_LowLevelGetRemoteAddress': 'public: virtual class FString __cdecl UNetConnection::LowLevelGetRemoteAddress(bool)',
    'UNetConnection_LowLevelDescribe': 'public: virtual class FString __cdecl UNetConnection::LowLevelDescribe(void)',
    'APlayerController_ClientMessage': 'public: void __cdecl APlayerController::ClientMessage(class FString const &, class FName, float)',
    'AGameModeBase_PostLogin':  'public: virtual void __cdecl AGameModeBase::PostLogin(class APlayerController *)',
    'AGameModeBase_RestartPlayer': 'public: virtual void __cdecl AGameModeBase::RestartPlayer(class AController *)',
    'AGameModeBase_SpawnDefaultPawnAtTransform_Implementation': 'public: virtual class APawn * __cdecl AGameModeBase::SpawnDefaultPawnAtTransform_Implementation(class AController *, struct UE::Math::TTransform<double> const &)',
    'AESGameModeBase_SpawnDefaultPawnAtTransform_Implementation': 'public: virtual class APawn * __cdecl AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation(class AController *, struct UE::Math::TTransform<double> const &)',
    'APlayerController_ServerChangeName': 'public: void __cdecl APlayerController::ServerChangeName(class FString const &)',
    'APlayerController_ServerChangeName_Implementation': 'public: virtual void __cdecl APlayerController::ServerChangeName_Implementation(class FString const &)',
    'APlayerController_ClientTeamMessage_Implementation': 'public: virtual void __cdecl APlayerController::ClientTeamMessage_Implementation(class APlayerState *, class FString const &, class FName, float)',
    'APlayerController_ServerSetSpectatorLocation_Implementation': 'regex:^public: virtual void __cdecl APlayerController::ServerSetSpectatorLocation_Implementation\\(struct UE::Math::TVector<double>, struct UE::Math::TRotator<double>\\)$',
    'APlayerController_ClientMessage_Implementation': 'public: virtual void __cdecl APlayerController::ClientMessage_Implementation(class FString const &, class FName, float)',
    'APlayerController_ClientTravel': 'public: void __cdecl APlayerController::ClientTravel(class FString const &, enum ETravelType, bool, struct FGuid)',
    'UIpNetDriver_InitListen': 'public: virtual bool __cdecl UIpNetDriver::InitListen(class FNetworkNotify *, struct FURL &, bool, class FString &)',
    'UIpNetDriver_InitBase': 'public: virtual bool __cdecl UIpNetDriver::InitBase(bool, class FNetworkNotify *, struct FURL const &, bool, class FString &)',
    'UIpNetDriver_InitConnect': 'public: virtual bool __cdecl UIpNetDriver::InitConnect(class FNetworkNotify *, struct FURL const &, class FString &)',
    'UNetDriver_InitBase': 'public: virtual bool __cdecl UNetDriver::InitBase(bool, class FNetworkNotify *, struct FURL const &, bool, class FString &)',
    'UNetDriver_InitConnectionClass': 'public: virtual bool __cdecl UNetDriver::InitConnectionClass(void)',
    'UIpNetDriver_GetSocketSubsystem': 'public: virtual class ISocketSubsystem * __cdecl UIpNetDriver::GetSocketSubsystem(void)',
    'ISocketSubsystem_Get': 'public: static class ISocketSubsystem * __cdecl ISocketSubsystem::Get(class FName const &)',
    'CreateNetDriver_Local': 'class UNetDriver * __cdecl UE::Private::CreateNetDriver_Local(class UEngine *, struct FWorldContext &, class FName, class FName)',
    'UEngine_CreateNamedNetDriver_World': 'public: bool __cdecl UEngine::CreateNamedNetDriver(class UWorld *, class FName, class FName)',
    'UClass_SetUpRuntimeReplicationData': 'public: void __cdecl UClass::SetUpRuntimeReplicationData(void)',
    'UPrimitiveComponent_SetPhysicsLinearVelocity': 'public: virtual void __cdecl UPrimitiveComponent::SetPhysicsLinearVelocity(struct UE::Math::TVector<double>, bool, class FName)',
    'UPrimitiveComponent_SetPhysicsAngularVelocityInDegrees': 'public: void __cdecl UPrimitiveComponent::SetPhysicsAngularVelocityInDegrees(struct UE::Math::TVector<double>, bool, class FName)',
    'UPrimitiveComponent_GetPhysicsLinearVelocity': 'public: struct UE::Math::TVector<double> __cdecl UPrimitiveComponent::GetPhysicsLinearVelocity(class FName)',
    'UPrimitiveComponent_IsSimulatingPhysics': 'public: virtual bool __cdecl UPrimitiveComponent::IsSimulatingPhysics(class FName) const',
    'AActor_GetVelocity': 'public: virtual struct UE::Math::TVector<double> __cdecl AActor::GetVelocity(void) const',
    'AActor_TeleportTo': 'public: virtual bool __cdecl AActor::TeleportTo(struct UE::Math::TVector<double> const &, struct UE::Math::TRotator<double> const &, bool, bool)',
    'UEngine_GetGameUserSettings': 'public: class UGameUserSettings * __cdecl UEngine::GetGameUserSettings(void)',
    # ---- NPC fire mirroring ----
    # Native input handlers (not UFunctions, so unreachable from the console `call`) — used by the
    # `input` test command to drive weapon swap / travel / cruise exactly like a real key press.
    'AESPlayerController_InputNextPrimaryWeapon': 'private: void __cdecl AESPlayerController::InputNextPrimaryWeapon(void)',
    'AESPlayerController_InputPreviousPrimaryWeapon': 'private: void __cdecl AESPlayerController::InputPreviousPrimaryWeapon(void)',
    'AESPlayerController_InputNextSecondaryWeapon': 'private: void __cdecl AESPlayerController::InputNextSecondaryWeapon(void)',
    'AESPlayerController_InputChargeTravelMode': 'private: void __cdecl AESPlayerController::InputChargeTravelMode(void)',
    'AESPlayerController_InputChargeTravelModeReleased': 'private: void __cdecl AESPlayerController::InputChargeTravelModeReleased(void)',
    'AESPlayerController_InputChargeCruiseMode': 'private: void __cdecl AESPlayerController::InputChargeCruiseMode(void)',
    'AESPlayerController_InputChargeCruiseModeReleased': 'private: void __cdecl AESPlayerController::InputChargeCruiseModeReleased(void)',
    # Main-menu multiplayer UI: create an ES2 menu button, label it, and splice it into the menu's
    # own VerticalBox so it looks and behaves like a stock entry.
    'UWidgetBlueprintLibrary_Create': 'public: static class UUserWidget * __cdecl UWidgetBlueprintLibrary::Create(class UObject *, class TSubclassOf<class UUserWidget>, class APlayerController *)',
    'UPanelWidget_RemoveChild': 'public: bool __cdecl UPanelWidget::RemoveChild(class UWidget *)',
    'UPanelWidget_AddChild': 'public: class UPanelSlot * __cdecl UPanelWidget::AddChild(class UWidget *, class UPanelSlot *)',
    'FText_FromString': 'public: static class FText __cdecl FText::FromString(class FString const &)',
    'UGameplayLib_ApplyESRadialDamage': 'public: static bool __cdecl UGameplayLib::ApplyESRadialDamage(class UObject const *, struct FDamageInfo, struct UE::Math::TVector<double> const &, float, class TArray<class AActor *, class TSizedDefaultAllocator<32>> const &, bool &, struct FWeaponData, class TArray<class AActor *, class TSizedDefaultAllocator<32>> &, class TArray<class AActor *, class TSizedDefaultAllocator<32>> &, class TArray<struct FHitResult, class TSizedDefaultAllocator<32>> &, float, float, class AActor *, class AController *, bool, bool, bool, bool)',
    'UGameplayLib_DamageDealtByPlayerOrPlayerFriend': 'public: static void __cdecl UGameplayLib::DamageDealtByPlayerOrPlayerFriend(class UHitpointComponent const *, float, class AController *, class AActor *, class AActor *, struct FHitResult const &, bool, bool)',
    'UGameplayLib_ApplyESPointDamage': 'public: static struct FDamageInfo __cdecl UGameplayLib::ApplyESPointDamage(class AActor *, struct FDamageInfo, struct UE::Math::TVector<double> const &, struct FHitResult const &, class AController *, class AActor *, bool &, struct FWeaponData, float, float, bool)',
    'FNetGUIDCache_GetNetGUID': 'public: class FNetworkGUID __cdecl FNetGUIDCache::GetNetGUID(class UObject const *) const',
    'UPackageMapClient_GetNetGUIDFromObject': 'regex:^public: virtual class FNetworkGUID __cdecl UPackageMapClient::GetNetGUIDFromObject\\(class UObject const \\*\\) const$',
    'FNetGUIDCache_GetObjectFromNetGUID': 'public: class UObject * __cdecl FNetGUIDCache::GetObjectFromNetGUID(class FNetworkGUID const &, bool)',
    'UWeaponComponent_GetLockedTarget': 'public: class AActor * __cdecl UWeaponComponent::GetLockedTarget(void) const',
    'UWeaponComponent_SetLockedTarget': 'public: void __cdecl UWeaponComponent::SetLockedTarget(class AActor *)',
    'UShieldComponent_TickRegeneration': 'public: void __cdecl UShieldComponent::TickRegeneration(class AActor *, class UEnergyCoreComponent *, float, float, float)',
    'UWeaponComponent_TickComponent': 'public: virtual void __cdecl UWeaponComponent::TickComponent(float, enum ELevelTick, struct FActorComponentTickFunction *)',
    'UWeaponComponent_StartFire': 'public: void __cdecl UWeaponComponent::StartFire(void)',
    'UWeaponComponent_StopFire': 'public: void __cdecl UWeaponComponent::StopFire(void)',
    # ---- client-local ship materialisation ----
    'AESPawn_PostInitializeComponents': 'public: virtual void __cdecl AESPawn::PostInitializeComponents(void)',
    'AESPawn_PreInitializeComponents': 'public: virtual void __cdecl AESPawn::PreInitializeComponents(void)',
    'AESPawn_BeginPlay': 'protected: virtual void __cdecl AESPawn::BeginPlay(void)',
    'AESPawn_UpdateShipModules': 'public: void __cdecl AESPawn::UpdateShipModules(bool)',
    'FShipData_Assign': 'public: struct FShipData & __cdecl FShipData::operator=(struct FShipData const &)',
    'UInventoryLib_ReinitShipAfterPotentialChanges': 'public: static void __cdecl UInventoryLib::ReinitShipAfterPotentialChanges(void)',
    'FWeaponInfo_Ctor': 'regex:^public: __cdecl FWeaponInfo::FWeaponInfo\\(void\\)$',
    'UWeaponComponent_CreateWeaponInfoFromItem': 'public: static void __cdecl UWeaponComponent::CreateWeaponInfoFromItem(class UItem *, struct FWeaponInfo &)',
    'UWeaponComponent_EquipWeapon': 'public: void __cdecl UWeaponComponent::EquipWeapon(int, bool, bool, bool)',
    'UWeaponComponent_SpawnWeapons': 'public: void __cdecl UWeaponComponent::SpawnWeapons(void)',
    # ---- steam p2p ----
    'FOnlineFactorySteam_SteamSingleton': 'private: static class TSharedPtr<class FOnlineSubsystemSteam, 1> FOnlineFactorySteam::SteamSingleton',
    'FSocketSubsystemSteam_SocketSingleton': 'protected: static class FSocketSubsystemSteam *FSocketSubsystemSteam::SocketSingleton',
    'FOnlineSessionSteam_GetNumSessions': 'public: virtual int __cdecl FOnlineSessionSteam::GetNumSessions(void)',
    'FOnlineIdentitySteam_GetUniquePlayerId': 'public: virtual class TSharedPtr<class FUniqueNetId const, 1> __cdecl FOnlineIdentitySteam::GetUniquePlayerId(int) const',
    # ---- death / respawn ----
    'AController_Possess': 'public: virtual void __cdecl AController::Possess(class APawn *)',
    'UHealthComponent_SetCurrentHitpointsWithRatio': 'public: virtual void __cdecl UHealthComponent::SetCurrentHitpointsWithRatio(float)',
    'UUserFunctionsLib_LoadGame': 'public: static void __cdecl UUserFunctionsLib::LoadGame(class UObject const *, class FString, int)',
    'AESPlayerController_ReturnToMainMenu': 'public: void __cdecl AESPlayerController::ReturnToMainMenu(void)',
    'ChangeTrackedMission_Internal': 'void __cdecl ChangeTrackedMission_Internal(class FName, bool)',
    'UPlayerData_OnMissionCompleted': 'public: void __cdecl UPlayerData::OnMissionCompleted(class AMissionBase *)',
    # ---- loot ----
    'UGameplayLib_SpawnPickupFromItem': 'public: static class APickupBase * __cdecl UGameplayLib::SpawnPickupFromItem(class UObject *, class UItem *, struct UE::Math::TVector<double> const &, struct UE::Math::TRotator<double> const &, bool &)',
    'UGameplayLib_SpawnPickupFromItemID': 'public: static class APickupBase * __cdecl UGameplayLib::SpawnPickupFromItemID(class UObject *, class FName, struct UE::Math::TVector<double> const &, int, struct UE::Math::TRotator<double> const &, bool &)',
    'UGameplayLib_SpawnPickups': 'public: static bool __cdecl UGameplayLib::SpawnPickups(class UObject *, class TArray<struct FPickupEntry, class TSizedDefaultAllocator<32>> const &, struct UE::Math::TVector<double> const &, struct UE::Math::TRotator<double> const &, class TArray<class APickupBase *, class TSizedDefaultAllocator<32>> &)',
    'UInventory_GetFirstCargoItem': 'public: class UItem * __cdecl UInventory::GetFirstCargoItem(void)',
    # ---- missions / dialog / xp ----
    'UMissionLib_UpdateTaskInPlayerData': 'public: static void __cdecl UMissionLib::UpdateTaskInPlayerData(class AMissionTaskBase *, bool, bool)',
    'UMissionLib_FindTaskInPlayerData': 'public: static struct FTaskSaveGameData * __cdecl UMissionLib::FindTaskInPlayerData(class FName)',
    'UMapLib_RefreshMissionAndWaypointIndicators': 'public: static void __cdecl UMapLib::RefreshMissionAndWaypointIndicators(void)',
    'UDialogManager_EnqueueDialog_Member': 'private: bool __cdecl UDialogManager::EnqueueDialog(class FName, class FDialogFinishedDelegate const *, float, enum EDialog::Type, enum EDialogBehavior::Type, bool)',
    'UDialogManager_EnqueueDialog_Static': 'public: static void __cdecl UDialogManager::EnqueueDialog(bool &, class FName, float, enum EDialog::Type, enum EDialogBehavior::Type, bool)',
    'UDialogManager_GetSingleton_Bool': 'private: static class UDialogManager & __cdecl UDialogManager::GetSingleton(bool)',
    'UGameplayLib_AddXP': 'public: static bool __cdecl UGameplayLib::AddXP(float, bool, bool, float)',
    'UGameplayLib_ChangeCredits': 'public: static void __cdecl UGameplayLib::ChangeCredits(int, enum ECreditsTransferType::Type, bool, bool)',
    'UXPComponent_OwnerHealthDepleted': 'public: void __cdecl UXPComponent::OwnerHealthDepleted(class AActor *, class AActor *, class AController *)',
    # ---- game ----
    'UESGameInstance_SetWorldOriginShifting': 'public: void __cdecl UESGameInstance::SetWorldOriginShifting(bool)',
    'UMapLib_GetCurrentLocationData': 'public: static struct FLocationData const & __cdecl UMapLib::GetCurrentLocationData(class UObject const *)',
    'UMapLib_GetLocationData': 'public: static struct FLocationData const & __cdecl UMapLib::GetLocationData(class FName const &)',
    'UGameplayLib_ChangeLocation': 'public: static void __cdecl UGameplayLib::ChangeLocation(class UObject *, struct FLocationData const &, struct FLocationData const &, bool, bool)',
    'UGameData_GetSingleton': 'public: static class UGameData & __cdecl UGameData::GetSingleton(void)',
    'UGameplayLib_ESOpenLevel': 'public: static void __cdecl UGameplayLib::ESOpenLevel(class UObject const *, class FName, bool, class FString)',
    'UESGameInstance_PushPause': 'public: static void __cdecl UESGameInstance::PushPause(void)',
    'UESGameInstance_PopPause': 'public: static void __cdecl UESGameInstance::PopPause(void)',
    'AESPawn_PostInitializeComponents': 'public: virtual void __cdecl AESPawn::PostInitializeComponents(void)',
    'UGameplayLib_GetESPlayerPawn': 'public: static class AESPawn * __cdecl UGameplayLib::GetESPlayerPawn(class UObject const *)',
    'UGameplayLib_GetESPlayerController': 'public: static class AESPlayerController * __cdecl UGameplayLib::GetESPlayerController(class UObject const *)',
    'UGameplayLib_GetPlayerData': 'public: static class UPlayerData * __cdecl UGameplayLib::GetPlayerData(void)',
    'UGameplayLib_SpawnPlayerShip': 'public: static class APawn * __cdecl UGameplayLib::SpawnPlayerShip(class UObject const *, int, struct UE::Math::TTransform<double> const &, bool)',
    'UGameplayLib_SpawnNPCPawnWithParams_Native': 'public: static class AESPawn * __cdecl UGameplayLib::SpawnNPCPawnWithParams_Native(class UObject const *, class TSubclassOf<class AActor>, struct UE::Math::TTransform<double> const &, struct FSpawnParameter const &)',
    'AESPlayerController_InputStartFirePrimary': 'private: void __cdecl AESPlayerController::InputStartFirePrimary(void)',
    'AESPlayerController_InputStopFirePrimary': 'private: void __cdecl AESPlayerController::InputStopFirePrimary(void)',
    'AESPlayerController_InputStartFireSecondary': 'private: void __cdecl AESPlayerController::InputStartFireSecondary(void)',
    'AESPlayerController_InputStopFireSecondary': 'private: void __cdecl AESPlayerController::InputStopFireSecondary(void)',
    'AESPawn_GetLockedTarget': 'regex:^public: class AActor \\* __cdecl AESPawn::GetLockedTarget\\(void\\)( const)?$',
    'AESPawn_LockClosestTarget': 'public: void __cdecl AESPawn::LockClosestTarget(void)',
    'AESPawn_StartFire': 'public: void __cdecl AESPawn::StartFire(bool)',
    'AESPawn_AreControlsDisabled': 'public: bool __cdecl AESPawn::AreControlsDisabled(void)',
    'UWeaponComponent_SetAllowFire': 'public: void __cdecl UWeaponComponent::SetAllowFire(bool)',
    # ---- ship loadout ----
    'UInventoryLib_GetCurrentShip': 'public: static struct FShipData __cdecl UInventoryLib::GetCurrentShip(void)',
    'UInventoryLib_GetPlayerShip': 'public: static struct FShipData __cdecl UInventoryLib::GetPlayerShip(int)',
    'FShipData_GetShipDataState': 'public: struct FShipDataState __cdecl FShipData::GetShipDataState(void)',
    'FShipData_StaticStruct': 'public: static class UScriptStruct * __cdecl FShipData::StaticStruct(void)',
    'FShipDataState_StaticStruct': 'public: static class UScriptStruct * __cdecl FShipDataState::StaticStruct(void)',
    'UInventory_CreateShipDataFromState': 'public: static struct FShipData __cdecl UInventory::CreateShipDataFromState(struct FShipDataState &)',
    'UScriptStruct_InitializeStruct': 'public: virtual void __cdecl UScriptStruct::InitializeStruct(void *, int) const',
    'UScriptStruct_DestroyStruct': 'public: virtual void __cdecl UScriptStruct::DestroyStruct(void *, int) const',
    'UScriptStruct_ExportText': 'public: void __cdecl UScriptStruct::ExportText(class FString &, void const *, void const *, class UObject *, int, class UObject *, bool) const',
    'UScriptStruct_ImportText': 'public: wchar_t const * __cdecl UScriptStruct::ImportText(wchar_t const *, void *, class UObject *, int, class FOutputDevice *, class FString const &, bool) const',
    'UGameplayLib_GetPlayerData': 'public: static class UPlayerData * __cdecl UGameplayLib::GetPlayerData(void)',
    'UGameplayLib_RefreshPlayerShipData': 'public: static void __cdecl UGameplayLib::RefreshPlayerShipData(void)',
    'AController_UnPossess': 'public: virtual void __cdecl AController::UnPossess(void)',
    'AController_Possess': 'public: virtual void __cdecl AController::Possess(class APawn *)',
    'AESHUD_Tick': 'public: virtual void __cdecl AESHUD::Tick(float)',
    'AESGameModeBase_GetESGameMode': 'regex:^public: static class AESGameModeBase \\* __cdecl AESGameModeBase::GetESGameMode\\(.*$',
}

# class -> members for offsets.h
OFFSETS = {
    'UObjectBase': ['ObjectFlags', 'InternalIndex', 'ClassPrivate', 'NamePrivate', 'OuterPrivate'],
    'FUObjectArray': ['ObjObjects'],
    'FChunkedFixedUObjectArray': ['Objects', 'PreAllocatedObjects', 'MaxElements', 'NumElements', 'MaxChunks', 'NumChunks'],
    'FUObjectItem': ['Object', 'Flags', 'ClusterRootIndex', 'SerialNumber'],
    'UStruct': ['SuperStruct', 'Children', 'ChildProperties', 'PropertiesSize', 'PropertyLink'],
    'UClass': ['ClassFlags', 'ClassCastFlags', 'ClassDefaultObject', 'ClassReps', 'NetFields', 'FuncMap', 'FirstOwnedClassRep'],
    'UFunction': ['FunctionFlags', 'NumParms', 'ParmsSize', 'ReturnValueOffset', 'RPCId', 'RPCResponseId', 'FirstPropertyToInit', 'Func'],
    'FField': ['ClassPrivate', 'Owner', 'Next', 'NamePrivate', 'FlagsPrivate'],
    'FFieldClass': ['Name', 'Id', 'CastFlags', 'ClassFlags', 'SuperClass'],
    'FProperty': ['ArrayDim', 'ElementSize', 'PropertyFlags', 'RepIndex', 'Offset_Internal', 'PropertyLinkNext', 'NextRef', 'DestructorLinkNext', 'PostConstructLinkNext'],
    'FBoolProperty': ['FieldSize', 'ByteOffset', 'ByteMask', 'FieldMask'],
    'FObjectPropertyBase': ['PropertyClass'],
    'FStructProperty': ['Struct'],
    'FArrayProperty': ['Inner'],
    'FByteProperty': ['Enum'],
    'FEnumProperty': ['UnderlyingProp', 'Enum'],
    'FClassProperty': ['MetaClass'],
    'UEnum': ['CppType', 'Names', 'CppForm', 'EnumFlags'],
    # Main-menu injection: reorder the button VerticalBox by hand (UPanelWidget has no InsertChildAt).
    'UPanelWidget': ['Slots'],
    'UPanelSlot': ['Parent', 'Content'],
    'UVerticalBoxSlot': ['Size', 'Padding', 'HorizontalAlignment', 'VerticalAlignment'],
    'UEngine': ['GameViewport', 'NetDriverDefinitions'],
    'FNetDriverDefinition': ['DefName', 'DriverClassName', 'DriverClassNameFallback', 'MaxChannelsOverride'],
    'UField': ['Next'],
    'UGameEngine': ['GameInstance'],
    'UWorldProxy': ['World'],
    'UWorld': ['PersistentLevel', 'NetDriver', 'DemoNetDriver', 'AuthorityGameMode', 'GameState', 'OwningGameInstance', 'Levels', 'ActiveLevelCollectionIndex', 'URL', 'bIsWorldInitialized', 'OriginLocation', 'bBegunPlay'],
    'ULevel': ['Actors', 'OwningWorld'],
    'UGameInstance': ['LocalPlayers', 'OnlineSession', 'WorldContext'],
    'FWorldContext': ['WorldType', 'ContextHandle', 'TravelURL', 'TravelType', 'PendingNetGame', 'ThisCurrentWorld', 'GameViewport', 'OwningGameInstance', 'LastURL', 'LastRemoteURL'],
    'AActor': ['Owner', 'RemoteRole', 'Role', 'NetDriverName', 'ReplicatedMovement', 'NetUpdateFrequency', 'MinNetUpdateFrequency', 'NetPriority', 'RootComponent', 'Instigator', 'Children', 'OwnedComponents', 'InitialLifeSpan', 'NetCullDistanceSquared', 'NetTag', 'NetDormancy', 'bActorInitialized', 'bActorIsBeingDestroyed', 'ActorHasBegunPlay'],
    'APawn': ['Controller', 'PlayerState', 'LastHitBy', 'AIControllerClass'],
    'AController': ['Pawn', 'PlayerState', 'Character', 'ControlRotation'],
    'APlayerController': ['Player', 'AcknowledgedPawn', 'PlayerCameraManager', 'MyHUD', 'NetConnection', 'PendingSwapConnection', 'PlayerInput', 'CheatManager', 'CheatClass'],
    'APlayerState': ['PlayerId', 'PlayerNamePrivate', 'bIsABot', 'bOnlySpectator', 'UniqueId'],
    'AGameSession': ['MaxSpectators', 'MaxPlayers', 'MaxPartySize'],
    'AGameModeBase': ['GameSessionClass', 'GameStateClass', 'PlayerControllerClass', 'PlayerStateClass', 'HUDClass', 'DefaultPawnClass', 'SpectatorClass', 'ReplaySpectatorPlayerControllerClass', 'ServerStatReplicatorClass', 'GameSession', 'GameState', 'OptionsString', 'bUseSeamlessTravel', 'bStartPlayersAsSpectators', 'bPauseable'],
    'UActorComponent_XX': ['OwnerPrivate'],
    'UNetDriver': ['GuidCache', 'NetDriverName', 'ClientConnections', 'ServerConnection', 'World', 'NetConnectionClass', 'MaxInternetClientRate', 'MaxClientRate', 'ServerTravelPause', 'ElapsedTime', 'NetServerMaxTickRate'],
    'UNetConnection': ['Children', 'Driver', 'PackageMap', 'OpenChannels', 'SentTemporaries', 'ViewTarget', 'OwningActor', 'MaxPacket', 'PlayerController', 'URL', 'LastReceiveTime', 'ClientLoginState', 'PlayerId'],
    'FURL': ['Protocol', 'Host', 'Port', 'Valid', 'Map', 'RedirectURL', 'Op', 'Portal'],
    'FString': [],
    'FActorSpawnParameters': ['Name', 'Template', 'Owner', 'Instigator', 'OverrideLevel', 'SpawnCollisionHandlingOverride', 'TransformScaleMethod', 'bRemoteOwned', 'bNoFail', 'bDeferConstruction', 'bAllowDuringConstructionScript', 'NameMode', 'ObjectFlags'],
    'FRepMovement': ['LinearVelocity', 'AngularVelocity', 'Location', 'Rotation', 'bSimulatedPhysicSleep', 'bRepPhysics', 'ServerFrame', 'ServerPhysicsHandle'],
    'USceneComponent': ['AttachParent', 'AttachChildren', 'RelativeLocation', 'RelativeRotation', 'RelativeScale3D', 'ComponentVelocity', 'bAbsoluteLocation'],
    'UPlayer': ['PlayerController', 'CurrentNetSpeed', 'ConfiguredInternetSpeed', 'ConfiguredLanSpeed'],
    'ULocalPlayer': ['ViewportClient', 'ControllerId'],
    'UHealthComponent': ['HitpointRatio', 'MinHitpointRatio', 'BonusHitpointRatio', 'CannotDeplete'],
    'AESPawn': ['XP', 'LootDrop', 'bIsPlayerPawn', 'ShipData', 'bGetShipModulesFromShipData', 'bGetShipColorsFromShipData', 'bGetDecalsFromShipData'],
    'AESPlayerController': [],
    'AESGameModeBase': ['ESPlayerPawn', 'ESPlayerController', 'PlayerData'],
    'UESGameInstance': ['bUseWorldOriginShifting', 'WorldOriginShiftingStack'],
    'FOnlineSubsystemSteam': ['bSteamworksClientInitialized', 'SessionInterface', 'IdentityInterface'],
    'UPlayerData': ['Ships', 'ShipsSaveState', 'CurrentShip', 'CompletedMissions', 'TrackedMainMission', 'TrackedSideMission', 'TrackedJob'],
    'FTaskSaveGameData': ['TaskID', 'TaskState', 'Stage', 'Progress', 'LocationID', 'StationID', 'TimeStamp', 'bIsMission'],
    'AMissionBase': ['MissionTaskID'],
    'AMissionTaskBase': ['MissionTaskID', 'LocationID', 'StationID', 'TaskState', 'bIsHidden', 'ProgressValue', 'StageValue', 'TimeStamp'],
    'UXPComponent': ['XP', 'NeededPlayerDamageRatio'],
    'UGameData': ['Locations'],
    'FLocationData': ['Name', 'LocationID', 'SystemID', 'MapAssetString', 'bNeverShowIngame'],
    'UItem': ['ItemTemplateID', 'Seed', 'NameSeed', 'ItemLevel', 'Rarity', 'Amount'],
    'APickupBase': ['PickupEntry'],
    'FPickupEntry': ['PickupClassPath', 'PickupInventory'],
    'FWeaponInfo': ['WeaponItem', 'WeaponClass', 'SpawnedWeapons'],
    'UActorComponent': ['OwnerPrivate'],
    'UWeaponComponent': ['WeaponSlots', 'WeaponSockets', 'WeaponCategory', 'EquippedSlotIndex',
                         'FocusLocation', 'ClampedNonAutoAimedFocusLocation', 'CurrentAutoAimTarget', 'LockedTarget',
                         'RemainingMissileLockTime',
                         'OverrideShootAtTarget', 'bUseAutoAiming', 'bShootWeaponsAtFocusLocation',
                         'FocusPointDistance'],
    'UDeviceComponent': ['DeviceSlots', 'SelectedDeviceIndex'],
    'UConsumableComponent': ['ConsumableSlots', 'SelectedConsumableIndex'],
    'FDeviceInfo': ['DeviceItem', 'DeviceClass', 'SpawnedDevice'],
    'FConsumableInfo': ['ConsumableItem', 'ConsumableClass', 'DefaultConsumableObject'],
    'UInventory': ['PrimaryWeapons', 'SecondaryWeapons', 'EnergyCores', 'Sensors', 'Shields', 'CargoUnits', 'Platings', 'Thrusters', 'Devices', 'Consumables', 'Cargo'],
    'FShipData': ['Name', 'Inventory', 'ShipItemInstance', 'UltimateDevice'],
    'UShipMovementComponent': [],
}
BITFIELDS = {  # class -> bitfield members  (adds <m>_off and <m>_mask)
    'FActorSpawnParameters': ['bRemoteOwned', 'bNoFail', 'bDeferConstruction', 'bAllowDuringConstructionScript'],
    'AActor': ['bActorIsBeingDestroyed', 'bActorInitialized', 'bReplicates', 'bReplicateMovement', 'bNetStartup', 'bOnlyRelevantToOwner', 'bAlwaysRelevant', 'bNetLoadOnClient', 'bNetUseOwnerRelevancy', 'bHidden', 'bTearOff', 'bExchangedRoles', 'bHasFinishedSpawning', 'bActorEnableCollision', 'bCanBeDamaged', 'bReplicateUsingRegisteredSubObjectList'],
    'APawn': ['bUseControllerRotationPitch', 'bCanAffectNavigationGeneration'],
    'UNetDriver': [],
}

def load_symbols(path):
    syms = []
    with open(path) as f:
        for line in f:
            parts = line.rstrip('\n').split('\t')
            if len(parts) < 4: continue
            syms.append((int(parts[0], 16), parts[1], parts[2], parts[3]))
    return syms

def main():
    symbols_tsv, types_txt, outdir = sys.argv[1:4]
    os.makedirs(outdir, exist_ok=True)
    syms = load_symbols(symbols_tsv)
    by_name = {}
    rva_count = {}
    for rva, kind, mang, dem in syms:
        by_name.setdefault(dem, []).append(rva)
        if kind == 'function': rva_count[rva] = rva_count.get(rva, 0) + 1
    lines = ['// AUTO-GENERATED by tools/gen_sdk.py from ES2-Win64-Shipping.pdb — do not edit', '#pragma once', '#include <cstdint>',
             'namespace es2rva {', f'  constexpr uint32_t PE_TIMESTAMP = 4225050077u; // ES2-Win64-Shipping.exe build id guard']
    errors = 0
    for ident, sig in RVAS.items():
        if sig.startswith('regex:'):
            rx = re.compile(sig[6:])
            hits = sorted({rva for dem, rvas in by_name.items() if rx.search(dem) for rva in rvas})
            names = sorted({dem for dem in by_name if rx.search(dem)})
        else:
            hits = sorted(set(by_name.get(sig, [])))
            names = [sig] if hits else []
        if len(hits) != 1:
            print(f'ERROR: {ident}: {len(hits)} matches for {sig!r}: {names[:5]}', file=sys.stderr); errors += 1
            lines.append(f'  // ERROR {ident}: {len(hits)} matches')
            continue
        folded = rva_count.get(hits[0], 0)
        if folded > 1:
            print(f'WARNING: {ident}: RVA 0x{hits[0]:X} is shared by {folded} functions (ICF-folded stub) — do NOT hook it', file=sys.stderr)
            lines.append(f'  constexpr uint32_t {ident} = 0x{hits[0]:X}; // {names[0]}  !!! ICF-FOLDED x{folded}: never hook')
            lines.append(f'  constexpr bool {ident}_IS_FOLDED = true;')
        else:
            lines.append(f'  constexpr uint32_t {ident} = 0x{hits[0]:X}; // {names[0]}')
    lines.append('}')
    open(os.path.join(outdir, 'rvas.h'), 'w').write('\n'.join(lines) + '\n')
    print(f'wrote rvas.h ({len(RVAS)} entries, {errors} errors)', file=sys.stderr)

    # offsets via pdb_types
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import pdb_types
    db = pdb_types.TypeDB(types_txt)
    out = ['// AUTO-GENERATED by tools/gen_sdk.py from ES2-Win64-Shipping.pdb — do not edit', '#pragma once', '#include <cstdint>', 'namespace es2off {']
    for cls, members in OFFSETS.items():
        r = db.resolve_def(cls)
        if r is None:
            print(f'WARNING: class {cls} not found', file=sys.stderr); continue
        lay = db.layout(cls)
        bym = {}
        for off, sz, tn, nm, owner in lay:
            bym.setdefault(nm, (off, sz, tn, owner))
        out.append(f'  namespace {cls} {{')
        out.append(f'    constexpr uint32_t __size = {r["size"]};')
        for m in members:
            if m not in bym:
                print(f'WARNING: {cls}::{m} not found', file=sys.stderr); out.append(f'    // MISSING {m}'); continue
            off, sz, tn, owner = bym[m]
            out.append(f'    constexpr uint32_t {m} = 0x{off:X}; // {tn} (size {sz}) from {owner}')
        for m in BITFIELDS.get(cls, []):
            if m not in bym:
                print(f'WARNING: {cls}::{m} bitfield not found', file=sys.stderr); out.append(f'    // MISSING bitfield {m}'); continue
            off, sz, tn, owner = bym[m]
            mm = re.search(r': (\d+) @bit(\d+)', tn)
            if not mm:
                out.append(f'    // {m} is not a bitfield: {tn}'); continue
            bits, bitoff = int(mm.group(1)), int(mm.group(2))
            mask = ((1 << bits) - 1) << bitoff
            out.append(f'    constexpr uint32_t {m}_off = 0x{off:X}; constexpr uint32_t {m}_mask = 0x{mask:X}; // {tn} from {owner}')
        out.append('  }')
    out.append('}')
    open(os.path.join(outdir, 'offsets.h'), 'w').write('\n'.join(out) + '\n')
    print('wrote offsets.h', file=sys.stderr)
    sys.exit(1 if errors else 0)

if __name__ == '__main__':
    main()
