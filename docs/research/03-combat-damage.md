# ES2 co-op research: damage

_Auto-generated from the PDB research workflow (2026-08-19)._

## Summary

ES2 combat is 100% single-player code: there is not one HasAuthority/GetLocalRole/GetNetMode check anywhere in the fire or damage path (verified — the only `HasAuthority` symbol in the whole binary is `UMovieSceneSequencePlayer::HasAuthority`). Every machine that ticks a weapon component will trace, spawn projectiles and subtract hitpoints locally. So co-op combat = (a) hard-block the damage funnel on the client, (b) ship the client's fire intent to the host, (c) push results back.

FIRE PATH (verified by disassembly, top to bottom)
AESPlayerController::InputStartFirePrimary @017177A4 reads `this->ESPawn` (AESPlayerController+0xA40), gates on `bIsMovementBlocked`(+0x897), `bSelectingDevicesOrConsumables`(+0xB33), `ESPawn->Health(+0x350)->IsDepleted()`, `ESPawn->JumpDrive(+0x3B0)->bIsInCruiseMode(+0x318)` and `->bOwnerIsChargingJump(+0x3A0)`, then calls `ESPawn->PrimaryWeapons(+0x380)->StartFire()` and `JumpDrive->SetCruiseMode(false)`.
UWeaponComponent::StartFire @0150FE78 does almost nothing: broadcasts OnTriggerPulled(+0x608), checks the FBuffableBool `bAllowFire`(+0x8D8), sets **bFireActivated (UWeaponComponent+0x7B4) = true** and stamps FireHoldStartTimeStamp(+0x7B0). StopFire @0171760C clears it.
=> **Firing is a continuous per-tick bool, not a discrete event.** `UWeaponComponent::TickComponent` @012A2368 reads +0x7B4 every tick (5 sites) and drives cadence via NextShotWithinBurstCountdown/RemainingShotsInBurst, then calls `ProcessNewShot(float,bool)` @012D5E64 -> `AWeaponBase::ProcessFiring(float,float,bool)` @012D65A0 -> `AWeaponBase::FireWeapon(float,float,int,FVector,FVector)` @012D7344.
FireWeapon branches on `IsInstantHitWeapon()`:
 * projectile: `AProjectileBase::GetProjectileInitialVelocity` -> `FMath::VRandCone(spread)` -> **`AWeaponBase::SpawnProjectile` @012D3568**; damage happens later in `AProjectileBase::Explode` @0167EFD0 -> UGameplayLib::ApplyESPointDamage.
 * hitscan: `AWeaponBase::WeaponTrace` @01872284 -> **`AWeaponBase::DealInstantHitDamage` @014289AC** -> `CalculateDamage` -> `UGameplayLib::ApplyESPointDamage`.
Aim comes from `AWeaponBase::GetAimDirection` @012A398C which reads `WeaponComponent->FocusLocation` (UWeaponComponent+0x888). FocusLocation is produced each tick by TickComponent from `DesiredFocusLocation`(+0x840) via ClampWeaponRotation+SmoothedAutoaim, and DesiredFocusLocation is set by **`UWeaponComponent::SetDesiredFocusLocationFromDeprojectedCrosshair(FVector const&, FVector const&)` @012D8788**, called twice (PrimaryWeapons +0x380, SecondaryWeapons +0x388) from `AESPlayerController::ProcessPlayerInput` @01380AF0 with `this+0xA70` (CrosshairLocationWorld) and `this+0xA88` (CrosshairDirectionWorld). That two-vector pair is the entire aim state.

DAMAGE FUNNEL (the single choke point)
All four public entry points — UGameplayLib::ApplyESDamage @01429694, ApplyESPointDamage @01429804, ApplyESRadialDamage @016EE520, ApplyESDamageToComponent @05E20A58 — tail into ONE unnamed static helper at **RVA 012D0210** (proved by an E8/E9 rel32 scan of .text: exactly 5 call sites, the 4 above plus one outlined cold chunk at 02886D54). That helper resolves Shield/Armor/Health via FindComponentByClass, applies item/faction/difficulty modifiers, then calls `UHitpointComponent::TakeDamage` through **vtable slot 151 (offset 0x4B8)** on each component in turn, and finally `AActor::TakeDamage` through **vtable slot 207 (offset 0x678)** on the target. `AESPawn::TakeDamage` @0142AF94 is purely cosmetic (HUD marker / lock-on chatter) — it does NOT touch health. The actual hitpoint mutation is `UHealthComponent::TakeDamage` @015583E8 (and Shield @012CEEB0 / Armor @01556B2C) calling `UHitpointComponent::ChangeHitpoints(float,float&,float&)` @014E55C8, which writes `HitpointRatio` (UHitpointComponent+0xB0). Health depletion in UHealthComponent::TakeDamage broadcasts OnHealthDepleted/OnDied, calls UGameplayLib::DamageDealtByPlayerOrPlayerFriend (HUD numbers only), UPlayerData::OnKilledActor, UMetaSaveData::IncDeathCount.
**Health state on the wire is ONE float per component: HitpointRatio at +0xB0 (0..MaxHitpointRatio).** `UHitpointComponent::GetRatio()` @012D00CC returns HitpointRatio + BonusHitpointRatio(+0x230). The setter `SetCurrentHitpointsWithRatio(float)` (vtable slot 153; UHitpointComponent @01556A24, UHealthComponent override @01556948) clamps and broadcasts OnHitpointsChanged — it is a **reflected UFunction**, so the client can apply a host-pushed ratio with a single ProcessEvent call.

DEATH
`AESPawn::Die_Implementation` @05DE2718 is 0x24 bytes and only calls `USelfRegisteringComponent::Unregister` — the real death logic is Blueprint (AESPawn::Die is a BlueprintNativeEvent, vtable slot 278; `OnHealthDepleted` is a reflected BP event). Loot: UHealthComponent's OnHealthDepleted drives `ULootDropComponent::OwnerHealthDepleted` -> `DropLoot(bool)` @016CCDC4 -> `UGameplayLib::SpawnPickups`, and `UXPComponent::OwnerHealthDepleted`. All host-side. Actor destruction replicates natively; hitpoints do not.

REPLICATION REALITY CHECK
An xref scan for `UActorComponent::SetIsReplicated` @0188F62C found **zero ES2 game callers** — Health/Shield/Armor/Weapon components are not replicated at all. Flipping CPF_Net on `HitpointRatio` alone would therefore do nothing; you would additionally have to SetIsReplicated(true) on every instance and re-run SetUpRuntimeReplicationData on every already-linked BP subclass on both sides. Recommendation: do NOT use runtime CPF_Net for combat state; use RPCs.

RPC CHANNELS (FunctionFlags decoded from each Z_Construct_..._Statics::FuncParams blob in .rdata; NumProps/StructureSize in the uint16 pair at +0x30, EFunctionFlags dword at +0x38)
Best client->host binary: **APlayerController::ServerRecvClientInputFrame(int32, TArray<uint8>)** — flags 0x00220C42 = Net|NetServer|Native|Event|Public, **no FUNC_NetReliable, no FUNC_NetValidate**, StructureSize 0x18, 3 props. Its _Implementation @04E974E8 only writes into `APlayerController::InputBuffer_DEPRECATED` (+0x6F0, 264B) — dead network-physics code in this build. Arbitrary byte payload, unreliable, zero side effects if the hook skips the original.
Best host->client with an actor reference: **ClientPrestreamTextures(AActor*, float, bool, int32)** — flags 0x01020CC2 = NetClient|**NetReliable**, size 0x18, 4 props; _Implementation @04E7DC48 just calls IsPrimaryPlayer + AActor vtable slot 220. The AActor* is serialized as a NetGUID and resolves to the client's own replicated copy — perfect for "actor X ratio Y".
Also available and unique-RVA-hookable: ClientRecvServerAckFrame(int,int,int8) @01973F00 (unreliable, writes only ClientFrameInfo_DEPRECATED +0x7F8), ClientAddTextureStreamingLoc(FVector,float,bool) @04E7CF58 (reliable), ClientTeamMessage(APlayerState*,FString,FName,float) @04E7F77C (reliable), ClientPlaySoundAtLocation(USoundBase*,FVector,float,float) @04E7DA54 (unreliable), ServerSetSpectatorLocation(FVector,FRotator) @04E97A18 (unreliable, full-precision vector+rotator — a natural fit for the crosshair pair), ServerUpdateCamera(FVector_NetQuantize,int32) @04E97D6C (unreliable), ServerAcknowledgePossession(APawn*) @04E96F44 (reliable, object ref), ServerNotifyLoadedWorld(FName) @04E973A8 (reliable). Note `ServerExecRPC(FString)` and `ClientRepObjRef(UObject*)` are reliable and their _Implementations are the ICF-folded empty `ret` at 012386C0 — free of side effects but **unhookable by RVA**; they can only be received by intercepting ProcessEvent.
ES2 defines no Server*/Client*/Multicast UFunctions of its own (grep of es2_functions.txt returned nothing) — all RPC capacity must come from engine APlayerController functions.

DESIGN
1. Client: install the funnel block (012D0210) whenever NetMode==NM_Client. All client-side damage (own hitscan, own projectiles, replicated NPCs firing locally) dies at one address. Client keeps local muzzle flashes/projectiles for feel.
2. Client, per ProcessPlayerInput (@01380AF0, post-hook, throttle to 30–60 Hz): pack {u8 flags(bFirePrimary|bFireSecondary|charging), 3xfloat crosshair loc (relative to pawn, float is plenty), 3xfloat crosshair dir, u32 lockedTargetNetGUID, u16 seq} ≈ 32 bytes into ServerRecvClientInputFrame(seq, bytes).
3. Host: hook ServerRecvClientInputFrame_Implementation @04E974E8, skip the original, find that PC's `ESPawn` (+0xA40), then per packet: call `SetDesiredFocusLocationFromDeprojectedCrosshair` @012D8788 on PrimaryWeapons(+0x380) and SecondaryWeapons(+0x388), resolve+`SetLockedTarget`, and edge-trigger `StartFire`/`StopFire` (both reflected UFunctions, or RVAs 0150FE78/0171760C). The host's own TickComponent then does the trace, the spread roll, the ammo/energy consumption and the damage with full authority — no need to trust or transmit hit reports at all. Re-validate host-side with the same gates InputStartFirePrimary uses (Health->IsDepleted, JumpDrive->bIsInCruiseMode/bOwnerIsChargingJump).
   `UGameplayLib::IsPlayerPawn` @012A1810 returns true for any AESPawn whose vtable slot 254 IsPlayerControlled() is true, so the joiner's server-side pawn is correctly treated as a player pawn by damage/loot/XP logic once possessed — no extra flag needed.
4. Host->client results: hook the funnel @012D0210 as a *pass-through* on the host (call original, then read target->Health(+0x350)/Shield(+0x360)/Armor(+0x358) `HitpointRatio`+0xB0) and push at most ~10 Hz per changed actor via ClientPrestreamTextures(TargetActor, ratio, bIsShield, packedExtra). Client hook @04E7DC48 skips the original and calls the reflected UFunction `SetCurrentHitpointsWithRatio` on the matching component, which fires OnHitpointsChanged so HUD/shield FX follow.
5. Kills: nothing extra is needed for the actor disappearing — UE replicates actor destruction automatically. Loot pickups spawn host-side; whether they reach the client depends on APickupBase's bReplicates (BP default, must be checked live). Death FX and the "kill" HUD credit are host-local and need one extra event over the same channel if you want the joiner to see them.
6. Minimal property set if you insist on native replication: only `UHitpointComponent::HitpointRatio` (float @0xB0, is a real reflected FProperty) and `UWeaponComponent::bFireActivated` (bool @0x7B4, reflected) are worth it — everything else (EquippedSlotIndex @0x8D0, CurrentChargeRatio @0x8CC, LockedTarget @0x8C0 which is a TWeakObjectPtr and not net-serializable) is either unreflected or unreplicable. Both live on components that are not replicated at all, so the cost is high and the RPC path is strictly better.

## Key functions

- **AESPlayerController::InputStartFirePrimary** @ `017177A4` unique=True — Input handler. Reads this->ESPawn (+0xA40); bails if bIsMovementBlocked(+0x897) or bSelectingDevicesOrConsumables(+0xB33) set, if ESPawn->Health(+0x350)->IsDepleted(), or if ESPawn->JumpDrive(+0x3B0)->bIsInCruiseMode(+0x318) / bOwnerIsChargingJump(+0x3A0). Otherwise calls PrimaryWeapons(+0x380)->StartFire() then JumpDrive->SetCruiseMode(false). These are exactly the gates the host should re-check 
  - `private: void __cdecl AESPlayerController::InputStartFirePrimary(void)`
  - ABI: __fastcall, rcx=this. No args.
- **AESPlayerController::InputStopFirePrimary** @ `017173D4` unique=True — Mirror of the start handler; ends in UWeaponComponent::StopFire on PrimaryWeapons.
  - `private: void __cdecl AESPlayerController::InputStopFirePrimary(void)`
  - ABI: rcx=this.
- **AESPlayerController::InputStartFireSecondary** @ `01715CC8` unique=True — Same pattern for SecondaryWeapons (AESPawn+0x388).
  - `private: void __cdecl AESPlayerController::InputStartFireSecondary(void)`
  - ABI: rcx=this.
- **AESPlayerController::InputStopFireSecondary** @ `01715EC4` unique=True — Stops secondary fire.
  - `private: void __cdecl AESPlayerController::InputStopFireSecondary(void)`
  - ABI: rcx=this.
- **AESPlayerController::ProcessPlayerInput** @ `01380AF0` unique=True — Per-frame local-controller input. At +0x342 calls DeprojectScreenPositionToWorld64, stores the result into this->CrosshairLocationWorld(+0xA70) and CrosshairDirectionWorld(+0xA88), then calls UWeaponComponent::SetDesiredFocusLocationFromDeprojectedCrosshair(&+0xA70,&+0xA88) on ESPawn->PrimaryWeapons(+0x380) and ESPawn->SecondaryWeapons(+0x388). Best client-side sampling point for aim + fire state 
  - `public: virtual void __cdecl AESPlayerController::ProcessPlayerInput(float DeltaTime, bool bGamePaused)`
  - ABI: rcx=this, xmm1=float DeltaTime, r8b=bool. Virtual override of APlayerController::ProcessPlayerInput.
- **UWeaponComponent::StartFire** @ `0150FE78` unique=True — Broadcasts OnTriggerPulled(+0x608); if the FBuffableBool bAllowFire(+0x8D8) current value != 0, sets bFireActivated(+0x7B4)=true and FireHoldStartTimeStamp(+0x7B0)=World->TimeSeconds. Does NOT shoot. This is the single bit the host needs flipped on the joiner's server-side pawn.
  - `public: void __cdecl UWeaponComponent::StartFire(void)`
  - ABI: rcx=this. Callable via ProcessEvent (Z_Construct_UFunction_UWeaponComponent_StartFire exists), no params.
- **UWeaponComponent::StopFire** @ `0171760C` unique=True — Broadcasts OnTriggerReleased(+0x618) and OnFireStopped(+0x5F8), zeroes FireHoldStartTimeStamp(+0x7B0), clears bFireActivated(+0x7B4), sets bCanPlayDepletedSound(+0x9C8)=1, and applies ramp-down bookkeeping to the first equipped AWeaponBase.
  - `public: void __cdecl UWeaponComponent::StopFire(void)`
  - ABI: rcx=this.
- **UWeaponComponent::TickComponent** @ `012A2368` unique=True — The engine of firing. Reads bFireActivated(+0x7B4) at 5 sites, maintains RampUpCountdown(+0x938)/NextShotWithinBurstCountdown(+0x944)/RemainingShotsInBurst(+0x948), clears bFocusLocationNeedsUpdate(+0x820) and recomputes FocusLocation(+0x888) via ClampWeaponRotation + SmoothedAutoaim from DeprojectedCrosshairLocation(+0x828)/CrosshairDirection(+0x798), writes CurrentAutoAimTarget(+0x728), then cal
  - `public: virtual void __cdecl UWeaponComponent::TickComponent(float DeltaTime, enum ELevelTick, struct FActorComponentTickFunction *)`
  - ABI: rcx=this, xmm1=float, r8d=ELevelTick, r9=FActorComponentTickFunction*.
- **UWeaponComponent::ProcessNewShot** @ `012D5E64` unique=True — Per-shot gate: charge/beam checks, ammo (GetAmmo/ConsumeAmmo), energy (GetCurrentWeaponsEnergyConsumption/IsEnoughEnergyAvailable), plays depleted sounds, then calls AWeaponBase::ProcessFiring for each equipped weapon instance and UpdateCoolDownAndInstantRampDown.
  - `private: void __cdecl UWeaponComponent::ProcessNewShot(float DeltaTime, bool bIsBurst)`
  - ABI: rcx=this, xmm1=float, r8b=bool. Function body is chunked by PGO (symbol range says 0x64 bytes; real code continues at 0x12D5F2C+).
- **UWeaponComponent::SetDesiredFocusLocationFromDeprojectedCrosshair** @ `012D8788` unique=True — Sets bFocusLocationNeedsUpdate(+0x820)=1, DeprojectedCrosshairLocation(+0x828)=loc, CrosshairDirection(+0x798)=dir, DesiredFocusLocation(+0x840)=loc+dir*FocusPointDistance(+0x280). This is the whole aim input for a weapon component — the host calls it on the joiner's server-side pawn with the two vectors received over the wire.
  - `public: void __cdecl UWeaponComponent::SetDesiredFocusLocationFromDeprojectedCrosshair(struct UE::Math::TVector<double> const & CrosshairWorldLoc, struct UE::Math::TVector<double> const & CrosshairWorldDir)`
  - ABI: rcx=this, rdx=FVector const* (24B, BY POINTER — const-ref), r8=FVector const* (24B). NOT reflected, so the host must call it by RVA.
- **UWeaponComponent::SetLockedTarget** @ `012A6090` unique=True — Sets LockedTarget (TWeakObjectPtr @+0x8C0) and fires OnNewTargetLocked/OnTargetUnLocked. Reflected, so callable via ProcessEvent once the host resolves the client's target NetGUID.
  - `public: void __cdecl UWeaponComponent::SetLockedTarget(class AActor *)`
  - ABI: rcx=this, rdx=AActor*.
- **UWeaponComponent::SetAllowFire** @ `017174E8` unique=True — Pushes/pops a modifier on the FBuffableBool bAllowFire(+0x8D8) (handles in DisallowFireHandles +0x918). Clean way to hard-disable a weapon component (e.g. to stop client-side simulated NPCs firing) without touching bFireActivated.
  - `public: void __cdecl UWeaponComponent::SetAllowFire(bool)`
  - ABI: rcx=this, dl=bool.
- **UWeaponComponent::GetFirstEquippedWeaponInstance** @ `012A2EE4` unique=True — Returns the spawned AWeaponBase for the currently equipped slot (EquippedSlotIndex +0x8D0). Useful to read WeaponConfig(+0x2F8) for validation on the host.
  - `public: class AWeaponBase * __cdecl UWeaponComponent::GetFirstEquippedWeaponInstance(void) const`
  - ABI: rcx=this, returns AWeaponBase* in rax.
- **AWeaponBase::ProcessFiring** @ `012D65A0` unique=True — One trigger pull for one weapon actor: IsChargeWeapon check, calls FireWeapon, PlayWeaponFireFX, then consumes energy (GetEnergyConsumptionForOneShot -> UWeaponComponent::ChangeEnergyForCurrentWeapon) and ammo (RequiresAmmo -> UWeaponComponent::ConsumeAmmo).
  - `public: void __cdecl AWeaponBase::ProcessFiring(float ChargePercent, float DeltaOrRate, bool bSomething)`
  - ABI: rcx=this, xmm1=float, xmm2=float, r8b=bool.
- **AWeaponBase::FireWeapon** @ `012D7344` unique=True — THE shot function. Calls GetAimDirection (reads WeaponComponent->FocusLocation +0x888), then branches on IsInstantHitWeapon(): projectile path -> AProjectileBase::GetProjectileInitialVelocity, FMath::VRandCone(spread), AWeaponBase::SpawnProjectile; hitscan path -> GetActualProjectileSpawnLocation, VRandCone, GetWeaponRange, AWeaponBase::WeaponTrace, AWeaponBase::DealInstantHitDamage, reflection ha
  - `protected: void __cdecl AWeaponBase::FireWeapon(float ChargePercent, float Spread, int NumProjectiles, struct UE::Math::TVector<double> StartLocation, struct UE::Math::TVector<double> AimDirection)`
  - ABI: rcx=this, xmm1=float, xmm2=float, r9d=int, [rsp+0x28]=FVector* (BY HIDDEN POINTER, 24B), [rsp+0x30]=FVector* (BY HIDDEN POINTER). Confirms the >8-byte-struct-by-pointer rule.
- **AWeaponBase::SpawnProjectile** @ `012D3568` unique=True — The projectile-weapon spawn point. Instantiates/recycles an AProjectileBase (pool via UActorPool) and calls AProjectileBase::Init(FWeaponData const&, float, APawn*, AWeaponBase*, AActor*, FLinearColor, int) @012D4788. Projectiles are NOT replicated (AProjectileBase ctor never calls SetReplicates) — they are per-machine cosmetics whose damage lands in Explode.
  - `private: void __cdecl AWeaponBase::SpawnProjectile(struct UE::Math::TVector<double> Loc, struct UE::Math::TVector<double> Vel, float ChargePercent, int Index, bool bDuplicate, struct FWeaponData Config)`
  - ABI: FVector params and the 1808-byte FWeaponData all pass BY HIDDEN POINTER.
- **AWeaponBase::WeaponTrace** @ `01872284` unique=True — The hitscan line trace used for instant-hit weapons.
  - `public: struct FHitResult __cdecl AWeaponBase::WeaponTrace(struct UE::Math::TVector<double> const & Start, struct UE::Math::TVector<double> const & End)`
  - ABI: Returns a 256-byte FHitResult BY HIDDEN POINTER in rcx; rdx=this; r8/r9 = the two FVector const*. Classic MSVC sret shift.
- **AWeaponBase::DealInstantHitDamage** @ `014289AC` unique=True — THE function that applies damage for hitscan weapons. Copies WeaponConfig(+0x2F8), AddDamageModifiersToCopyWeaponConfig, AWeaponBase::CalculateDamage, builds an FPointDamageEvent, then calls UGameplayLib::ApplyESPointDamage. Afterwards fires OnWeaponHit / CheckForItemImpactEffects / FOnPlayerWeaponHitActor::Broadcast.
  - `public: void __cdecl AWeaponBase::DealInstantHitDamage(float ChargePercent, float DamageScale, struct FHitResult const & Hit, struct UE::Math::TVector<double> const & ShotDir, float & OutHullDamage, float & OutShieldDamage, bool & bOutCritical)`
  - ABI: rcx=this, xmm1=float, xmm2=float, r9=FHitResult const*, then stack: FVector const*, float*, float*, bool*.
- **AWeaponBase::CalculateDamage** @ `012D5978` unique=True — Turns weapon data + component damage factors (DamageFactor +0x298, DamageFactorEnergy +0x2D8, DamageFactorKinetic +0x318, CriticalHitDamageFactor +0x418) into the hull/shield damage numbers passed to ApplyESPointDamage. Useful for host-side sanity-checking a reported damage value.
  - `public: static void __cdecl AWeaponBase::CalculateDamage(struct FWeaponData const &, class UWeaponComponent const *, float ChargePercent, float Scale, float & OutHull, float & OutShield)`
  - ABI: static; rcx=FWeaponData const* (1808B, by pointer), rdx=UWeaponComponent const*, xmm2/xmm3 floats, [rsp+0x28]=float*, [rsp+0x30]=float*.
- **ES_ApplyDamage_Internal (UNNAMED — the single damage funnel)** @ `012D0210` unique=True — THE single funnel where a target's health actually decreases. Verified by an E8/E9 rel32 scan over .text: the only callers are ApplyESDamage(+0x14E), ApplyESPointDamage(+0x1C0), ApplyESRadialDamage(+0x6A3), ApplyESDamageToComponent(+0x3FD) and one outlined cold chunk (02886D54). It resolves UTurretComponent/UFactionComponent/UShieldComponent/UArmorComponent/UHealthComponent on the target, honours 
  - `void __cdecl <no symbol>(struct FDamageInfo * OutApplied /*rcx*/, class AActor * DamagedActor /*rdx*/, struct FDamageEvent const * /*r8*/, struct FHitResult const * /*r9*/, struct FDamageInfo const * DamageIn /*[rsp+0x20]*/, struct UE::Math::TVector<double> const * HitFromDirection /*[rsp+0x28]*/, class AController * EventInstigator /*[rsp+0x30]*/, class AActor * DamageCauser /*[rsp+0x38]*/, bool * bOutIsCriticalHit /*[rsp+0x40]*/, struct FWeaponData const * WeaponConfig /*[rsp+0x48]*/, float ShieldPiercingRatio /*[rsp+0x50]*/, float ArmorPiercingRatio /*[rsp+0x58]*/, class UClass * HitpointComponentClass /*[rsp+0x60]*/, bool bAllowPlayerCriticalDamage /*[rsp+0x68]*/, bool bFlag /*[rsp+0x70]*/)`
  - ABI: NO symbol exists at this RVA (grep '^012D0210\t' returns 0 rows) — it is a static helper, so it is trivially not ICF-shared, but the mod's gen_sdk must be told about it explicitly. Prologue is `mov rax,rsp; push rbp; push rbx; push rsi; push rdi; push r12..r15; lea rbp,[rax-0x948]; sub rsp,0xA08` — 
- **UGameplayLib::ApplyESPointDamage** @ `01429804` unique=True — Public point-damage entry. Builds a UDamageTypeBase subclass via GetDamageType(ETypeOfDamage 2) and an FPointDamageEvent, then calls the funnel. This is what DealInstantHitDamage and AProjectileBase::Explode use.
  - `public: static struct FDamageInfo __cdecl UGameplayLib::ApplyESPointDamage(class AActor * DamagedActor, struct FDamageInfo DamageAmount, struct UE::Math::TVector<double> const & HitFromDirection, struct FHitResult const & HitInfo, class AController * EventInstigator, class AActor * DamageCauser, bool & bIsCriticalHit, struct FWeaponData WeaponConfig, float ShieldPiercingRatio, float ArmorPiercingRatio, bool bAllowPlayerCriticalDamage)`
  - ABI: sret: rcx = hidden FDamageInfo* return slot, rdx=DamagedActor, r8=FDamageInfo* (12B, BY POINTER), r9=FVector const*, then stack [RSP0+0x28]=FHitResult const*, +0x30=AController*, +0x38=AActor*, +0x40=bool*, +0x48=FWeaponData* (1808B by pointer), +0x50/+0x58 floats, +0x60 bool. As a UFunction: 12 pro
- **UGameplayLib::ApplyESDamage** @ `01429694` unique=True — Simplest authoritative damage entry — the one to call from a host-side hook if you ever do want to apply an explicit damage amount (e.g. validated client hit reports). Constructs a plain FDamageEvent + default FHitResult and calls the funnel.
  - `public: static struct FDamageInfo __cdecl UGameplayLib::ApplyESDamage(class AActor * DamagedActor, struct FDamageInfo DamageAmount, class AController * EventInstigator, class AActor * DamageCauser, bool & bIsCriticalHit, float ShieldPiercingRatio, float ArmorPiercingRatio, bool bAllowPlayerCriticalDamage)`
  - ABI: sret: rcx = hidden FDamageInfo* return, rdx=DamagedActor, r8=FDamageInfo*, r9=AController*, [RSP0+0x28]=AActor* DamageCauser, +0x30=bool*, +0x38=float, +0x40=float, +0x48=bool. As a UFunction: 9 props, StructureSize 0x48 (72B), flags 0x04422401. Param-struct offsets: 0 DamagedActor, 8 DamageAmount(F
- **UGameplayLib::ApplyESRadialDamage** @ `016EE520` unique=True — Explosion damage (missiles, mines, self-destructs). Also funnels through 012D0210, so the single client-side block covers it.
  - `public: static bool __cdecl UGameplayLib::ApplyESRadialDamage(class UObject const *, struct FDamageInfo, struct UE::Math::TVector<double> const &, float Radius, class TArray<class AActor *> const & IgnoreActors, bool &, struct FWeaponData, class TArray<class AActor *> & OutHit, class TArray<class AActor *> & OutKilled, class TArray<struct FHitResult> &, float, float, class AActor *, class AController *, bool, bool, bool, bool)`
  - ABI: Loops targets and calls the funnel per target with the same hidden-FDamageInfo-out convention (lea rcx,[rbp-0x10]).
- **UGameplayLib::ApplyESDamageToComponent** @ `05E20A58` unique=True — Targeted shield-only / hull-only damage. Fourth funnel caller.
  - `public: static struct FDamageInfo __cdecl UGameplayLib::ApplyESDamageToComponent(class AActor *, struct FDamageInfo, class AController *, class AActor *, bool &, class TSubclassOf<class UHitpointComponent>, bool)`
  - ABI: TSubclassOf<> passes BY HIDDEN POINTER. Fills the funnel's 13th slot (HitpointComponentClass) so only one component type is damaged.
- **UHitpointComponent::TakeDamage (pure virtual base, ICF-folded)** @ `012386C0` unique=False — Empty base. The real work is in the three overrides.
  - `public: virtual void __cdecl UHitpointComponent::TakeDamage(struct FDamageInfo const &, bool, class AController *, class AActor *, struct FDamageEvent const &, struct FHitResult const &, float &, float &, bool &)`
  - ABI: *** DO NOT HOOK *** — this RVA is the universal ICF-folded `ret 0` stub shared by 30486 symbols. It is vtable SLOT 151 (offset 0x4B8) — always dispatch through the vtable, or hook the concrete overrides below.
- **UHealthComponent::TakeDamage** @ `015583E8` unique=True — Where hull HP actually drops. Honours bIsInvulnerable(+0x168)/IgnoreDamageChance(+0x1E8)/DamageReduction(+0x240)/HullDamageReduction(+0x418)/DamageLimit(+0x494), broadcasts OnIncomingRawDamage/OnDamageIgnored, calls UHitpointComponent::ChangeHitpoints, then on depletion broadcasts OnHealthChanged(+0x3F8), OnHealthDepleted(+0x3D8)/OnDied(+0x3E8), UGameplayLib::DamageDealtByPlayerOrPlayerFriend (HUD
  - `public: virtual void __cdecl UHealthComponent::TakeDamage(struct FDamageInfo const & DamageInfo, bool, class AController * Instigator, class AActor * Causer, struct FDamageEvent const &, struct FHitResult const &, float & OutAbsorbed, float & OutOverkill, bool & bOut)`
  - ABI: vtable slot 151. rcx=this, rdx=FDamageInfo const*, r8b=bool, r9=AController*, then 5 stack pointers.
- **UShieldComponent::TakeDamage** @ `012CEEB0` unique=True — Shield absorption; resets RemainingRechargeDelay(+0x560) / shutdown, broadcasts OnShieldChanged(+0x3C8)/OnShieldDepleted(+0x3D8).
  - `public: virtual void __cdecl UShieldComponent::TakeDamage(struct FDamageInfo const &, bool, class AController *, class AActor *, struct FDamageEvent const &, struct FHitResult const &, float &, float &, bool &)`
  - ABI: vtable slot 151.
- **UArmorComponent::TakeDamage** @ `01556B2C` unique=True — Armor absorption layer between shield and hull.
  - `public: virtual void __cdecl UArmorComponent::TakeDamage(struct FDamageInfo const &, bool, class AController *, class AActor *, struct FDamageEvent const &, struct FHitResult const &, float &, float &, bool &)`
  - ABI: vtable slot 151.
- **UHitpointComponent::ChangeHitpoints** @ `014E55C8` unique=True — The raw hitpoint mutation used by all three TakeDamage overrides. Reads MaxHitpoints (FAttributeAccess +0xF8, current value at +0x100+0x30), updates HitpointRatio(+0xB0) and BonusHitpointRatio(+0x230), accumulates RecentDamage(+0x148) and arms the recent-damage timers. Hook here on the host if you want a single 'health changed' notification independent of which component was hit.
  - `public: void __cdecl UHitpointComponent::ChangeHitpoints(float Delta, float & OUT_DamageAbsorbed, float & OUT_DamageOverkill)`
  - ABI: rcx=this, xmm1=float, r8=float*, r9=float*.
- **UHitpointComponent::SetCurrentHitpointsWithRatio** @ `01556A24` unique=True — Sets HitpointRatio(+0xB0)=clamp(Ratio,0,MaxHitpointRatio) with the remainder into BonusHitpointRatio(+0x230), and broadcasts OnHitpointsChanged(+0x350) so HUD/FX react. THE host->client health apply function; being a reflected UFunction with one float param it can be invoked generically by name.
  - `public: virtual void __cdecl UHitpointComponent::SetCurrentHitpointsWithRatio(float Ratio)`
  - ABI: vtable SLOT 153 (offset 0x4C8). rcx=this, xmm1=float. UHealthComponent overrides it at 01556948 — call through the vtable or via ProcessEvent, never by this base RVA on a health component.
- **UHealthComponent::SetCurrentHitpointsWithRatio** @ `01556948` unique=True — Health-specific ratio setter (also resets the death/depleted latches bHealthDepletedCalled +0x530 / bOnDiedCalled +0x531 semantics). This is what actually runs when the client applies a host-pushed hull ratio.
  - `public: virtual void __cdecl UHealthComponent::SetCurrentHitpointsWithRatio(float Ratio)`
  - ABI: The concrete override occupying vtable slot 153 on UHealthComponent.
- **UHitpointComponent::GetRatio** @ `012D00CC` unique=True — Returns HitpointRatio(+0xB0) + BonusHitpointRatio(+0x230) when the MaxHitpointRatio FBuffableFloat is > 0. The value the host samples and ships to clients.
  - `public: float __cdecl UHitpointComponent::GetRatio(void)`
  - ABI: rcx=this, returns float in xmm0.
- **UHitpointComponent::IsDepleted** @ `012CEACC` unique=True — Used by InputStartFirePrimary to block firing while dead; host should apply the same gate to remote fire requests.
  - `public: bool __cdecl UHitpointComponent::IsDepleted(void)`
  - ABI: rcx=this, al=bool.
- **UHitpointComponent::SetCurrentHitpoints** @ `014E6B9C` unique=True — Absolute setter; alternative to the ratio path when Max differs between machines (it should not, since ship loadout drives MaxHitpoints).
  - `public: void __cdecl UHitpointComponent::SetCurrentHitpoints(float Hitpoints)`
  - ABI: rcx=this, xmm1=float (absolute HP, not a ratio).
- **UHealthComponent::IsDead** @ `02B3F538` unique=True — Host-side kill detection to trigger a kill event on the RPC channel.
  - `public: bool __cdecl UHealthComponent::IsDead(void)`
  - ABI: rcx=this, al=bool.
- **AESPawn::TakeDamage** @ `0142AF94` unique=True — NOT the damage application point. Calls FDamageEvent::GetBestHitInfo, then APawn::TakeDamage (base bookkeeping), then purely cosmetic work: if the causer's controller's pawn is an enemy, tries to auto-lock the attacker for the HUD marker (UHUDMarkerComponent::IsLockable, UWeaponComponent::GetLockedTarget). Health has already been reduced by the time this runs.
  - `public: virtual float __cdecl AESPawn::TakeDamage(float Damage, struct FDamageEvent const &, class AController * EventInstigator, class AActor * DamageCauser)`
  - ABI: vtable SLOT 207 (offset 0x678) — the AActor::TakeDamage slot. rcx=this, xmm1=float, r8=FDamageEvent const*, r9=AController*, [RSP0+0x28]=AActor*. AActor::TakeDamage is NOT a UFUNCTION in this build (no Z_Construct_UFunction_AActor_TakeDamage).
- **AESPawn::Die_Implementation** @ `05DE2718` unique=True — Calls USelfRegisteringComponent::Unregister (AESPawn+0x340) and returns. Everything visible about a kill (explosion, wreck, destroy) is Blueprint, executed on whichever machine runs it — i.e. the host.
  - `public: virtual void __cdecl AESPawn::Die_Implementation(class AController * Killer, class APawn * KillerPawn, class AActor * DamageCauser)`
  - ABI: vtable SLOT 278. Only 0x24 bytes long. `Die` is a BlueprintNativeEvent — the substantive death behaviour lives in the Blueprint override, reachable only via ProcessEvent.
- **AESPawn::RemoveDropAndXP** @ `0193E9A0` unique=True — Disables ULootDropComponent (LootDrop +0x3A8) and UXPComponent (XP +0x3B8) on a pawn. Called by ABattleSimulator::SpawnClass and AWantedLevelManager::InitPawn. Useful if you ever need to suppress duplicate loot for spawned-per-client actors.
  - `public: void __cdecl AESPawn::RemoveDropAndXP(bool bRemoveDrop, bool bRemoveXP)`
  - ABI: rcx=this, dl=bool, r8b=bool.
- **ULootDropComponent::DropLoot** @ `016CCDC4` unique=True — Rolls the loot table with an FRandomStream, honours bOnlyDropLootWhenPlayerIsCloseOrWhenDamagedByPlayer(+0xD9) using UGameplayStatics::GetPlayerPawn(0) distance (a co-op hazard: only player 0 is considered), then UGameplayLib::SpawnPickups -> APickupBase actors, recorded in LastSpawnedPickups(+0x110), and broadcasts OnPickupsSpawned(+0x130). Reached from ULootDropComponent::OwnerHealthDepleted, bo
  - `public: void __cdecl ULootDropComponent::DropLoot(bool bForce)`
  - ABI: rcx=this, dl=bool.
- **AProjectileBase::Explode** @ `0167EFD0` unique=True — Projectile impact damage: calls UGameplayLib::ApplyESPointDamage (at +0xA9C) using the projectile's WeaponConfig(+0x320) and MyController(+0xA6C). Blocked by the same funnel hook on clients.
  - `protected: void __cdecl AProjectileBase::Explode(struct FHitResult const & Hit, bool bDirect)`
  - ABI: rcx=this, rdx=FHitResult const* (256B), r8b=bool.
- **AProjectileBase::OnImpact** @ `01680890` unique=True — Collision callback that routes into Explode / reflect handling.
  - `public: void __cdecl AProjectileBase::OnImpact(struct FHitResult const &)`
  - ABI: rcx=this, rdx=FHitResult const*.
- **UGameplayLib::IsPlayerPawn** @ `012A1810` unique=True — IsChildOf(AESPawn) && (vtable slot 254 IsPlayerControlled() || AESPawn::bIsPlayerPawn @+0x10C1). Important for co-op: the joiner's server-side pawn, once possessed by its APlayerController, returns true on the HOST, so damage attribution, XP and loot-proximity logic treat it as a player. No manual flag needed.
  - `public: static bool __cdecl UGameplayLib::IsPlayerPawn(class AActor const *)`
  - ABI: static, rcx=AActor const*.
- **UGameplayLib::DamageDealtByPlayerOrPlayerFriend** @ `015572A0` unique=True — COSMETIC ONLY — floating damage numbers (AESHUD::ShowHitpointNumbers), AESHUD::OnPlayerDealtDamage and the FOnPlayerDealtDamage delegate. Not an authoritative damage path; do not hook it expecting to gate damage.
  - `public: static void __cdecl UGameplayLib::DamageDealtByPlayerOrPlayerFriend(class UHitpointComponent const *, float, class AController *, class AActor *, class AActor *, struct FHitResult const &, bool, bool)`
  - ABI: static.
- **APlayerController::ServerRecvClientInputFrame_Implementation** @ `04E974E8` unique=True — BEST client->host binary channel. Its body writes only into APlayerController::InputBuffer_DEPRECATED (+0x6F0, a 30-slot ring of TArray<uint8>) and a counter at +0x6F0 — dead network-physics code in this build. Skipping the original in the detour leaves zero side effects. Keep payloads well under ~200 bytes (unreliable bunch limit).
  - `public: virtual void __cdecl APlayerController::ServerRecvClientInputFrame_Implementation(int RecvClientInputFrame, class TArray<unsigned char> const & Data)`
  - ABI: UFunction ServerRecvClientInputFrame: 3 props, param StructureSize 0x18 (int32 @0, TArray<uint8> @8), FunctionFlags 0x00220C42 = RequiredAPI|Net|Native|Event|Public|NetServer — UNRELIABLE, NO WithValidation. The C++ stub has no symbol, so send it with ProcessEvent(PC, "ServerRecvClientInputFrame", &
- **APlayerController::ClientPrestreamTextures_Implementation** @ `04E7DC48` unique=True — BEST host->client channel carrying an actor identity: the AActor* param is serialised as a NetGUID and resolves on the client to its own replicated copy of the same actor. Ideal for 'actor X now has hull/shield ratio Y' and for kill notifications. Original body only calls IsPrimaryPlayer + AActor vtable slot 220 (PrestreamTextures) — safe to skip entirely.
  - `public: virtual void __cdecl APlayerController::ClientPrestreamTextures_Implementation(class AActor * ForcedActor, float ForceDuration, bool bEnableStreaming, int CinematicTextureGroups)`
  - ABI: UFunction: 4 props, StructureSize 0x18 (AActor* @0, float @8, bool @12, int32 @16), FunctionFlags 0x01020CC2 = NetClient|NetReliable|Net|Native|Event|Public. rcx=this, rdx=AActor*, xmm2=float, r9b=bool, [RSP0+0x28]=int32.
- **APlayerController::ClientRecvServerAckFrame_Implementation** @ `01973F00` unique=True — Tiny (9 payload bytes) unreliable host->client channel with zero meaningful side effects. Good for high-rate scalar state (e.g. a packed 'this pawn's hull+shield ratio' for the joiner's own ship at 20 Hz).
  - `public: virtual void __cdecl APlayerController::ClientRecvServerAckFrame_Implementation(int LastProcessedInputFrame, int RecvServerFrameNumber, signed char TimeDilation)`
  - ABI: UFunction: 3 props, StructureSize 0x0C, FunctionFlags 0x01020C42 = NetClient|Net|Native|Event|Public — UNRELIABLE. Body is 4 instructions: writes +0x7FC, +0x800, +0x804 (inside ClientFrameInfo_DEPRECATED @+0x7F8) and returns.
- **APlayerController::ClientAddTextureStreamingLoc_Implementation** @ `04E7CF58` unique=True — Reliable host->client channel carrying a full-precision FVector + float + bool. Body calls IStreamingManager::AddViewLocation — a real side effect, so the client hook must NOT call the original.
  - `public: void __cdecl APlayerController::ClientAddTextureStreamingLoc_Implementation(struct UE::Math::TVector<double> InLoc, float Duration, bool bOverrideLocation)`
  - ABI: UFunction: 3 props, StructureSize 0x20, FunctionFlags 0x01820CC3 = Final|NetClient|NetReliable|HasDefaults|Net|Native|Event|Public.
- **APlayerController::ServerSetSpectatorLocation_Implementation** @ `04E97A18` unique=True — Unreliable client->host channel carrying a full-precision vector + rotator — a direct structural match for {CrosshairLocationWorld, CrosshairDirectionWorld}, if you prefer typed params over a byte blob.
  - `public: virtual void __cdecl APlayerController::ServerSetSpectatorLocation_Implementation(struct UE::Math::TVector<double> NewLoc, struct UE::Math::TRotator<double> NewRot)`
  - ABI: UFunction: 2 props, StructureSize 0x30 (FVector@0, FRotator@24), FunctionFlags 0x80A20C42 = NetValidate|HasDefaults|Net|Native|Event|Public|NetServer — UNRELIABLE, WithValidation (ServerSetSpectatorLocation_Validate @01745B60 is an ICF-shared `return true`, so it always passes).
- **APlayerController::ServerUpdateCamera_Implementation** @ `04E97D6C` unique=True — Cheap unreliable client->host vector+int channel. Lower bandwidth than the spectator one because of quantisation, but its body genuinely updates the server-side camera cache — skip the original.
  - `public: virtual void __cdecl APlayerController::ServerUpdateCamera_Implementation(struct FVector_NetQuantize CamLoc, int CamPitchAndYaw)`
  - ABI: UFunction: 2 props, StructureSize 0x20, FunctionFlags 0x80220C42 — UNRELIABLE, WithValidation. FVector_NetQuantize loses sub-unit precision.
- **APlayerController::ClientTeamMessage_Implementation** @ `04E7F77C` unique=True — Second reliable host->client string channel (distinct from ClientMessage, which the mod already uses), and it additionally carries an APlayerState* net reference so you can attribute the message to a specific player.
  - `public: virtual void __cdecl APlayerController::ClientTeamMessage_Implementation(class APlayerState * SenderPlayerState, class FString const & S, class FName Type, float MsgLifeTime)`
  - ABI: UFunction: 4 props, StructureSize 0x28, FunctionFlags 0x01020CC2 — RELIABLE NetClient.
- **APlayerController::ServerAcknowledgePossession_Implementation** @ `04E96F44` unique=True — Reliable client->host channel carrying one APawn* by NetGUID. Usable as a low-rate 'client asserts target/context actor' message; note the real body sets AcknowledgedPawn, so a detour must skip it if repurposed.
  - `public: virtual void __cdecl APlayerController::ServerAcknowledgePossession_Implementation(class APawn * P)`
  - ABI: UFunction: 1 prop, StructureSize 8, FunctionFlags 0x80220CC2 — RELIABLE NetServer WithValidation.
- **APlayerController::ServerNotifyLoadedWorld_Implementation** @ `04E973A8` unique=True — Reliable client->host FName channel. FNames replicate by string when not hardcoded, so it is really a small reliable string channel; second option next to the ServerChangeName one already in use.
  - `public: void __cdecl APlayerController::ServerNotifyLoadedWorld_Implementation(class FName WorldPackageName)`
  - ABI: UFunction: 1 prop, StructureSize 8, FunctionFlags 0x80220CC3 — RELIABLE NetServer.
- **APlayerController::ServerChangeName_Implementation** @ `04E971B4` unique=True — The channel the mod already uses. Its body calls GameMode->ChangeName, which is a real side effect — keep skipping the original. Fine for low-rate control messages; too heavy (UTF-16 FString, reliable ordering) for 30 Hz combat state.
  - `public: virtual void __cdecl APlayerController::ServerChangeName_Implementation(class FString const & S)`
  - ABI: UFunction: 1 prop, StructureSize 0x10, FunctionFlags 0x80220CC2 — RELIABLE NetServer WithValidation.
- **APlayerController::ClientMessage_Implementation** @ `04E7D54C` unique=True — The mod's existing host->client string channel. Keep for control/setup traffic; move per-actor health to ClientPrestreamTextures.
  - `public: virtual void __cdecl APlayerController::ClientMessage_Implementation(class FString const & S, class FName Type, float MsgLifeTime)`
  - ABI: UFunction: 3 props, StructureSize 0x20, FunctionFlags 0x01020CC2 — RELIABLE NetClient.
- **UActorComponent::SetIsReplicated** @ `0188F62C` unique=True — An xref scan found NO ES2 game-code callers — meaning Health/Shield/Armor/Weapon components are never replicated in this game. Any plan to replicate a component property at runtime must call this per-instance on both sides, in addition to flipping CPF_Net. This is the main reason to prefer RPCs.
  - `public: void __cdecl UActorComponent::SetIsReplicated(bool bShouldReplicate)`
  - ABI: rcx=this, dl=bool.

## Types

### UWeaponComponent (size 2512)
- 0x0230 WeaponCategory TEnumAsByte<EWeaponCategory::Type> — primary vs secondary class
- 0x0238 WeaponSlots TArray<FWeaponInfo> — the equipped-weapon list (UPROPERTY, replicable in principle but a heavy TArray of structs)
- 0x0268 bAutoSpawnWeapons bool — whether the component spawns AWeaponBase actors on init
- 0x0280 FocusPointDistance float — multiplier used to project the crosshair direction into DesiredFocusLocation
- 0x0298 DamageFactor FBuffableFloat (64B) — global damage multiplier fed into CalculateDamage
- 0x02D8 DamageFactorEnergy FBuffableFloat
- 0x0318 DamageFactorKinetic FBuffableFloat
- 0x0358 EnergyConsumptionFactor FBuffableFloat
- 0x0398 FireRateFactor FBuffableFloat — cadence; must match on both sides or the host fires at a different rate than the client's local FX
- 0x0458 SpreadFactor FBuffableFloat
- 0x05E8 OnFireStarted / 0x05F8 OnFireStopped / 0x0608 OnTriggerPulled / 0x0618 OnTriggerReleased — multicast delegates fired by StartFire/StopFire
- 0x0728 CurrentAutoAimTarget AActor* — written by TickComponent from SmoothedAutoaim
- 0x0798 CrosshairDirection FVector — set by SetDesiredFocusLocationFromDeprojectedCrosshair
- 0x07B0 FireHoldStartTimeStamp float — set on StartFire
- 0x07B4 bFireActivated bool — **THE firing state bit**; reflected UPROPERTY; read every tick by TickComponent
- 0x07C0 FireCooldowns TMap<UItem*,float> — per-weapon-item cooldowns (not reflected as a simple value; do not try to replicate)
- 0x0820 bFocusLocationNeedsUpdate bool — set by SetDesiredFocusLocationFromDeprojectedCrosshair, consumed by TickComponent
- 0x0828 DeprojectedCrosshairLocation FVector — the crosshair world position
- 0x0840 DesiredFocusLocation FVector — crosshair loc + dir*FocusPointDistance
- 0x0888 FocusLocation FVector — post-autoaim aim point; read by AWeaponBase::GetAimDirection. THE thing the host must reproduce
- 0x08C0 LockedTarget TWeakObjectPtr<AActor> — reflected but a weak ptr, so NOT net-serialisable; must go over RPC as a NetGUID
- 0x08C8 bInstancesSpawned bool
- 0x08CC CurrentChargeRatio float — NOT a reflected UPROPERTY
- 0x08D0 EquippedSlotIndex int — current weapon index; NOT a reflected UPROPERTY, so it cannot be CPF_Net'd; drive it host-side with the reflected UFunction EquipWeapon(int,bool,bool,bool) @01519154
- 0x08D8 bAllowFire FBuffableBool (64B) — StartFire's gate; manipulate with SetAllowFire
- 0x0938 RampUpCountdown float / 0x093C InstantRampDownCountdown / 0x0940 CurrentRampUpDeviation
- 0x0944 NextShotWithinBurstCountdown float — the per-tick fire-cadence timer
- 0x0948 RemainingShotsInBurst int / 0x094C PreviousRemainingShotsInBurst
- 0x0954 bLastFireActivated bool — edge-detect helper the tick uses
- 0x09A0 RemainingMissileLockTime float
- 0x09C8 bCanPlayDepletedSound bool

### UHitpointComponent (size 968)
- 0x00A8 SaveDataMapKey FName
- 0x00B0 HitpointRatio float — **THE health/shield/armor value**; reflected UPROPERTY; 0..MaxHitpointRatio; written by ChangeHitpoints and SetCurrentHitpointsWithRatio; the single float to ship per component
- 0x00B8 MaxHitpointRatio FBuffableFloat (64B) — upper clamp; current value at +0x00E8 (0xB8+0x30)
- 0x00F8 MaxHitpoints FAttributeAccess (80B) — absolute max HP; the FBuffableFloat inside is at +0x100, its cached value at +0x130
- 0x0148 RecentDamage float — drives the 'recently damaged' HUD state
- 0x0160 TotalHitpointsSum float
- 0x0168 bIsInvulnerable FBuffableBool (64B)
- 0x01A8 bIsInvulnerableButAllowRawDamage FBuffableBool
- 0x01E8 IgnoreDamageChance FBuffableFloat
- 0x0228 bIgnoresRadialDamage bool
- 0x022C MinHitpointRatio float (reflected)
- 0x0230 BonusHitpointRatio float (reflected) — added to HitpointRatio by GetRatio
- 0x0234 MaxBonusHitpointRatio float (reflected)
- 0x0238 CannotDeplete bool
- 0x0240 DamageReduction FAttributeAccess
- 0x0290 IncomingDamageIncreasePercent FBuffableFloat
- 0x0350 OnHitpointsChanged delegate — broadcast by SetCurrentHitpointsWithRatio, so a client-side ratio apply still drives the UI
- 0x0360 OnIncomingRawDamage / 0x0370 OnDamageReceived / 0x0380 OnDamageIgnored / 0x0390 OnOverkillDamageReceived / 0x03A0 OnBreak / 0x03B0 OnWouldDeplete
- 0x03C0 LastDamageInstigator AController* (reflected)

### UHealthComponent (size 1336)
- (inherits all of UHitpointComponent, so HitpointRatio is still at 0x00B0)
- 0x03C8 OnPreHealthDepleted delegate
- 0x03D8 OnHealthDepleted delegate — ULootDropComponent::OwnerHealthDepleted and UXPComponent::OwnerHealthDepleted bind here
- 0x03E8 OnDied delegate
- 0x03F8 OnHealthChanged delegate
- 0x0418 HullDamageReduction FAttributeAccess
- 0x0468 RepairRatePerSecond float
- 0x0480 DamageParts TArray<FDamagePartEntry> — ADamagePart sub-actors
- 0x0494 DamageLimit float / 0x0498 DamageThreshold float
- 0x04A0 bImmuneToItemDamage FBuffableBool
- 0x04E0 bDontAutomaticallyBroadcastOnDied bool
- 0x04F0 RemainingHitpointsToRepair float
- 0x04F8 LastDamageCauser AActor* (reflected)
- 0x0504 RemainingInvincibiliyTime float / 0x0508 InvincibilityDuration float
- 0x0520 HullRepairQueue TArray<FHullRepair>
- 0x0530 bHealthDepletedCalled bool — the death latch
- 0x0531 bOnDiedCalled bool — the second death latch; a client that only receives ratios will never set these, which is why kills must ride the RPC channel or rely on actor destruction

### UShieldComponent (size 1384)
- (inherits UHitpointComponent; shield charge is HitpointRatio at 0x00B0)
- 0x03C8 OnShieldChanged delegate
- 0x03D8 OnShieldDepleted delegate
- 0x03E8 OnShieldCharging delegate
- 0x03F8 RechargeSpeed FAttributeAccess
- 0x0448 RechargeDelayAfterHit FAttributeAccess
- 0x0498 ShutDownDuration FAttributeAccess
- 0x04E8 InstantShieldRechargeAfterShutdown FAttributeAccess
- 0x0538 DamageLimit float
- 0x0558 RemainingShutdownTime float
- 0x055C RemainingDisruptTime float
- 0x0560 RemainingRechargeDelay float — reset by the damage funnel via UShieldComponent::ResetShutDownTime; on a client this ticks independently and drifts, so periodic host pushes are required

### UArmorComponent (size 1008)
- (inherits UHitpointComponent with no new fields; armor value is HitpointRatio at 0x00B0)

### AESPawn (size 4912)
- 0x0338 CollisionRoot UMovementRootComponent*
- 0x0340 SelfRegistering USelfRegisteringComponent* — unregistered by Die_Implementation
- 0x0348 Savegame USaveGameComponent*
- 0x0350 Health UHealthComponent* — read by InputStartFirePrimary for the IsDepleted gate
- 0x0358 Armor UArmorComponent*
- 0x0360 Shield UShieldComponent*
- 0x0368 Faction UFactionComponent*
- 0x0370 EnergyCore UEnergyCoreComponent*
- 0x0378 ShipMovement UShipMovementComponent*
- 0x0380 PrimaryWeapons UWeaponComponent* — StartFire/StopFire/SetDesiredFocusLocation target
- 0x0388 SecondaryWeapons UWeaponComponent*
- 0x0390 Devices UDeviceComponent* / 0x0398 Consumables UConsumableComponent*
- 0x03A0 HUDMarker UHUDMarkerComponent*
- 0x03A8 LootDrop ULootDropComponent* — loot on death
- 0x03B0 JumpDrive UJumpDriveComponent* — cruise/charge gates on firing
- 0x03B8 XP UXPComponent*
- 0x0890 PawnType TEnumAsByte<EPawnType::Type>
- 0x10C1 bIsPlayerPawn bool (reflected) — fallback used by UGameplayLib::IsPlayerPawn when the pawn is not player-controlled

### AESPlayerController (size 0)
- 0x0897 bIsMovementBlocked bool — fire gate
- 0x08C0 CrosshairWorldVector FVector
- 0x0A40 ESPawn AESPawn* — the controlled ship (cached, reflected)
- 0x0A70 CrosshairLocationWorld FVector — **client aim payload part 1**, written every ProcessPlayerInput
- 0x0A88 CrosshairDirectionWorld FVector — **client aim payload part 2**
- 0x0AD0 PlayerCameraWorldRotation FRotator / 0x0B00 PlayerCameraWorldLocation FVector / 0x0B18 PlayerCameraWorldDirection FVector
- 0x0B33 bSelectingDevicesOrConsumables bool — fire gate
- 0x0B53 bBlockNextPrimary bool / 0x0B78 bBlockNextSecondary bool

### APlayerController (engine, relevant offsets only) (size 0)
- 0x06F0 InputBuffer_DEPRECATED APlayerController::FInputCmdBuffer (264B) — the ONLY thing ServerRecvClientInputFrame_Implementation touches; dead code in this build
- 0x07F8 ClientFrameInfo_DEPRECATED APlayerController::FClientFrameInfo (20B) — the only thing ClientRecvServerAckFrame_Implementation touches (writes +0x7FC, +0x800, +0x804)
- 0x0848 NetworkPhysicsTickOffset int
- 0x084C bNetworkPhysicsTickOffsetAssigned bool

### AWeaponBase (size 3104)
- 0x02C0 ProjectileClass TSubclassOf<AProjectileBase>
- 0x02F8 WeaponConfig FWeaponData (1808B) — all damage/firerate/spread/range attributes for this weapon instance
- 0x0A10 OnWeaponHit delegate
- 0x0AE8 LastFiredProjectile AProjectileBase*
- 0x0AF0 WeaponComponent UWeaponComponent* — back-pointer used by GetAimDirection to read FocusLocation
- 0x0AF8 ProjectilePool UActorPool*
- 0x0B84 bIsCharging bool
- 0x0B88 LastFireTime float
- 0x0BE4 BurstCounter int
- 0x0BF0 ReflectCount int / 0x0BF4 MaxReflect int
- 0x0C04 CurrentEnergy float / 0x0C08 CurrentOverchargeEnergy float / 0x0C0C CurrentRechargeDelay float
- 0x0C10 LastShotChargePercent float

### FDamageInfo (size 12)
- 0x0000 Energy float — energy-type damage
- 0x0004 Kinetic float — kinetic-type damage
- 0x0008 bIsChainReaction bool
- 0x0009 bIsShieldPiercing bool
- 0x000A bIsArmorPiercing bool
- (this is both the input and the sret output of the damage funnel; a client-side block writes 12 zero bytes to the out slot)

### FWeaponData (size 1808)
- 0x0000 WeaponCategory TEnumAsByte<EWeaponCategory::Type>
- 0x0008 FireRate FAttributeAccess (80B)
- 0x0148 HullDamage FAttributeAccess — the hull damage number
- 0x0198 ShieldDamage FAttributeAccess — the shield damage number
- 0x01E8 Range FAttributeAccess — used by the hitscan trace length
- 0x0238 Speed FAttributeAccess — projectile speed
- 0x0288 SpreadDegrees FAttributeAccess — the VRandCone half-angle; unless client and host agree on the RNG they will disagree on individual shot directions, which is exactly why hit reports should not be trusted
- 0x06D0 WeaponSeed int — the per-weapon random seed; if you ever DO want deterministic shared shots, this is the value both sides need
- 0x06D8 WeaponItem UItem*
- 0x06FC OwnerLevel int
- 0x0704 ShieldPiercingRatio float / 0x0708 ArmorPiercingRatio float

### FHitResult (size 256)
- 0x0010 Location FVector_NetQuantize
- 0x0028 ImpactPoint FVector_NetQuantize
- 0x0040 Normal FVector_NetQuantizeNormal
- 0x0058 ImpactNormal FVector_NetQuantizeNormal
- 0x0070 TraceStart FVector_NetQuantize / 0x0088 TraceEnd FVector_NetQuantize
- 0x00AD bBlockingHit (bit0) / bStartPenetrating (bit1)
- 0x00B8 HitObjectHandle FActorInstanceHandle (32B) — resolve the actor with FActorInstanceHandle::FetchActor
- 0x00D8 Component TWeakObjectPtr<UPrimitiveComponent>
- (needed only if you build a synthetic hit for a host-side ApplyESPointDamage call; the recommended design avoids this entirely)

### FBuffableFloat (size 64)
- 0x0000 BaseValue float
- 0x0020 Modifiers TArray<FModifier>
- 0x0030 CurrentValue float — the value all the disassembly reads after RefreshValue
- 0x0034 bIsDirty bool
- 0x0038 BaseValueAtRefresh float
- (read pattern seen everywhere: if (bIsDirty || BaseValueAtRefresh != BaseValue) RefreshValue(); then use +0x30)

### ULootDropComponent (size 328)
- 0x00A4 LootID FName
- 0x00B0 FixedLootEntries TArray<FFixedLootEntry>
- 0x00D9 bOnlyDropLootWhenPlayerIsCloseOrWhenDamagedByPlayer bool — DropLoot evaluates this against UGameplayStatics::GetPlayerPawn(0) only, so a kill made only near player 1 may drop nothing
- 0x0110 LastSpawnedPickups TArray<APickupBase*>
- 0x0120 LootToDrop TArray<FPickupEntry>
- 0x0130 OnPickupsSpawned delegate
- 0x0140 bEnableLootDrop bool
- 0x0141 bLootDropped bool

### UXPComponent (size 176)
- 0x00A0 XP float
- 0x00A4 GainCondition TEnumAsByte<EGainCondition::Type>
- 0x00A8 NeededPlayerDamageRatio float — XP is awarded only if the player did this share of the damage; in co-op the two players' contributions are tracked separately, so shared kills may award nobody


## Hook plan

- **ES_ApplyDamage_Internal (unnamed damage funnel)** @ `012D0210` [client] — Make combat host-authoritative in one stroke: stop the client from ever changing anyone's hitpoints (its own hitscan, its own projectiles, replicated NPCs firing locally, radial/explosion damage, scripted damage).
  - behaviour: Block. Detour installed unconditionally but active only when the world's NetMode == NM_Client. When active: memset(rcx, 0, 12) to zero the sret FDamageInfo (Energy, Kinetic, 3 bools), and if the bool* at [rsp+0x40] is non-null set *it = false; then return without calling the original. When inactive (host / single player), tail into the trampoline unchanged. Signature to declare: void(__fastcall*)(FDamageInfo* out, AActor* target, void* dmgEvent, void* hit, const FDamageInfo* in, const FVector* dir, AController* instigator, AActor* causer, bool* bOutCrit, const FWeaponData* cfg, float shieldPierce, float armorPierce, UClass* hpCompClass, bool bAllowPlayerCrit, bool bFlag).
  - risk: Medium-low. The RVA has NO symbol, so gen_sdk must carry it as a hand-added address and the PE-timestamp guard becomes load-bearing (a game patch moves it silently). It is verifiably a real function entry (16-byte aligned, full MSVC prologue) and verifiably the only funnel (5 call sites found by rel32 scan). Zeroing the sret is mandatory — ApplyESRadialDamage reads *out immediately after the call, so returning without writing it leaks stack garbage. Safer variant if you want a named target: hook the four public wrappers (01429694, 01429804, 016EE520, 05E20A58) instead — four hooks, all uniquely named, same coverage, but each needs its own sret handling.
- **AESPlayerController::ProcessPlayerInput** @ `01380AF0` [client] — Sample the local player's aim and fire state once per frame, at exactly the point the game itself has just computed them, and push them to the host.
  - behaviour: Observe (call original first, then read). After the trampoline returns: read this->CrosshairLocationWorld (+0xA70, 3 doubles) and this->CrosshairDirectionWorld (+0xA88, 3 doubles), plus this->ESPawn(+0xA40)->PrimaryWeapons(+0x380)->bFireActivated(+0x7B4) and ->SecondaryWeapons(+0x388)->bFireActivated. Resolve PrimaryWeapons->LockedTarget(+0x8C0) to a NetGUID through the NetDriver's GuidCache. Rate-limit to 30–60 Hz and to change-or-heartbeat, pack into ~32 bytes {u16 seq, u8 flags, u8 pad, float3 crosshairLoc (relative to the pawn origin), float3 crosshairDir, u32 targetGuid} and send via ProcessEvent(PC, "ServerRecvClientInputFrame", {seq, TArray<uint8>}).
  - risk: Low. Read-only, post-call, on a function that already runs every frame on the owning client only. Watch the FVector 24-byte (double) layout — do not assume floats when reading +0xA70/+0xA88; convert to float only for the wire.
- **APlayerController::ServerRecvClientInputFrame_Implementation** @ `04E974E8` [host] — Receive the client's fire/aim intent as raw bytes without touching any live engine state.
  - behaviour: Intercept and replace. Do NOT call the original (its only effect is writing into InputBuffer_DEPRECATED @+0x6F0). Decode the payload, drop out-of-order sequences, then on the AESPlayerController's ESPawn(+0xA40): (1) call UWeaponComponent::SetDesiredFocusLocationFromDeprojectedCrosshair @012D8788 on PrimaryWeapons(+0x380) and SecondaryWeapons(+0x388) with the two reconstructed FVectors (pass BY POINTER); (2) resolve the target GUID and call the reflected UFunction SetLockedTarget; (3) re-validate exactly as InputStartFirePrimary does — Health(+0x350)->IsDepleted() false, JumpDrive(+0x3B0)->bIsInCruiseMode(+0x318) false, ->bOwnerIsChargingJump(+0x3A0) false — and on a rising/falling edge of each fire flag call UWeaponComponent::StartFire @0150FE78 / StopFire @0171760C (or the same-named reflected UFunctions via ProcessEvent). Nothing else: the host's own UWeaponComponent::TickComponent then performs the trace, the spread roll, the ammo/energy spend and the damage with full authority.
  - risk: Low. Unique RVA, unreliable transport so packet loss is expected and harmless (state is level-triggered, so send the fire flags as absolute state plus a heartbeat, never as edges-only). Keep the byte array small (< ~200B) so the unreliable bunch is never dropped for size. Add a staleness timeout that calls StopFire if no packet arrives for ~0.5 s, so a disconnecting client cannot leave its guns welded on.
- **ES_ApplyDamage_Internal (same address, host-side pass-through)** @ `012D0210` [host] — Detect every authoritative health change and schedule a push to clients, without polling every actor.
  - behaviour: Observe. Call the original first, then look at the target actor (rdx): if it is an AESPawn, read Health(+0x350)->HitpointRatio(+0xB0), Shield(+0x360)->HitpointRatio, Armor(+0x358)->HitpointRatio; if any changed by more than a small epsilon since the last send, enqueue a dirty entry. Flush the queue at ~10 Hz per actor (coalesced, most-recently-damaged first) over ClientPrestreamTextures. Also read Health->bHealthDepletedCalled(+0x530) / call UHealthComponent::IsDead @02B3F538 to emit a one-shot 'killed' event.
  - risk: Low-medium. Same unnamed-RVA caveat as the client block; use the same hook object with a role check so there is only one detour on the address. Do not do work inside the hook beyond reading floats and pushing to a queue — this is called for every projectile impact and beam tick.
- **APlayerController::ClientPrestreamTextures_Implementation** @ `04E7DC48` [client] — Receive per-actor authoritative health/shield/kill updates carrying a resolved actor reference.
  - behaviour: Intercept and replace. Do NOT call the original (it would kick off texture prestreaming). Params: rdx = AActor* (already resolved from the NetGUID to the client's own replicated copy), xmm2 = float ratio, r9b = bool (0 = hull, 1 = shield), [RSP0+0x28] = int32 packed extras (e.g. low bit = 'this actor just died', next bits = armor ratio quantised). Apply by calling the reflected UFunction SetCurrentHitpointsWithRatio on the actor's Health(+0x350) / Shield(+0x360) / Armor(+0x358) via ProcessEvent (never by the base RVA 01556A24 — UHealthComponent overrides vtable slot 153 at 01556948). That path broadcasts OnHitpointsChanged(+0x350) so the HUD and shield FX follow.
  - risk: Low. Unique RVA, reliable ordered transport. If the AActor* arrives null the client has not yet received that actor's channel — drop the update silently rather than dereferencing. Note the client will not run OnHealthChanged / OnDamageReceived, so hit-flash FX will be missing until you add a separate cosmetic event.
- **APlayerController::ClientRecvServerAckFrame_Implementation** @ `01973F00` [client] — Optional high-rate, low-cost host→client scalar channel for the joiner's OWN ship (hull + shield ratio at 20 Hz), where reliability is unnecessary because the next packet supersedes it.
  - behaviour: Intercept and replace. Do NOT call the original (it writes ClientFrameInfo_DEPRECATED). Params: edx = int32 (pack hull ratio as fixed-point), r8d = int32 (shield ratio + flags), r9b = int8 (message type tag). Apply the same SetCurrentHitpointsWithRatio path as above to the local pawn's components.
  - risk: Very low. Nine payload bytes, unreliable, dead-code body. Only downside is the tiny payload — use it for the local ship only and leave the actor-addressed channel for everyone else.
- **UWeaponComponent::TickComponent** @ `012A2368` [client] — Optional: stop client-side simulated-proxy NPCs from burning CPU spawning cosmetic projectiles for weapons the host is already authoritative over, and prevent double FX once you start replicating bFireActivated.
  - behaviour: Guard. On the client, early-out (skip the original) when the owning actor's Role is ROLE_SimulatedProxy AND the owner is not the local player pawn — or, less invasively, leave the tick alone and instead call UWeaponComponent::SetAllowFire(false) @017174E8 once on those components, which suppresses shooting through the game's own modifier system.
  - risk: Medium. TickComponent also drives autoaim, missile lock, energy regeneration and weapon orientation; blanket-skipping it on simulated proxies will freeze their weapon meshes. Prefer the SetAllowFire route, and only reach for the tick guard if profiling shows it matters.
- **AESPlayerController::InputStartFirePrimary / InputStopFirePrimary / InputStartFireSecondary / InputStopFireSecondary** @ `017177A4` [client] — Alternative/またはadditional edge-accurate capture of fire intent if the per-frame ProcessPlayerInput sampling proves too lossy (the other three are 017173D4, 01715CC8, 01715EC4 — all unique).
  - behaviour: Observe. Call the original, then immediately enqueue a fire-state packet so the transition reaches the host with minimum latency, on top of the periodic state heartbeat.
  - risk: Low. These are client-only input handlers and are never invoked on the host for a remote player. Do not block them — the local pawn's cosmetic firing is what makes the client feel responsive.

## Open questions

- Does APickupBase have bReplicates set in its Blueprint defaults? ULootDropComponent::DropLoot spawns pickups host-side; whether the joiner sees them depends entirely on a BP class default that is not in the PDB. Check live with the console (dump bReplicates on a spawned pickup). If false, either flip it on the CDO before spawn or spawn a client-side proxy over the RPC channel.
- ULootDropComponent::DropLoot gates on bOnlyDropLootWhenPlayerIsCloseOrWhenDamagedByPlayer (+0xD9) using UGameplayStatics::GetPlayerPawn(0) distance only. A kill made far from player 1 but next to player 2 may drop nothing. Needs either a hook that widens the check to all player pawns, or clearing that bool on the component.
- UXPComponent::NeededPlayerDamageRatio (+0xA8) means XP is awarded only when 'the player' dealt a sufficient share of the damage — with two players sharing a kill, neither may qualify. Determine how damage share is tracked (UHitpointComponent::SaveDamageDealtInFactionComponent @0155A060 and TrackDPS @01559FE4 look relevant) and whether contributions need to be pooled.
- Does the joiner's server-side BP_Ship_Player_C actually have weapons spawned (UWeaponComponent::bInstancesSpawned +0x8C8 true, WeaponSlots +0x238 non-empty)? The existing notes say it gets a default loadout, but firing on the host is a no-op if GetFirstEquippedWeaponInstance returns null. Verify live before wiring the fire channel.
- Whether any Blueprint class subclasses UHealthComponent / UShieldComponent / UWeaponComponent. The PDB shows only the three native UHitpointComponent children, but BP-generated classes are runtime-only. This matters if you ever do go the CPF_Net route, since every already-linked child class needs SetUpRuntimeReplicationData re-run.
- Exact unreliable-RPC payload ceiling in this build (MaxPacket / MAX_SINGLE_BUNCH_SIZE_BITS as configured by the IpNetDriver). Measure empirically before growing the ServerRecvClientInputFrame byte array beyond ~64 bytes.
- Whether the host's UWeaponComponent tick behaves identically for a pawn that is player-controlled but is NOT UGameplayStatics::GetPlayerPawn(0). TickComponent calls IsGamepadModeEnabled and GetPlayerData (both global singletons keyed to player 1) for autoaim parameter selection — the joiner may get player 1's gamepad/mouse autoaim profile. Cosmetic for damage, but it changes where shots actually land, so worth measuring.
- AESPawn::Die and AESPawn::OnHealthDepleted are Blueprint events; what the BP override actually does on death (DestroyActor vs. hide-and-pool vs. spawn a wreck actor) determines whether native actor-destruction replication is sufficient for the client to see a kill. Needs a live ProcessEvent trace or a BP dump.
- Whether ADamagePart sub-actors (UHealthComponent::DamageParts +0x480) need their own state sync — they are spawned per-pawn on damage thresholds and would otherwise differ per machine.
- Confirm at runtime that ProcessEvent on a FUNC_Net UFunction from the injected DLL takes the CallRemoteFunction path rather than executing locally (it should, given GetFunctionCallspace, but the existing mod reaches the wire through the generated stubs for ServerChangeName — this design calls ServerRecvClientInputFrame purely by name because its stub has no symbol).
