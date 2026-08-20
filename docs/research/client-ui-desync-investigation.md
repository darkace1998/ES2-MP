# Client HUD desync — investigation results (2026-08-20)

Generated from the es2-client-ui-desync workflow. Findings were produced against the PDB and,
for devices/consumables, validated live on a host+client session.


## Finding (confidence: high )

### Root cause

The client's hull/shield/armour bars never move because the HP sync is dead code on the client and, even if it ran, it would not refresh the UI.

Three independent defects stack up in mod/src/combat.cpp:

1. DEAD CODE (the dominant cause). combat.cpp:225 does
     `if (p && !p->local && p->pawn) SetHealthRatio(p->pawn, hp);`
   On a CLIENT the players registry contains ONLY the local player: coop.cpp:257 calls
   `players::RegisterLocalAs(me, id)` on WELCOME, and `players::RegisterController` is only ever
   called on the host (coop.cpp:305/323 guarded by `r == Role::Host`, and coop.cpp:440 `OnLogin`
   is a host-side GameMode hook). So `players::ById(id)` on a client returns either nullptr
   (for the host's id 0, whose slot is empty) or the local player — which `!p->local` then skips.
   The "HP" handler therefore does literally nothing on a client today. This is why the client's own
   bars never drop AND why a partner's condition is never reflected either.

2. WRONG WRITE MECHANISM. `SetHealthRatio` pokes `UHitpointComponent::HitpointRatio` (+0xB0)
   directly. The ES2 HUD is Blueprint and event-driven: it refreshes off the multicast delegates
   that the engine fires from `UHitpointComponent::SetCurrentHitpointsWithRatio`
   (`OnHitpointsChanged` @ +0x350) and, for hull, `UHealthComponent::OnHealthChanged` @ +0x3F8.
   A raw field write fires nothing, so the widget keeps its cached value — exactly the fact
   respawn.cpp:102-103 already records.

3. INCOMPLETE PROTOCOL. The "HP|id|health|shield" message carries a shield value that is parsed but
   never applied, and armour is not in the protocol at all even though it is a separate component
   (`AESPawn::Armor`, UArmorComponent). The host-side `GetShieldRatio` also reads the raw ratio
   field rather than `GetRatio()`, so it silently drops `BonusHitpointRatio` (over-shield).

Additional aggravating factor: the client's own ship never takes damage locally
(UGameplayLib::ApplyESPointDamage is no-oped in the client role, combat.cpp:174-183) and ES2
replicates no hitpoint state, so the only possible source of truth on the client is this message.

Mechanically the fix is exact and cheap because the vanilla damage path itself ends in the very
function we need to call:
  UShieldComponent/UArmorComponent/UHealthComponent::TakeDamage
    -> UHitpointComponent::ChangeHitpoints (0x14E55C8)
    -> UHitpointComponent::SetCurrentHitpoints (0x14E6B9C)   [computes hp/MaxHitpoints]
    -> virtual slot 153 (vtable +0x4C8) SetCurrentHitpointsWithRatio
Calling SetCurrentHitpointsWithRatio(ratio) on the client reproduces the identical state
transition and the identical delegate broadcasts as real damage, and — importantly — it does NOT
fire any depletion/death delegate (OnWouldDeplete/OnBreak/OnHealthDepleted/OnDied live in
ChangeHitpoints/TakeDamage, not in the setter), so driving the client's hull to 0.0 will not kick
off the single-player game-over flow. The host stays the sole authority on death/respawn.

### Evidence

- CLASS HIERARCHY (answers (a)/(c)): `python3 tools/pdb_types.py layout sdk/raw/types.txt UHealthComponent|UShieldComponent|UArmorComponent` — all three carry the identical UHitpointComponent prefix (SaveDataMapKey 0xA8, HitpointRatio 0xB0, ... OnHitpointsChanged 0x350, LastDamageInstigator 0x3C0) and then diverge at 0x3C8. ES2 DOES have a single hitpoint base component: UHitpointComponent (sdk/uclasses.txt line 1687). UHealthComponent/UShieldComponent/UArmorComponent all derive from it.
- SETTER IS ONE VIRTUAL, SLOT 153: `python3 tools/pdb_types.py vtable sdk/raw/types.txt UHitpointComponent` -> `[153] SetCurrentHitpointsWithRatio  void (float)` (and `[151] TakeDamage`). 153*8 = 0x4C8, which matches `call qword ptr [rax + 0x4c8]` at both 0x14E6C13 (UHitpointComponent::SetCurrentHitpoints tail-jmp `jmp r10`, r10 = [vtable+0x4c8]) and 0x1956308 (the exec stub).
- RVAs, each verified UNIQUE (not ICF-folded) with `awk -F'\t' '$1==RVA{c++}' sdk/symbols.tsv`: UHitpointComponent::SetCurrentHitpointsWithRatio = 0x1556A24 (1 symbol); UHealthComponent::SetCurrentHitpointsWithRatio = 0x1556948 (1); UArmorComponent::SetCurrentHitpointsWithRatio = 0x1556858 (1); UHitpointComponent::SetCurrentHitpoints = 0x14E6B9C (1); UHitpointComponent::ChangeHitpoints = 0x14E55C8 (1); UHitpointComponent::GetRatio = 0x12D00CC (1); UHitpointComponent::GetCurrentHitpoints = 0x12CEA7C (1); UHitpointComponent::GetMaxHitpoints = 0x2B3AF6C (1); UHitpointComponent::SetMaxHitpoints = 0x16FBDF8 (1).
- UShieldComponent has NO SetCurrentHitpointsWithRatio override — `grep -E '^UShieldComponent::' sdk/es2_functions.txt` lists TakeDamage/TickRegeneration/ForceRecharge/ShutDown/... but no SetCurrentHitpointsWithRatio. Only UHealthComponent and UArmorComponent override it. Shield therefore uses the base at 0x1556A24.
- BASE SETTER SEMANTICS (disasm 0x1556A24): oldTotal(xmm6) = [this+0xB0] + [this+0x230]; new HitpointRatio = max(0, min(MaxHitpointRatio, arg)) stored at [this+0xB0]; BonusHitpointRatio = clamp(arg-1.0, 0, MaxBonusHitpointRatio[+0x234]) stored at [this+0x230]; `ucomiss xmm4, xmm6 / je` — if the total did not change it returns WITHOUT broadcasting; otherwise it builds a 32-byte parms block {OwnerPrivate(+0x90), this, MaxHitpoints*delta, deltaRatio, previousRatio} and calls TMulticastScriptDelegate::ProcessMulticastDelegate on `lea rcx,[r9+0x350]` = OnHitpointsChanged.
- HULL OVERRIDE (disasm 0x1556948): saves old [rbx+0xB0], calls the base, and if HitpointRatio changed builds {Actor=OwnerPrivate, HitpointDelta = MaxHitpoints*new - MaxHitpoints*old, HitDirection=0,0,0} and broadcasts `lea rcx,[rbx+0x3f8]` = UHealthComponent::OnHealthChanged. No death/depletion delegate is touched.
- ARMOUR OVERRIDE (disasm 0x1556858): calls the base, then `UGameplayLib::IsPlayerPawn(OwnerPrivate)`; if true and MaxHitpoints>0 it calls `UGameplayLib::GetPlayerData()` and writes `Ships[CurrentShip].ArmorRatio = GetRatio()` (`imul rcx, rax, 0x3d0` with FShipData sizeof=976=0x3D0, `[rcx+rdx+0x364]`; confirmed FShipData.ArmorRatio @ 0x364, UPlayerData.Ships @ 0x468, UPlayerData.CurrentShip @ 0x119C). It does NOT broadcast OnArmorChanged.
- IsPlayerPawn (disasm 0x12A1810) = IsA(AESPawn) && (virtual [vtable+0x7F0] || [actor+0x10C1]); slot 0x7F0/8 = 254 = APawn::IsPlayerControlled (`pdb_types.py vtable APawn` -> `[254] IsPlayerControlled`), and AESPawn+0x10C1 = bIsPlayerPawn. A co-op partner's pawn on a client is player-controlled, so calling the ARMOUR override on a partner's pawn would overwrite the LOCAL player's saved PlayerData ArmorRatio.
- PROOF THE HUD IS DRIVEN BY OnHitpointsChanged (not by OnArmorChanged, and not by polling): `UArmorComponent::TickComponent` (0x1557748) — armour regeneration — ends in `call qword ptr [rax + 0x4c8]` (the virtual setter) and never touches +0x3C8/OnArmorChanged. `python3 tools/disasm.py 0x01556B2C 2400 | grep 0x3c8` shows OnArmorChanged is broadcast only from UArmorComponent::TakeDamage. Since the armour bar visibly refills during regen in vanilla, the bar must be fed by OnHitpointsChanged (or OnHealthChanged for hull) — i.e. by exactly what SetCurrentHitpointsWithRatio broadcasts. Combined with respawn.cpp:102-103's already-proven note, a raw HitpointRatio write cannot be enough and the setter is.
- GETTERS (answer to (d)): `GetRatio` (0x12D00CC) = HitpointRatio(+0xB0) + BonusHitpointRatio(+0x230), returning 0 when MaxHitpoints<=0. `GetCurrentHitpoints` (0x12CEA7C) = GetRatio() * MaxHitpoints. `GetMaxHitpoints` (0x2B3AF6C) is a plain FBuffableFloat read. Because SetCurrentHitpointsWithRatio(r) is the exact inverse of GetRatio() for r in [0, MaxHitpointRatio+MaxBonusHitpointRatio], a single 0..1(+bonus) float round-trips losslessly and is independent of any MaxHitpoints mismatch between host and client. Absolute hitpoints are NOT needed for correctness.
- ARMOUR IS A REAL SEPARATE COMPONENT (answer to (e)): sdk/uclasses.txt line 497 = UArmorComponent; AESPawn layout gives direct component pointers — Health @ 0x350 (UHealthComponent*), Armor @ 0x358 (UArmorComponent*), Shield @ 0x360 (UShieldComponent*). These replace combat.cpp's slow reflected-property scan (FindComponentOfClass at combat.cpp:44).
- DELEGATE FIELD OFFSETS: UHitpointComponent::OnHitpointsChanged @ 0x350 (FOnHitpointsChangedDelegate: {AActor* Actor; UHitpointComponent* HitpointComponent; float DeltaHitpoints; float DeltaRatio; float PreviousRatio} sizeof 32). UHealthComponent::OnHealthChanged @ 0x3F8, OnPreHealthDepleted 0x3C8, OnHealthDepleted 0x3D8, OnDied 0x3E8. UShieldComponent::OnShieldChanged @ 0x3C8, OnShieldDepleted 0x3D8, OnShieldCharging 0x3E8. UArmorComponent::OnArmorChanged @ 0x3C8, OnArmorDepleted 0x3D8.
- CannotDeplete is on the BASE at +0x238 (so combat.cpp's CmdGod using es2off::UHealthComponent::CannotDeplete against all three components is accidentally correct — the field is inherited).
- TRANSPORT: coop::SendToClient uses APlayerController::ClientMessage (a RELIABLE client RPC, coop.cpp:124-128); the receive hook is APlayerController::ClientMessage_Implementation (net.cpp:150, installed net.cpp:301), which runs on the game thread during net dispatch, so calling UFunctions / engine setters from OnClientOp is safe.
- The client's pawn is the replicated proxy of the HOST's server-side pawn, not a locally spawned actor — loadout.cpp:368-369 / 387-388 take `pc->Pawn` and only fill its ShipData and build weapons locally. So the host and client are looking at two copies of the SAME replicated actor, and the host's copy is the one that actually takes damage.

### Proposed fix

CONCRETE FIX FOR mod/src/combat.cpp (three parts: helpers, host tick, client receipt).

--- 1. Replace the component helpers (delete FindComponentOfClass / GetHealthRatio / SetHealthRatio /
       GetShieldRatio at combat.cpp:42-70) with direct AESPawn offsets + engine calls:

    using Fn_HpGet = float (*)(ue::UObject* hitpointComponent);
    using Fn_HpSet = void  (*)(ue::UObject* hitpointComponent, float ratio);

    static UObject* HullOf  (AActor* p){ return p ? UE_FIELD(UObject*, p, es2off::AESPawn::Health) : nullptr; }
    static UObject* ArmorOf (AActor* p){ return p ? UE_FIELD(UObject*, p, es2off::AESPawn::Armor)  : nullptr; }
    static UObject* ShieldOf(AActor* p){ return p ? UE_FIELD(UObject*, p, es2off::AESPawn::Shield) : nullptr; }

    // GetRatio() == HitpointRatio + BonusHitpointRatio, i.e. it includes over-shield; the raw field does not.
    static float RatioOf(UObject* c) {
        if (!c || !IsValidObject(c)) return -1.f;
        return Rva<std::remove_pointer_t<Fn_HpGet>>(es2rva::UHitpointComponent_GetRatio)(c);
    }

    // Set through the engine's own setter, never the field: this IS the function the vanilla damage path
    // ends in (TakeDamage -> ChangeHitpoints -> SetCurrentHitpoints -> virtual SetCurrentHitpointsWithRatio),
    // and it is what broadcasts OnHitpointsChanged / OnHealthChanged, which is what the HUD listens to.
    // It never fires a depletion/death delegate, so ratio 0 will not start the game-over flow on a client.
    static void SetRatio(UObject* c, uint32_t rva, float want) {
        if (!c || !IsValidObject(c) || want < 0.f) return;
        float have = RatioOf(c);
        if (have >= 0.f && fabsf(have - want) < 0.0005f) return;   // engine no-ops anyway; skip the call
        Rva<std::remove_pointer_t<Fn_HpSet>>(rva)(c, want);
    }

    // `ownShip` selects UArmorComponent's override, which also writes
    // PlayerData.Ships[CurrentShip].ArmorRatio — right for our own ship, corrupting for a partner's,
    // so a partner's armour goes through the UHitpointComponent base instead.
    static void ApplyHitpoints(AActor* pawn, bool ownShip, float hull, float shield, float armor) {
        if (!pawn || !IsValidObject((UObject*)pawn)) return;
        SetRatio(HullOf(pawn),   es2rva::UHealthComponent_SetCurrentHitpointsWithRatio,   hull);
        SetRatio(ShieldOf(pawn), es2rva::UHitpointComponent_SetCurrentHitpointsWithRatio, shield); // no override
        SetRatio(ArmorOf(pawn),  ownShip ? es2rva::UArmorComponent_SetCurrentHitpointsWithRatio
                                         : es2rva::UHitpointComponent_SetCurrentHitpointsWithRatio, armor);
    }

--- 2. Host tick (replace combat.cpp:232-243). Send all three ratios, only when they move:

    struct HpSnap { float hull = -2, shield = -2, armor = -2; double lastSent = -1e9; };
    static std::map<int, HpSnap> g_hpSnap;
    static double g_hpNow = 0;
    static float  g_healthHz = 10.f;               // was 4; change-gated below so idle traffic is lower

    void Tick(float dt, bool isHost) {
        g_hpNow += dt;
        if (!isHost) return;
        g_healthAccum += dt;
        if (g_healthAccum < 1.0 / g_healthHz) return;
        g_healthAccum = 0;
        for (auto* p : players::All()) {
            if (!p->pawn) continue;
            float hull = RatioOf(HullOf(p->pawn));
            if (hull < 0) continue;
            float shield = RatioOf(ShieldOf(p->pawn));      // -1 when the ship has no shield
            float armor  = RatioOf(ArmorOf(p->pawn));
            HpSnap& s = g_hpSnap[p->id];
            bool moved = fabsf(hull - s.hull) > 0.002f ||
                         fabsf(shield - s.shield) > 0.002f ||
                         fabsf(armor  - s.armor)  > 0.002f;
            if (!moved && g_hpNow - s.lastSent < 2.0) continue;   // keepalive every 2s, else silence
            s = HpSnap{hull, shield, armor, g_hpNow};
            coop::SendToAllClients(Format("HP|%d|%.4f|%.4f|%.4f", p->id, hull, shield, armor));
        }
    }

--- 3. Client receipt (replace combat.cpp:220-227). The load-bearing change is dropping `!p->local`:

    if (op == "HP") {
        // HP|<playerId>|<hullRatio>|<shieldRatio>|<armorRatio>   (-1 = component absent)
        int id = 0; float hull = -1, shield = -1, armor = -1;
        int n = sscanf(body.c_str(), "%d|%f|%f|%f", &id, &hull, &shield, &armor);
        if (n < 2) return true;
        players::Player* p = players::ById(id);
        if (!p || !p->pawn) return true;
        // NOTE: on a client the registry holds ONLY the local player, so this is the local-player path.
        // The old `!p->local` guard made this handler unreachable on a client.
        ApplyHitpoints(p->pawn, p->local, hull, n >= 3 ? shield : -1.f, n >= 4 ? armor : -1.f);
        return true;
    }

--- 4. Also update the two console commands so they show the truth (CmdCombat combat.cpp:274 and
    CmdHp combat.cpp:352): print RatioOf(HullOf/ShieldOf/ArmorOf) instead of the raw field, and switch
    CmdGod (combat.cpp:287-291) to the AESPawn::Health/Armor/Shield offsets + es2off::UHitpointComponent::CannotDeplete.

--- 5. OPTIONAL EXTENSION — partner bars in a 3+ player session. `players::ById()` cannot resolve a
    remote player on a client (the registry only ever gets the local slot there). If partner condition
    is wanted, have the host append the pawn's NetGUID to the HP message and resolve it client-side with
    the machinery already present in ApplyNpcFire (combat.cpp:188-205):
      host:   Rva<Fn_GetNetGUID>(es2rva::FNetGUIDCache_GetNetGUID)(LocalGuidCache(), &guid, (const UObject*)p->pawn);
              coop::SendToAllClients(Format("HP|%d|%.4f|%.4f|%.4f|%llu", p->id, hull, shield, armor, guid));
      client: if the id is not our own, resolve `guid` through FNetGUIDCache_GetObjectFromNetGUID and pass
              ownShip=false so the armour write takes the base implementation.

--- 6. FALLBACK, only if the shield bar alone still lags after the above. Unlike armour, the shield's
    two vanilla change paths (UShieldComponent::TakeDamage 0x12CF3AD and TickRegeneration 0x1559BC6)
    both additionally broadcast OnShieldChanged (+0x3C8), so a widget could be bound there. If so, after
    SetRatio on the shield, fire it manually — MS x64 ABI verified from the wrapper at 0x2B3F59C
    (rcx=&delegate, rdx=AActor*, xmm2=float delta, r9=const FVector*, [rsp+0x20]=const FHitResult*):
      using Fn_ShieldChanged = void (*)(void* dlg, AActor* actor, float delta, const FVector* dir, const void* hit);
      alignas(16) unsigned char zeroHit[256] = {};   // FHitResult is 256 bytes
      FVector dir{};
      Rva<std::remove_pointer_t<Fn_ShieldChanged>>(es2rva::FOnShieldChangedDelegate_Broadcast)(
          (char*)shieldComp + es2off::UShieldComponent::OnShieldChanged, ownerPawn, deltaHitpoints, &dir, zeroHit);
    Do this only when the ratio actually changed. (Do NOT hook 0x2B3F49C — FOnHealthChangedDelegate::Broadcast
    and FOnArmorChangedDelegate::Broadcast are ICF-folded onto it; calling is fine, hooking is not.)

### Risks

- ARMOUR OVERRIDE HAS A PERSISTENT SIDE EFFECT. UArmorComponent::SetCurrentHitpointsWithRatio writes UGameplayLib::GetPlayerData()->Ships[CurrentShip].ArmorRatio whenever IsPlayerPawn(owner) is true. On a client a PARTNER's pawn is also player-controlled, so calling the override on a partner would silently overwrite the local player's saved armour ratio. The fix routes partners through the UHitpointComponent base (0x1556A24) to avoid this. The same latent hazard already exists in respawn.cpp's RestoreHitpoints, which calls the override on the host for whichever player is respawning — it writes the HOST's PlayerData ArmorRatio = 1.0 (benign today, but worth a note).
- LOCAL REGENERATION FIGHTS THE SYNC. The client's copy of the pawn ticks its own UShieldComponent::TickRegeneration, UArmorComponent::TickComponent and UHealthComponent hull-repair queue. Between HP packets the client's bars will drift upward on their own and then snap back to the host's value. Both machines simulate the same ship so drift is small, but at the recommended 10 Hz you may see a slight shimmer on a recharging shield. If it is objectionable, gate the client's local regen ticks off for its own pawn rather than lowering the sync rate.
- RELIABLE-CHANNEL PRESSURE. coop::SendToClient rides APlayerController::ClientMessage, a RELIABLE client RPC. Raising the rate from 4 Hz to 10 Hz unconditionally would triple reliable traffic per player; the change-detection gate in the proposed Tick is what keeps it net-lower than today (silence while nobody is being shot). Do not raise the rate without keeping the gate.
- MaxHitpoints DIVERGENCE. The bar is a ratio so it is exact, but the numeric hitpoint readout beside it is computed locally as GetRatio()*GetMaxHitpoints(). If the client's MaxHitpoints differs from the host's (different difficulty setting feeding UHitpointComponent::OnDifficultyChanged, or a host-side buff the client's ship does not have), the client will show the right percentage with the wrong absolute number. Add GetMaxHitpoints() to the message as a diagnostic before deciding whether SetMaxHitpoints (0x16FBDF8) needs to be synced too.
- SETTING HULL TO 0 ON THE CLIENT is safe with respect to death (the setter fires no depletion delegate) but the HUD will show a dead-empty hull bar for the ~5 s the host waits before respawning (respawn.cpp g_delay). That is arguably correct, but confirm it does not trip any Blueprint 'critical health' screen effect that latches.
- 0x2B3F49C IS ICF-FOLDED (FOnHealthChangedDelegate::Broadcast and FOnArmorChangedDelegate::Broadcast share it). Calling it is fine; hooking it is forbidden. The proposed fix never needs it — do not be tempted to add it to a hook list.

## Finding (confidence: medium )

### Root cause

There is NO single common root cause, and both "one common mechanism" candidates named in the brief are definitively ruled out. The four symptoms split into two independent causes.

CAUSE A — PROVEN, and it fully explains symptom 1 (hull/shield/armor bars). It is not a HUD bug at all: the data is dead.
  * ES2 replicates none of its own gameplay state. Zero ES2 classes implement `GetLifetimeReplicatedProps` (all 40 in sdk/symbols.tsv are engine classes; sdk/es2_functions.txt has none) and there are ZERO `OnRep_` functions in ES2 code. `UHitpointComponent::HitpointRatio` (+0xB0) therefore never crosses the wire.
  * On the client `combat.cpp` no-ops `UGameplayLib::ApplyESPointDamage`, so the client's own copy of its ship never takes local damage either.
  * The host does broadcast `HP|<id>|<hull>|<shield>` at 4 Hz (combat::Tick), but `combat::OnClientOp("HP")` applies it only `if (p && !p->local && p->pawn)` — it deliberately skips the local player. Worse, on a client `players` only ever contains the local player (`RegisterLocalAs` from WELCOME; `RegisterController` is only reached from host paths), so `players::ById(id)` returns null for the partner too and the HP message is a total no-op on a client.
  * Even if it were applied, `combat::SetHealthRatio` pokes `HitpointRatio` raw — exactly the stale-UI failure respawn.cpp already documents — and it never touches Shield or Armor at all.
  => The client's own hull/shield/armor floats literally never change. The bars are showing the truth; there is nothing to update.

CAUSE B — STRONG but not fully proven (needs one live check), and it explains symptoms 2, 3 and 4 together: the client's HUD is bound to a pawn that no longer exists.
  * The underlying state for those three IS alive on the client — no authority gate anywhere: `UWeaponComponent::SelectNextOrPreviousWeapon` (0x178FF1C) broadcasts `FEventOnWeaponSwitched` with no role check; `UJumpDriveComponent::TickComponent` (0x13EF840) only consults `GetPlayerPawn(0)`/`GetPlayerController(0)`/mission/dialog state, no HasAuthority. So the data changes and the HUD is not listening.
  * Those three indicators are pure Blueprint/UMG: `AESHUD::Tick` and `AESHUD::UpdateHUD` are 100% world-marker code (see evidence), and `AESHUD::CurrentESPlayerPawn` (+0x4B0) is written by no native code anywhere.
  * Every pawn-change notification a UMG HUD would normally rebind on is authority-only: `ReceivePossess`, `AController::OnNewPawn` (+0x308) and `AController::OnPossessedPawnChanged` (+0x2D0) are broadcast only at the tail of `AController::Possess` (0x16ACD68), which early-outs unless `bCanPossessWithoutAuthority` (bit2 of +0x338) or `Role (+0x168) == 3 (ROLE_Authority)`. On a client none of them ever fire. The only thing that runs on the client is `AESPlayerController::SetPawn` (0x5E170A8) via `ClientRestart`, which caches `ESPawn` (+0xA40) and rebinds exactly two CONTROLLER-side delegates (ShipMovement.OnBoostDepleted @+0x978, JumpDrive.OnCruiseModeEnd @+0x2A8) — nothing HUD-side.
  * And the mod swaps the client's pawn exactly once per session (`loadout::HostTick`: UnPossess -> `AGameModeBase::RestartPlayer` -> `K2_DestroyActor` on the old pawn), typically seconds-to-a-minute after join because the ~25 KB loadout stream is slow. The HOST's pawn is never swapped. That is precisely the host-works/client-broken asymmetry: anything the client's HUD cached at init points at a destroyed actor.

### Evidence

- RULES OUT (b), the AESHUD::Tick guard: full disassembly of AESHUD::Tick (RVA 0x13F2384..0x13F25EC) shows it does exactly three things - UBenchmarkHelper::BeginCustomTimeScope, Super AActor::Tick (0x12879EC), then (gated on AHUD::Owner!=null at +0x158, AESHUD::HideMainLayerCounter<=0 at +0x49C, and UESGameInstance::InstancePointer->bCheatHideMarkers at +0x1D4) a loop over USelfRegisteringComponent::GetAllActorsOfRegister(8) calling UHUDMarkerComponent::TickOcclusionTrace - then EndCustomTimeScope. It NEVER touches the player's own hull/shield/armor, weapons, jump drive or devices.
- RULES OUT (b), part 2: AESHUD::UpdateHUD(float) (0x13F31E8), the real HUD data function, is TAIL-CALLED from AESHUD::DrawHUD (0x16FE1C4, `jmp 0x1413f31e8` at 0x16FE211) i.e. from the AHUD PostRender path, which the mod does not hook. Its complete call graph (dumped, ~120 unique callees) is world-marker work only: UHUDMarkerComponent::TickWithMarkerData/QueryOcclusionTrace/GetMarkerPivotInWorld, UFactionComponent::GetRelationTo, per-TARGET UHitpointComponent::GetRatio via FindComponentByClass<UHealthComponent/UArmorComponent/UShieldComponent>, radar maths, AMapEventManager, UMapLib::GetLocationDisplayName.
- RULES OUT (b), residual caveat quantified: AActor::Tick (0x12879EC) disassembly shows it calls AActor::ReceiveTick(dt) (the BP Event Tick) and FLatentActionManager::ProcessLatentActions(this,dt). So the guard's ONLY possible channel to the four indicators is the HUD Blueprint's Event Tick / latent actions. But the guard's predicate (`!GetFirstLocalPlayerController(GetWorld()) || pc->Pawn==null`, mod/src/authority.cpp:38-42) is literally what commands.cpp CmdStatus prints (commands.cpp:45-50), and a live client reported a non-null pawn. Also loadout.cpp:476-479 deliberately does UnPossess+RestartPlayer in ONE host frame so the controller is never observed pawn-less. The guard logs '[authority] HUD tick skipped (no local pawn) xN' every 120 skips - one log grep closes this for good.
- RULES OUT (e), the identity accessors: UGameplayStatics::GetPlayerController (0x13F08F0) disassembly resolves World->OwningGameInstance (UWorld+0x1D8) -> LocalPlayers (UGameInstance+0x38, Num at +0x40) -> [0] -> ULocalPlayer/UPlayer::PlayerController (+0x30), with a FWorldContext->World()==World sanity check. It iterates LOCAL PLAYERS, not World->PlayerControllerList. So index 0 is always THIS machine's own player - correct on a client.
- RULES OUT (e), part 2: UGameplayLib::GetESPlayerPawn (0x13F0CC0) -> GetPlayerController(wco,0) + GetPawnOrSpectator + Cast<AESPawn>; UGameplayLib::GetESPlayerController (0x150D280) -> same + Cast; UGameplayLib::GetESHUD (0x15576B8) -> same + APlayerController::MyHUD (+0x358) + Cast; AESPlayerController::GetESHUD (0x155681C) -> this->MyHUD (+0x358). All correct on a client.
- RULES OUT (e), part 3: attribution.cpp's H_GetPlayerController only diverges while `g_acting` is non-null, and g_acting is set only inside H_OwnerHealthDepleted under `coop::CurrentRole() == coop::Role::Host` (attribution.cpp:79). It is inert on a client.
- CAUSE A evidence 1: `grep -F 'GetLifetimeReplicatedProps' sdk/symbols.tsv` -> 40 hits, every one an engine class (AActor, APawn, AController, APlayerController, UActorComponent, ...). `grep 'GetLifetimeReplicatedProps' sdk/es2_functions.txt` -> nothing. `grep -c 'OnRep_' sdk/es2_functions.txt` -> 0. ES2 replicates none of its own properties.
- CAUSE A evidence 2: mod/src/combat.cpp:220-227 - `if (op == "HP") { ... if (p && !p->local && p->pawn) SetHealthRatio(p->pawn, hp); }`. The `!p->local` skips the client's own ship. mod/src/combat.cpp:62-65 SetHealthRatio writes UE_FIELD(float, h, es2off::UHealthComponent::HitpointRatio) raw. Shield is read (GetShieldRatio) but never written; Armor is never touched.
- CAUSE A evidence 3: mod/src/players.cpp:81-92 RegisterLocalAs sets p.local = true for the client's own slot, and on a client that is the ONLY registered slot (RegisterController is called from coop::OnPostLogin, gated on Role::Host, and from RecvTransform which is host-only). So players::ById(otherId) is null on a client too - the partner-health mirror is dead as well.
- CAUSE A fix mechanism proven: UHitpointComponent::SetCurrentHitpointsWithRatio (0x1556A24) disassembly - clamps the ratio against MaxHitpointRatio (FBuffableFloat +0xB8, value at +0x30), writes HitpointRatio (+0xB0) and BonusHitpointRatio (+0x230), and if the total changed calls TMulticastScriptDelegate::ProcessMulticastDelegate on OnHitpointsChanged (UHitpointComponent+0x350) with {AActor* Owner(from UActorComponent::OwnerPrivate +0x90), this, float, float, float}. NO authority check anywhere. It is virtual (UHealthComponent override at 0x1556948, UArmorComponent overrides too, UShieldComponent does not) and it is a UFUNCTION (exec stub UHitpointComponent::execSetCurrentHitpointsWithRatio at 0x195626C), so FindFunction+ProcessEvent dispatches correctly - exactly what respawn.cpp:113-114 already does.
- CAUSE B evidence 1: AController::Possess (0x16ACD68) disassembly - `test byte ptr [rcx+0x338], 0x4` (bCanPossessWithoutAuthority) / `cmp byte ptr [rcx+0x168], 0x3` (Role==ROLE_Authority); if neither, it jumps to the FMessageLog warning path and returns. Only past that gate does it call OnPossess (vtable+0x7D8), ReceivePossess (FindFunctionChecked + ProcessEvent via vtable+0x278), OnNewPawn.Broadcast (AController+0x308) and OnPossessedPawnChanged ProcessMulticastDelegate (AController+0x2D0).
- CAUSE B evidence 2: AController layout - OnPossessedPawnChanged @0x2D0 (FOnPossessedPawnChanged, 16-byte dynamic multicast = BlueprintAssignable), Pawn @0x2E8, OldPawn @0x2F0, OnNewPawn @0x308 (24-byte native TMulticastDelegate), bCanPossessWithoutAuthority = bit2 of 0x338.
- CAUSE B evidence 3: AESPlayerController::SetPawn (0x5E170A8) disassembly - calls APlayerController::SetPawn, then if NewPawn!=null && this->Pawn(+0x2E8)==NewPawn && IsChildOf(AESPawn): stores AESPlayerController::ESPawn (+0xA40), then RemoveDynamic/AddDynamic on pawn->ShipMovement(+0x378)->OnBoostDepleted(+0x978) and pawn->JumpDrive(+0x3B0)->OnCruiseModeEnd(+0x2A8), both binding AESPlayerController member functions. Nothing HUD-side.
- CAUSE B evidence 4: scanned all 104 AESHUD symbols with llvm-objdump for any access to +0x4B0 - the ONLY hit is `movaps xmmword ptr [rbp + 0x4b0], xmm0` inside UpdateHUD, a stack write. AESHUD::CurrentESPlayerPawn (AESHUD+0x4B0, the last field; sizeof(AESHUD)=1208=0x4B8) is maintained entirely from Blueprint.
- CAUSE B evidence 5: mod/src/loadout.cpp:463-493 HostTick - on receipt of the client's full ship blob it does AController::UnPossess, AGameModeBase::RestartPlayer, then K2_DestroyActor on the old pawn, once per player (g_applied). g_respawn is only ever armed from OnServerOp("SD"), which only clients send, so the host's own pawn is never swapped. docs/NOTES.md records the blob as ~25 KB / 54 reliable chunks / up to ~1 min, so the swap lands well after the HUD has initialised.
- CAUSE B evidence 6 (no authority gate on the live state): UWeaponComponent::SelectNextOrPreviousWeapon (0x178FF1C) call graph contains FEventOnWeaponSwitched::Broadcast, UWeaponComponent::EquipWeapon, SpawnWeaponInstancesForSlot, CreateWeaponInfoFromItem - and no HasAuthority/GetNetMode/GetLocalRole. UJumpDriveComponent::TickComponent (0x13EF840) call graph: UGameplayStatics::GetPlayerPawn(wco,0), GetPlayerController(wco,0), UGameplayLib::GetPlayerControllerWithoutContext, UMissionLib::CantLeaveLocation, UDialogManager::IsMissionDialogPlayingOrEnqueued, CanCruiseModeBeActivated/CanJumpDriveBeActivated - again no authority check. Relevant delegates: UWeaponComponent::OnWeaponSwitched +0x648, OnWeaponChanged +0x698, OnNewWeaponInstalled +0x678, OnSpawnedWeaponsChanged +0x688; UJumpDriveComponent::OnCruiseModeCharge +0x258.
- (c) answered: the HUD binds by delegate, not by polling AESHUD. Component-side multicast delegates found: UHitpointComponent::OnHitpointsChanged +0x350 (present on Health/Armor/Shield), UHealthComponent::OnHealthChanged +0x3F8; UWeaponComponent::OnWeaponSwitched/OnWeaponChanged; UJumpDriveComponent::OnCruiseModeCharge/OnCruiseModeChargeStart/End. Plus a game-wide UI event bus on the UESGameInstance singleton (UESGameInstance::InstancePointer, RVA 0x9AC5E88): OnPauseChanged +0x9C0, OnHUDElementsInCornersChanged +0xA00, OnInventoryNeedsRefresh +0xA50, OnInventoryNeedsRebuild +0xA60, OnPlayerEquipmentChanged +0xA70, OnPlayerDevicesChanged +0xA80, OnTrackedMissionsChanged +0xA90. AESHUD::BeginPlay (0x5DE0C88) binds AESHUD::OnPauseChanged to UESGameInstance+0x9C0 and does nothing else. Note coop.cpp:287-288 suppresses UESGameInstance::PushPause/PopPause in MP, so OnPauseChanged never fires in a session - symmetric on host and client, so not the differentiator, but it does leave AESHUD::PausedSequencePlayers (+0x448) logic dormant.
- (d) answered: there is no single native 'refresh the HUD' UFunction. The nearest levers, in increasing cost: (i) per-component - UHitpointComponent::SetCurrentHitpointsWithRatio (broadcasts OnHitpointsChanged, ~free); (ii) the UESGameInstance UI bus - OnPlayerEquipmentChanged (+0xA70), OnPlayerDevicesChanged (+0xA80), OnInventoryNeedsRefresh (+0xA50), OnInventoryNeedsRebuild (+0xA60), broadcastable from native via TMulticastScriptDelegate::ProcessMulticastDelegate (0x124060C) with params taken from the matching /Script/ES2.<name>__DelegateSignature UFunction; (iii) AESHUD::PushHideMainLayer (0x5DE7974) + PopHideMainLayer (0x5DE7510), which drive the BlueprintNativeEvent AESHUD::OnHideMainLayerChanged - a blunt hide/show kick; (iv) UInventoryLib::ReinitShipAfterPotentialChanges, already called by loadout::ApplyOwnShipLocally.

### Proposed fix

Two separate fixes, because there are two causes. Neither is in authority.cpp or attribution.cpp.

FIX 1 (symptom 1, fully grounded, do this first - it is small and certain):
mod/src/combat.cpp
  a) Host side, `combat::Tick`: extend the broadcast to armour. Add `static float GetArmorRatio(AActor*)` mirroring GetShieldRatio but resolving "ArmorComponent", and send `HP|<id>|<hull>|<shield>|<armor>` (all read from `es2off::UHealthComponent::HitpointRatio` == UHitpointComponent+0xB0, which is the same offset on all three components).
  b) Client side, `combat::OnClientOp("HP")`: delete the `!p->local` condition and parse the 4th field. Resolve the target pawn as: `players::Player* p = players::ById(id); AActor* pawn = p ? p->pawn : nullptr; if (!pawn && id == players::LocalId()) { APlayerController* pc = GetFirstLocalPlayerController(GetWorld()); pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr; }` — the fallback is required because on a client the registry only ever holds the local slot.
  c) Replace `SetHealthRatio` with a setter that goes through the engine, for Health, Shield AND Armor:

        static void ApplyRatio(AActor* pawn, const char* cls, float ratio) {
            UObject* c = FindComponentOfClass(pawn, cls);
            if (!c || ratio < 0.f) return;
            float cur = UE_FIELD(float, c, es2off::UHealthComponent::HitpointRatio);
            if (fabsf(cur - ratio) < 0.001f) return;          // don't broadcast 4x/s for nothing
            UFunction* fn = FindFunction(c, "SetCurrentHitpointsWithRatio");
            if (fn) { float r = ratio; ProcessEvent(c, fn, &r); }
            else UE_FIELD(float, c, es2off::UHealthComponent::HitpointRatio) = ratio;
        }

     then call it for "HealthComponent", "ShieldComponent", "ArmorComponent". This is the same mechanism respawn.cpp:113-114 already relies on; SetCurrentHitpointsWithRatio (0x1556A24, virtual, UFUNCTION) clamps and broadcasts OnHitpointsChanged (+0x350), which is what the HUD listens to.
  d) Raise `g_healthHz` to ~10 while you are there, or gate sends on a changed ratio, so the bar drops promptly instead of in 250 ms steps.

FIX 2 (symptoms 2/3/4 — remove or compensate for the pawn swap). Prefer 2a; 2b is the fallback.

  2a (clean, recommended): never let the client fly a placeholder. In mod/src/net.cpp's `H_SpawnDefaultPawnAtTransform` / `H_RestartPlayer`, if the controller belongs to a remote player and `!loadout::HasStash(id)`, defer the spawn (return null / skip the restart) and retry from `loadout::HostTick` once the blob has landed. The client then possesses exactly one pawn for the whole session, and every HUD binding made at init stays valid. This deletes the entire class of stale-binding bugs (and also removes the visible ship-swap pop). Cost: the client sits pawn-less for up to the blob transfer time, so also shrink that — send the blob from `coop::OnClientMessage("WELCOME")` immediately rather than waiting for a pawn (`loadout::MaybeSendOnJoin` currently waits for `LocalPawn()`), and raise the chunk rate (kChunk 480 / 20 chunks-per-second is very conservative).

  2b (if the swap must stay): re-fire the possess notifications on the client. Hook `AESPlayerController::SetPawn` (RVA 0x5E170A8, unique symbol, safe to hook). In the detour: remember `prev = UE_FIELD(AActor*, pc, es2off::AController::Pawn)`, call the original, then if `coop::CurrentRole()==coop::Role::Client && pc == GetFirstLocalPlayerController(GetWorld())` and the new pawn is an ESPawn and differs from `prev`, replay the tail of AController::Possess that the authority gate skipped:
        - `ProcessEvent(pc, FindFunction(pc, "ReceivePossess"), &newPawn);`   (BP Event Possessed)
        - broadcast `AController::OnPossessedPawnChanged` at pc+0x2D0 via `TMulticastScriptDelegate::ProcessMulticastDelegate` (RVA 0x124060C) with a 16-byte param block `{APawn* OldPawn; APawn* NewPawn;}` — mirror the exact call at 0x16ACE2A.
        - optionally broadcast `AController::OnNewPawn` at pc+0x308 (native TMulticastDelegate<void(APawn*)>) via the helper called at 0x16ACE08.
     Do this only on the client and only for the local controller, and guard with a re-entrancy flag.
     Simpler variant worth trying first: set `bCanPossessWithoutAuthority` (bit 2 of AController+0x338) on the client's own PC once, then call the real `AController::Possess` (es2rva::AController_Possess, already in rvas.h) with the new pawn from the SetPawn detour — the engine then runs OnPossess + ReceivePossess + both broadcasts itself. Verify it does not misbehave through APlayerController::OnPossess's ClientRestart RPC (a server RPC invoked on a client is a no-op with a warning) — test before shipping.

  2c (blunt fallback if 2b's BP handler still misses something): after a client-side pawn swap, broadcast the UESGameInstance UI bus events `OnPlayerEquipmentChanged` (+0xA70), `OnPlayerDevicesChanged` (+0xA80) and `OnInventoryNeedsRebuild` (+0xA60) off `UESGameInstance::InstancePointer` (global RVA 0x9AC5E88), and/or do one `AESHUD::PushHideMainLayer` (0x5DE7974) + `PopHideMainLayer` (0x5DE7510) pair on the client's AESHUD.

DO NOT change authority.cpp's HUD guard or attribution.cpp's shim as part of this fix — both are proven not to be the cause. (One unrelated host-side hazard found: while `attribution::g_acting` is active, `UXPComponent::OwnerHealthDepleted` calls `UGameplayLib::GetESHUD` (xref at 0x1942468), which then resolves the REMOTE player's controller — whose server-side `MyHUD` is null on a listen server — so the host's own hit/XP HUD feedback is silently skipped during a remote player's kill. Worth fixing separately by narrowing the shim to the XP award call only.)

LIVE CHECKS that would settle the remaining uncertainty in ~5 minutes on a running session (client console via scripts/console.py):
  1. `status` -> pawn non-null; then grep the mod log for "[authority] HUD tick skipped" — if the counter is not climbing, cause (b) is closed for good.
  2. `hp` while the client is being shot -> its own hull ratio should stay exactly 1.000 (confirms CAUSE A).
  3. `props pc ESPawn` -> is AESPlayerController::ESPawn (+0xA40) the pawn the player is actually flying? If yes, SetPawn ran and only the HUD-side binding is stale.
  4. `objects UserWidget 300` then `props <mainHudWidget>` -> look for a cached pawn/ship/component reference whose address differs from the live pawn, or points at a destroyed object (confirms CAUSE B).
  5. `objects ESPawn` on the client -> is the pre-swap placeholder pawn still present/pending-kill?

### Risks

- FIX 1 sends an authoritative ratio 4-10x/s. SetCurrentHitpointsWithRatio also resets BonusHitpointRatio (+0x230) and clamps to MaxHitpointRatio; if the client's ship has different buffs/max-hitpoints than the host's copy, the client will see slightly different absolute numbers. Send the ratio only (as now), never absolute hitpoints, and skip the write when the delta is below ~0.001 so you do not broadcast OnHitpointsChanged every frame.
- FIX 1 can fight the client's own local regeneration/shield-recharge tick: the client's UShieldComponent will regenerate locally while the host says it is depleted, producing a visible flicker. Consider disabling local regen on the client's own pawn, or accept the host value as strictly authoritative and clamp.
- FIX 2b replays engine-internal notifications by hand. Broadcasting OnPossessedPawnChanged / OnNewPawn / ReceivePossess on a client fires Blueprint code that was written assuming server context — it may re-enter gameplay paths that touch AESGameModeBase, which does not exist on a client (docs/NOTES.md records that as an instant crash). Add a re-entrancy guard and test with a crash dump ready.
- FIX 2b's simpler variant (setting bCanPossessWithoutAuthority and calling the real AController::Possess on a client) also runs APlayerController::OnPossess, which calls PawnToPossess->PossessedBy(this), SetControlRotation, DispatchRestart and ClientRestart. On a client ClientRestart is a server-declared RPC invoked from the client and should be a no-op, but PossessedBy/DispatchRestart may re-run ES2 pawn setup. Higher blast radius than the manual replay.
- FIX 2a changes join timing: the client is pawn-less until its loadout blob arrives. authority.cpp's AESHUD::Tick guard exists precisely because AESHUD faults with no local pawn, and coop.cpp's client tick gates HELLO on LocalPawn(); a longer pawn-less window will exercise both. Verify the guard's log counter climbs only during that window and that HELLO/loadout still complete.
- Hooking AESPlayerController::SetPawn: verify with gen_sdk.py that 0x5E170A8 is not ICF-folded (it is a unique symbol in sdk/symbols.tsv, so it is safe), and that no other mod module already hooks it — MinHook refuses a second hook on the same RVA.
- The host-side attribution shim hazard noted in the fix (GetESHUD returning a remote player's null server-side HUD during a kill scope) is a real, separate defect; fixing it changes host-side XP/hit feedback and should be validated separately.

## Verifier verdict: PARTLY_WRONG (implementable: True )

- The ARMOUR OVERRIDE DOES MORE THAN CLAIMED (evidence 7 is incomplete). Disassembly of 0x1556858 shows two writes, not one: after `PlayerData.Ships[CurrentShip].ArmorRatio = GetRatio()` (`imul rcx,rax,0x3d0` / `[rcx+rdx+0x364]`), it reloads OwnerPrivate (+0x90), checks the owner's class IsChildOf the same global class IsPlayerPawn uses, and writes `[owner + 0x81C] = GetRatio()` — and 0x81C = AESPawn::ShipData (0x4B8) + FShipData::ArmorRatio (0x364). So calling the armour override on a partner's pawn corrupts BOTH the local player's saved PlayerData AND the partner pawn's ShipData. This strengthens the ownShip gate but the fix's comment should say so.
- EVIDENCE 9's SUPPORTING ARGUMENT IS WRONG. UArmorComponent::TickComponent (0x1557748..0x1557810) is not armour regeneration. It fetches another component through the owner's vtable slot [+0x700] and, if that component's effective MaxHitpoints <= 0 (`comiss xmm4, 0 / jbe 0x1557959`) or its ratio+bonus <= 0 (`[r9+0x230] + [r9+0xB0]`, `jbe 0x1557959`), calls this->SetCurrentHitpointsWithRatio(0.0f) — it FORCES armour to zero, it never refills it. The inference 'the armour bar visibly refills during regen, therefore the bar is fed by OnHitpointsChanged' is unsupported by that function. (The narrow facts that TickComponent calls [rax+0x4c8] at 0x155795F and never touches +0x3C8 are correct.) Practical consequence for the fix: on a client whose synced shield ratio is 0, the local armour tick re-zeroes armour every frame, so armour can never display above 0 while the shield is down.
- DEFECT 2 IS AN INFERENCE, NOT A PROVEN FACT. Nothing in the PDB shows the ES2 HUD binds to OnHitpointsChanged/OnHealthChanged rather than polling GetRatio() each frame. And because defect 1 makes the handler dead code, the reported symptom is fully explained by defect 1 alone — the observed behaviour carries zero information about the write mechanism. respawn.cpp:102-103 is prior in-project evidence, not proof for these particular bars. Downgrade 'even if it ran, it would not refresh the UI' from fact to strong hypothesis. There is, however, a separate and provable reason the raw write is wrong: it leaves BonusHitpointRatio (+0x230) untouched, so writing a host-side GetRatio() (which INCLUDES the bonus) into the raw field double-counts over-shield.
- EVIDENCE 10's 'EXACT INVERSE' CLAIM IS TRUE ONLY WHEN MaxHitpointRatio == 1.0. The setter clamps HitpointRatio to [0, MaxHitpointRatio@+0xE8] but computes BonusHitpointRatio from a hard-coded 1.0 (`subss xmm5, [0x1486acb20]`), while GetRatio() returns HitpointRatio + BonusHitpointRatio. With MaxHitpointRatio = 1.2 and want = 1.3 you get 1.2 + 0.3 = 1.5, not 1.3. Then the proposed `fabsf(have - want) < 0.0005f` guard never trips and the setter (with its broadcast) fires on every message. Clamp `want` or accept the drift; in practice MaxHitpointRatio is 1.0.
- THE PROPOSED HOST TICK INTRODUCES A NEW BUG THE CLAIM DID NOT CONSIDER: the client's own components keep ticking. UShieldComponent::TickRegeneration (0x1559A4C) calls ChangeHitpoints (0x1559B80) and broadcasts OnShieldChanged (0x1559BC6), and there is no HasAuthority/role gate anywhere in UShieldComponent::TickComponent (0x1559BE0). So on the client the local shield regenerates on its own. Combined with the proposed change-gating ('only send when it moved; keepalive every 2.0 s'), the client's shield bar will visibly refill locally and then snap back down every 2 seconds — strictly worse than the current unconditional 4 Hz send. Keep an unconditional resend at >= 5 Hz (or drop the keepalive to ~0.2 s), or suppress local shield regen for the client's own pawn. UHealthComponent::TickComponent (0x1743298) likewise calls ChangeHitpoints and broadcasts for hull repair — harmless only while the repair queue is empty.
- gen_sdk OFFSETS TRAP in the SYMBOLS NEEDED block: 'MaxHitpointRatio' @0xB8 is an FBuffableFloat (sizeof 64: BaseValue +0x00, CurrentValue +0x30, bIsDirty +0x34, BaseValueAtRefresh +0x38) and 'MaxHitpoints' @0xF8 is an FAttributeAccess (sizeof 80: AttributeID FName +0x00, AttributeValueSimple FBuffableFloat +0x08, AttributeValueFromItem +0x48). The offsets are right but they are struct fields — `UE_FIELD(float, c, MaxHitpoints)` reads half an FName, not a value. Use GetMaxHitpoints() (0x2B3AF6C), which handles the item-attribute path too.
- MINOR: combat::Tick is never called on a client — coop.cpp only calls `combat::Tick(dt, true)` inside the host branch (coop.cpp:337); the client branch (coop.cpp:341-355) does not call it at all. The proposed `g_hpNow += dt;` before `if (!isHost) return;` therefore never advances on a client. Harmless (g_hpNow is host-only state) but the code comment implying a shared clock is wrong.
- MINOR: calling UHealthComponent::SetCurrentHitpointsWithRatio by RVA bypasses the vtable. That is safe here (it is a plain native virtual with an exec stub, not a BlueprintNativeEvent, so no BP subclass can override it), but calling `(*(void***)comp)[153]` would be more faithful and would reduce the new RVA surface to just the base 0x1556A24 (still needed for the deliberate 'use the base impl for a partner's armour' case).
- MINOR residual risk the claim states too strongly: 'it does NOT fire any depletion/death delegate' is true of the native code, but OnHealthChanged (+0x3F8) IS broadcast, and a Blueprint bound to it could itself react to ratio <= 0. Not verifiable statically — worth a first-run check before shipping a hull value of exactly 0 to a client.

**Fix corrections:**

 Implementable, and the core of it is right: drop the `!p->local` guard (that alone is the fix for symptom 1), extend the protocol to hull/shield/armour, and write through SetCurrentHitpointsWithRatio instead of the raw field. Every RVA, offset, vtable slot and calling convention it depends on checks out and none of the RVAs are ICF-folded. Apply these changes before implementing:

1. Do NOT ship the change-gated host tick as written. UShieldComponent::TickRegeneration (0x1559A4C) runs unguarded on the client, so between the proposed 2-second keepalives the client's shield refills locally and then snaps back. Keep an unconditional resend at >= 5 Hz, or shorten the keepalive to ~0.2 s. (Optionally also stop the local regen: the shield component's tick has no authority check to lean on, so the practical lever is a high resend rate.)

2. Keep the ownShip gate on the armour setter, and strengthen the comment: 0x1556858 writes BOTH PlayerData.Ships[CurrentShip].ArmorRatio (+0x364) AND owner->ShipData.ArmorRatio (owner+0x81C). A partner's pawn must use the base 0x1556A24.

3. In the SYMBOLS NEEDED block, drop 'MaxHitpointRatio' and 'MaxHitpoints' from OFFSETS['UHitpointComponent'] or annotate them as struct offsets (FBuffableFloat / FAttributeAccess). For any diagnostic that wants a number, call GetMaxHitpoints (0x2B3AF6C), not UE_FIELD(float, ...).

4. Clamp the applied ratio (e.g. `want = min(want, 1.0f + maxBonus)`) or accept that the `fabsf(have-want) < 0.0005f` early-out will not hold if MaxHitpointRatio != 1.0, causing a redundant broadcast per message.

5. Consider calling through the object's own vtable slot 153 (`(*(void***)comp)[153]`) instead of three per-class RVAs; you still need the base RVA 0x1556A24 for the partner-armour case.

6. Treat the "raw write leaves the UI stale" rationale as a hypothesis. Justify the setter on the provable ground instead: the raw write does not touch BonusHitpointRatio, so writing a host-side GetRatio() into +0xB0 double-counts over-shield.

7. Optional extension 5 (NetGUID for partner bars) and fallback 6 (manual OnShieldChanged broadcast) are both technically sound as written — the FOnShieldChangedDelegate::Broadcast ABI and the "never hook 0x2B3F49C" warning are both verified correct.
- CONFIRMED: DEFECT 1 (dominant cause) CONFIRMED. combat.cpp:225 is `if (p && !p->local && p->pawn) SetHealthRatio(p->pawn, hp);`. On a client the players registry contains ONLY the local slot: coop.cpp:257 `players::RegisterLocalAs(me,id)` on WELCOME is the only registration path that runs there. Every `players::RegisterController` call site is host-only — coop.cpp:151 sits inside RecvTransform, reached only from OnServerMessage which returns at coop.cpp:224 `if (g_role != Role::Host) return true;`; coop.cpp:228 is in that same host-gated function; coop.cpp:305 and 323 are inside `if (r == Role::Host)`; coop.cpp:439-440 OnLogin returns early for non-hosts. players::ById (players.cpp:24) returns nullptr for a slot that is not `alive`, so ById(0) (the host) is nullptr on a client and ById(myId) is the local player, which `!p->local` then skips. The HP handler is unreachable on a client.
- CONFIRMED: The host DOES send the HP message today: combat::Tick is invoked only from the host branch (coop.cpp:337 `combat::Tick(dt, true)`), at 4 Hz, for every entry in players::All(). So the data reaches the wire and is discarded on receipt.
- CONFIRMED: DEFECT 3 CONFIRMED. combat.cpp:224 parses `sh` and never applies it; there is no armour field in the protocol; combat.cpp:66-70 GetShieldRatio reads the raw HitpointRatio (+0xB0) and therefore drops BonusHitpointRatio (+0x230), i.e. over-shield.
- CONFIRMED: ALL NAMED RVAs EXIST WITH THE EXACT SIGNATURE AND ARE UNIQUE (not ICF-folded), verified with `awk -F'\t' '$1==RVA{c++}' sdk/symbols.tsv`: UHitpointComponent::SetCurrentHitpointsWithRatio 0x1556A24 (1), UHealthComponent:: 0x1556948 (1), UArmorComponent:: 0x1556858 (1), UHitpointComponent::SetCurrentHitpoints 0x14E6B9C (1), ChangeHitpoints 0x14E55C8 (1), GetRatio 0x12D00CC (1), GetCurrentHitpoints 0x12CEA7C (1), GetMaxHitpoints 0x2B3AF6C (1), SetMaxHitpoints 0x16FBDF8 (1), FOnShieldChangedDelegate::Broadcast 0x2B3F59C (1).
- CONFIRMED: The ICF warning is correct: 0x2B3F49C carries TWO symbols — FOnArmorChangedDelegate::Broadcast and FOnHealthChangedDelegate::Broadcast. It must never be hooked.
- CONFIRMED: UShieldComponent genuinely has NO SetCurrentHitpointsWithRatio override (`grep -E '^UShieldComponent::' sdk/es2_functions.txt` lists 22 entries, none of them the setter), so the shield must go through the base at 0x1556A24.
- CONFIRMED: Virtual slot CONFIRMED: `pdb_types.py vtable UHitpointComponent` gives [151] TakeDamage, [152] OnDifficultyChanged, [153] SetCurrentHitpointsWithRatio. 153*8 = 0x4C8, and UHitpointComponent::SetCurrentHitpoints (0x14E6B9C) loads r10 = [vtable+0x4c8] and tail-jumps `jmp r10` at 0x14E6C13 after dividing by the effective MaxHitpoints.
- CONFIRMED: BASE SETTER SEMANTICS CONFIRMED byte for byte at 0x1556A24: old total xmm6 = [this+0xB0] + [this+0x230]; HitpointRatio = max(0, min(MaxHitpointRatio_effective@+0xE8, arg)); BonusHitpointRatio = clamp(arg-1.0, 0, [+0x234]); `ucomiss xmm4, xmm6 / je 0x1556B09` returns without broadcasting when the total is unchanged; otherwise it fills a 32-byte parms block {OwnerPrivate(+0x90), this, MaxHitpoints*delta, deltaRatio, previousRatio} and calls ProcessMulticastDelegate on `lea rcx,[r9+0x350]` = OnHitpointsChanged.
- CONFIRMED: HULL OVERRIDE CONFIRMED at 0x1556948: saves [rbx+0xB0], calls 0x1556A24, and if HitpointRatio changed broadcasts `lea rcx,[rbx+0x3f8]` = UHealthComponent::OnHealthChanged with a zeroed hit direction. It touches none of OnPreHealthDepleted/OnHealthDepleted/OnDied/OnBreak/OnWouldDeplete.
- CONFIRMED: DEATH-FLOW SAFETY CONFIRMED. Neither 0x1556A24 nor 0x1556948 fires any depletion/death delegate, and respawn.cpp:141 `if (!g_enabled || !isHost) return;` means the mod's death watch never runs on a client. Driving the client's hull to 0.0 will not start a game-over or respawn flow.

## Verifier verdict: PARTLY_WRONG (implementable: True )

- FALSE — 'the mod swaps the client's pawn exactly once per session'. mod/src/respawn.cpp:120-141 (DoRespawn) performs the identical AController::UnPossess -> AGameModeBase::RestartPlayer -> K2_DestroyActor(oldPawn) sequence on EVERY death, driven from respawn::Tick over players::All(). The swap recurs, it is not a once-per-session event.
- FALSE — 'The HOST's pawn is never swapped. That is precisely the host-works/client-broken asymmetry.' players::RegisterController (mod/src/players.cpp:59-77) registers the host itself as slot 0 with local=true (isLocal = NetConnection==nullptr), and respawn::Tick iterates players::All(), so respawn::DoRespawn swaps the HOST's own pawn on host death too. The real asymmetry is not who gets swapped; it is the authority gate inside AController::Possess (0x16ACD68) — on the host every possession fires ReceivePossess/OnNewPawn/OnPossessedPawnChanged, on a client none ever does. The Possess-gate disassembly evidence is correct; the causal framing built on top of it is not.
- MISSED CONSEQUENCE that undermines FIX 2a — because AController::Possess early-outs on a client for ALL possessions, the client never receives a possess notification for its FIRST pawn either, not just the post-loadout replacement. If the HUD binds on ReceivePossess / OnPossessedPawnChanged, the client HUD is broken from frame one and FIX 2a ('never let the client fly a placeholder') restores nothing, because there was never a possess notification to preserve. The claim that 2a 'deletes the entire class of stale-binding bugs' is unsupported. 2a only helps in the narrower case where the HUD caches via a one-shot poll (BP BeginPlay + GetPlayerPawn(0)) that succeeded against the placeholder.
- WRONG REASONING in FIX 1(b) — 'the fallback is required because on a client the registry only ever holds the local slot'. On a client players::ById(players::LocalId()) DOES return a valid slot: coop.cpp:257 RegisterLocalAs(me, id) writes g_slots[id], and coop::Tick calls players::Refresh() for both roles (coop.cpp, after the `if (r == Role::None) return;`), which re-reads p->pawn from AController::Pawn every frame. The GetFirstLocalPlayerController fallback is harmless but unnecessary; the only required change is deleting the `!p->local` condition.
- OVERSTATED — 'CAUSE A ... fully explains symptom 1' and 'FIX 1 ... is small and certain'. The two causes are not independent for symptom 1: if CAUSE B is real the HUD reads a stale pawn, so FIX 1 alone will not make the bars move. FIX 1 is necessary but possibly not sufficient, and must be validated by reading the live pawn's HitpointRatio over the console (`combat`/`hp`), not by watching the bar.
- INCOMPLETE — FIX 1(c) armour caveat. UArmorComponent::SetCurrentHitpointsWithRatio (0x1556858) is NOT a pure setter: after chaining to the base at 0x1556A24 it calls UGameplayLib::IsPlayerPawn, UGameplayLib::GetPlayerData() and UHitpointComponent::GetRatio — i.e. it touches this machine's UPlayerData singleton. Driving it from a 4-10 Hz network message on the client runs that ES2 player-data path on every update. (UHealthComponent's override at 0x1556948 additionally broadcasts OnHealthChanged +0x3F8 and is benign; UShieldComponent has no override at all — no such symbol exists — so shield goes straight to the base virtual.)
- MISSED — neither SetCurrentHitpointsWithRatio override runs depletion/death logic (no OnWouldDeplete/OnBreak/depleted call anywhere in 0x1556A24, 0x1556948 or 0x1556858). Mirroring a 0.0 hull ratio to the client will therefore show an empty bar without killing the client locally. That is consistent with host-authoritative respawn (respawn.cpp), but the fix text does not state it and it should be, because it changes what the tester will see.
- MISSED — local regeneration will fight the mirror. Nothing gates shield/hull regeneration on the client (no authority check found on the hitpoint path), so a periodic authoritative overwrite plus local regen will make the shield bar jitter. Either raise the rate well above 4 Hz (the proposal's own point d), suppress local regen on the client, or accept the jitter knowingly.
- ICF HAZARD THE CLAIM MISSED — the OnNewPawn broadcast helper called at 0x16ACE08 lives at RVA 0x2B3ACB0 and is ICF-folded across 30 symbols (all TMulticastDelegate<...>::Broadcast instantiations, including TMulticastDelegate<void(APawn*)>::Broadcast). It is safe to CALL with a matching (this, APawn*) signature but must NEVER be hooked and must not enter the SDK spec as a hook target. (TMulticastScriptDelegate<FNotThreadSafeDelegateMode>::ProcessMulticastDelegate<UObject> at 0x124060C is unique — fine. All other RVAs in the claim are unique too; I checked every one.)
- MISSED INTERACTION — AController::Possess is ALREADY hooked by this mod (mod/src/respawn.cpp:192, H_Possess, veto of non-ship possession). FIX 2b's 'simpler variant' calls es2rva::AController_Possess, i.e. the raw address, which re-enters that detour. H_Possess is host-gated so it falls through on a client, but the interaction must be accounted for rather than discovered at runtime.
- UNDERSTATED RISK — FIX 2b's 'simpler variant' (set bCanPossessWithoutAuthority, then call AController::Possess on the client). Past the gate, Possess dispatches OnPossess through vtable+0x7D8; APlayerController::OnPossess calls Pawn->PossessedBy(this) (writing Controller/Owner on a replicated actor the client does not own — the next actor replication can stomp it) and ClientRestart -> PawnClientRestart (resets input/camera state). This is not a drop-in and is materially riskier than 'verify it does not misbehave'.
- UNVERIFIED (not refuted) — 'AESHUD::CurrentESPlayerPawn (+0x4B0) is written by no native code anywhere'. The offset itself is real and is the last field of AESHUD (sizeof 0x4B8), but I did not reproduce the 104-symbol objdump scan. It is diagnostic-only and does not affect either fix.
- MINOR — the AESHUD::Tick summary ('exactly three things') elides AHUD::GetOwningPawn, AActor::GetAllChildActors and the FCollisionQueryParams construction that precede the marker loop. The substantive conclusion (marker/occlusion work only, never the player's own hull/weapons/jump drive/devices) is correct.
- MINOR — the '~25 KB / 54 chunks / up to ~1 min' figure is quoted from docs/NOTES.md:13, not derivable from the code: kChunk = 480 at one chunk per 0.05 s (loadout.cpp:84, ClientTick) is ~9.6 KB/s, i.e. ~2.6 s of sending for 25 KB. Cite it as a doc observation about real reliable-RPC throughput, not as a code-derived bound.

**Fix corrections:**

 FIX 1 is sound and should be implemented essentially as written, with four amendments: (a) drop the GetFirstLocalPlayerController fallback rationale — players::ById(players::LocalId()) already resolves on a client because RegisterLocalAs fills that slot and players::Refresh() runs for both roles; the only required edit is deleting `!p->local`; (b) keep the change-threshold guard, and prefer gating sends on a changed ratio over simply raising g_healthHz, because nothing gates shield regeneration on the client and a periodic authoritative overwrite will otherwise make the shield bar jitter against local regen; (c) note in the code that neither SetCurrentHitpointsWithRatio override runs depletion/death logic, so mirroring 0.0 hull shows an empty bar without a local death — host-authoritative respawn still owns that; (d) note that UArmorComponent's override (0x1556858) additionally calls UGameplayLib::IsPlayerPawn and UGameplayLib::GetPlayerData(), i.e. it is not a pure setter and will touch this machine's UPlayerData on every armour update. Validate FIX 1 by reading the live pawn's HitpointRatio over the console, not by watching the bar — if CAUSE B is real the bar will still not move, so FIX 1 is necessary but may not be sufficient for symptom 1.

FIX 2 needs re-framing before implementation. The stated asymmetry ("the host's pawn is never swapped") is false: respawn::DoRespawn swaps the host's pawn on host death too, and swaps the client's pawn on every client death, not once per session. The correct asymmetry is the authority gate in AController::Possess — a client never receives ReceivePossess / OnNewPawn / OnPossessedPawnChanged for ANY pawn, including its first. Consequently FIX 2a (defer the spawn so the client possesses one pawn for the session) does NOT restore a notification the client never received; it only helps if the HUD caches via a one-shot poll that happened to succeed against the placeholder, and it does nothing for the recurring death-respawn swap. Prefer FIX 2b (re-fire the possess notifications from an AESPlayerController::SetPawn detour) as the primary fix, not the fallback, and keep 2a only as a cosmetic improvement.

For FIX 2b specifically: the hook point (0x5E170A8), ProcessMulticastDelegate (0x124060C) and all offsets are verified and safe. Three additions the plan is missing. First, the OnNewPawn broadcast helper called at 0x16ACE08 lives at RVA 0x2B3ACB0 and is ICF-folded across 30 symbols — call it if you must, but never hook it and never add it to the SDK spec as a hook target. Second, AController::Possess is already hooked by this mod (respawn.cpp:192, H_Possess); the "simpler variant" that calls es2rva::AController_Possess re-enters that detour (host-gated, so it falls through on a client, but this must be deliberate). Third, that simpler variant is riskier than presented: past the gate Possess dispatches OnPossess via vtable+0x7D8, and APlayerController::OnPossess calls Pawn->PossessedBy(this) — writing Controller/Owner on a replicated actor the client does not own, which the next actor replication can stomp — plus ClientRestart -> PawnClientRestart, which resets input and camera state. Implement the explicit replay (ReceivePossess via ProcessEvent with an 8-byte APawn* block, then ProcessMulticastDelegate on pc+0x2D0 with a 16-byte {OldPawn, NewPawn} block, optionally the pc+0x308 broadcast) rather than flipping bCanPossessWithoutAuthority.

Also run the live checks before committing to FIX 2 at all: CAUSE B is the one part of the analysis that is not proven, and the two decisive observations are cheap — grep the log for "[authority] HUD tick skipped" (closes the guard hypothesis for good) and dump the main HUD widget's cached pawn/component references on the client to see whether they point at a destroyed actor or were never set.
- CONFIRMED: Every RVA in the claim exists with exactly the stated signature and is UNIQUE (not ICF-folded): 0x1556A24 UHitpointComponent::SetCurrentHitpointsWithRatio(float), 0x1556948 UHealthComponent override, 0x1556858 UArmorComponent override, 0x195626C execSetCurrentHitpointsWithRatio, 0x5E170A8 AESPlayerController::SetPawn(APawn*), 0x124060C TMulticastScriptDelegate<FNotThreadSafeDelegateMode>::ProcessMulticastDelegate<UObject>(void*) const, 0x5DE7974 AESHUD::PushHideMainLayer, 0x5DE7510 AESHUD::PopHideMainLayer, 0x9AC5E88 UESGameInstance::InstancePointer, 0x178FF1C UWeaponComponent::SelectNextOrPreviousWeapon(bool,bool), 0x13EF840 UJumpDriveComponent::TickComponent, 0x16ACD68 AController::Possess, 0x13F2384 AESHUD::Tick, 0x13F31E8 AESHUD::UpdateHUD, 0x16FE1C4 AESHUD::DrawHUD, 0x13F08F0 UGameplayStatics::GetPlayerController, 0x13F0CC0 UGameplayLib::GetESPlayerPawn, 0x150D280 UGameplayLib::GetESPlayerController, 0x15576B8 UGameplayLib::GetESHUD, 0x155681C AESPlayerController::GetESHUD, 0x12879EC AActor::Tick.
- CONFIRMED: Every claimed offset is real. UHitpointComponent: HitpointRatio 0xB0, MaxHitpointRatio 0xB8 (FBuffableFloat, value at +0x30), BonusHitpointRatio 0x230, OnHitpointsChanged 0x350. UHealthComponent::OnHealthChanged 0x3F8. AController: OnPossessedPawnChanged 0x2D0 (16-byte dynamic multicast), Pawn 0x2E8, OldPawn 0x2F0, OnNewPawn 0x308 (24-byte TMulticastDelegate<void(APawn*)>), bCanPossessWithoutAuthority = bit2 of 0x338. AActor::Role 0x168, AActor::Owner 0x158. APlayerController::MyHUD 0x358. AESPlayerController::ESPawn 0xA40. AESHUD: bForceHidden 0x498, bMainHUDHidden 0x499, HideMainLayerCounter 0x49C, HideTopLayerCounter 0x4A0, CurrentESPlayerPawn 0x4B0 (last field, sizeof 0x4B8). AESPawn: Health 0x350, Armor 0x358, Shield 0x360, EnergyCore 0x370, ShipMovement 0x378, PrimaryWeapons 0x380, SecondaryWeapons 0x388, Devices 0x390, Consumables 0x398, HUDMarker 0x3A0, JumpDrive 0x3B0. UWeaponComponent: OnWeaponSwitched 0x648, OnNewWeaponInstalled 0x678, OnSpawnedWeaponsChanged 0x688, OnWeaponChanged 0x698. UJumpDriveComponent: OnCruiseModeCharge 0x258, ChargeStart 0x278, ChargeEnd 0x288, OnCruiseModeEnd 0x2A8. UShipMovementComponent::OnBoostDepleted 0x978. UESGameInstance: OnPauseChanged 0x9C0, OnHUDElementsInCornersChanged 0xA00, OnInventoryNeedsRefresh 0xA50, OnInventoryNeedsRebuild 0xA60, OnPlayerEquipmentChanged 0xA70, OnPlayerDevicesChanged 0xA80, OnTrackedMissionsChanged 0xA90, bCheatHideMarkers 0x1D4. UGameInstance::LocalPlayers 0x38 (Num at 0x40), UPlayer::PlayerController 0x30, UWorld::OwningGameInstance 0x1D8.
- CONFIRMED: ES2 replicates none of its own state. grep -F 'GetLifetimeReplicatedProps' sdk/symbols.tsv yields exactly 40 hits, all engine classes (AActor, APawn, AController, APlayerController, UActorComponent, UObject, USceneComponent, ...); sdk/es2_functions.txt has 0 hits for GetLifetimeReplicatedProps and 0 for OnRep_. UHitpointComponent::HitpointRatio therefore never crosses the wire.
- CONFIRMED: mod/src/combat.cpp:225 is literally `if (p && !p->local && p->pawn) SetHealthRatio(p->pawn, hp);` — the local player is skipped. combat.cpp:62-65 SetHealthRatio writes es2off::UHealthComponent::HitpointRatio raw, exactly the stale-UI pattern respawn.cpp:102-103 documents. Shield is read via GetShieldRatio and transmitted but never applied; Armor is never read or written. combat::Tick early-returns unless isHost.
- CONFIRMED: combat.cpp:174-183 no-ops UGameplayLib::ApplyESPointDamage, enabled only while coop::CurrentRole()==Role::Client (coop.cpp OnRoleChanged: combat::SetClientDamageBlock(r == Role::Client)). The client's own ship therefore takes no local damage either.
- CONFIRMED: On a client the players registry only ever holds the local slot. players::RegisterController is reached only from host paths: coop::OnServerOp early-returns unless Role::Host (coop.cpp:224) so RecvTransform/HELLO are host-only; OnRoleChanged and the world-change branch gate on Role::Host; coop::OnPostLogin returns unless Role::Host. Only RegisterLocalAs (coop.cpp:257, from WELCOME) runs on a client. players::ById(partnerId) is therefore null on a client and the partner-health mirror is dead as well.
- CONFIRMED: UHitpointComponent::SetCurrentHitpointsWithRatio (0x1556A24) disassembles exactly as claimed: reads +0xB0 + +0x230 as the old total, RefreshValue()s the FBuffableFloat at +0xB8, clamps the argument to [0, MaxHitpointRatio(+0xB8+0x30)] via minss/maxss, writes +0xB0, clamps the remainder against +0x234 into +0x230, and if the new total differs calls TMulticastScriptDelegate::ProcessMulticastDelegate (0x124060C) on the delegate at this+0x350 with a param block {AActor* Owner (from UActorComponent::OwnerPrivate +0x90), this, float, float, float}. There is NO authority check. It is virtual and has a UFUNCTION exec stub, so FindFunction + ProcessEvent dispatches through the override — exactly the mechanism respawn.cpp:113-114 already relies on.
- CONFIRMED: AController::Possess (0x16ACD68) has exactly the claimed authority gate: `test byte ptr [rcx+0x338], 0x4` (bCanPossessWithoutAuthority) then `cmp byte ptr [rcx+0x168], 0x3` (Role == ROLE_Authority); failing both branches to the cold warning path and returns. Only past the gate does it call OnPossess (vtable+0x7D8), FindFunctionChecked + ProcessEvent (vtable+0x278) for ReceivePossess with an 8-byte APawn* param block, Broadcast on this+0x308, and ProcessMulticastDelegate at 0x16ACE2A on this+0x2D0 with a 16-byte {OldPawn, NewPawn} block.
- CONFIRMED: AESPlayerController::SetPawn (0x5E170A8) disassembles as claimed: calls APlayerController::SetPawn, requires NewPawn != null && this->Pawn(+0x2E8) == NewPawn && IsChildOf(AESPawn), stores the result at this+0xA40, then RemoveDynamic/AddDynamic on ESPawn->ShipMovement(+0x378)+0x978 and ESPawn->JumpDrive(+0x3B0)+0x2A8, binding AESPlayerController member functions. Nothing HUD-side. AController::OnRep_Pawn (0x493C740) exists, so this path does run on a client when the pawn replicates — the hook point for FIX 2b is real and reachable.
- CONFIRMED: AESHUD::Tick (0x13F2384) is world-marker work only: BeginCustomTimeScope, AActor::Tick, then gated on Owner(+0x158)!=0, HideMainLayerCounter(+0x49C)<=0 and UESGameInstance::InstancePointer->bCheatHideMarkers(+0x1D4)==0, a loop over USelfRegisteringComponent::GetAllActorsOfRegister(8) doing UHUDMarkerComponent::IsHiddenInHUD / occlusion tracing, then EndCustomTimeScope. It never touches the player's own hull, shield, armour, weapons, jump drive or devices.

## Finding (confidence: high )

### Root cause

Symptom 4 has TWO independent client-side causes; both were confirmed live on a real host+client session (host console 27100, client console 27101).

=== CAUSE A — the client's device/consumable SLOT ARRAYS are never built (state, not UI) ===
`UDeviceComponent::Init` (rva 0x17EFE5C, called only from `UDeviceComponent::InitializeComponent` rva 0x17EFDF0) and `UConsumableComponent::InitializeComponent` (rva 0x193DA7C) both gate on exactly one thing: `Owner is AESPawn && Owner->ShipData.Inventory != null` (`[pawn+0x4C8]` = AESPawn::ShipData 0x4B8 + FShipData::Inventory 0x10). No authority check anywhere. They then do `Inventory->GetItemsOfCategory(8=Device / 9=Consumable, false)` -> `CreateDeviceInfoFromItem` / `CreateConsumableInfoFromItem` -> assign `DeviceSlots` (0xA0) / `ConsumableSlots` (0xA0), and Init tail-calls `RespawnDevices` (0x17EFEDC) -> `SpawnDevice` (0x17F11A4) which spawns the `ADeviceBase` actors.

The mod fills the client's `AESPawn::ShipData` in a hook on `AESPawn::PostInitializeComponents` (loadout.cpp `H_PostInitComponents`, rva 0x1515EEC). That is ONE STEP TOO LATE. Disassembly of `AActor::PostActorConstruction` (rva 0x156A448) shows the order:
  0x156A49E  call [vtable+0x528]  -> PreInitializeComponents (AActor vtable slot 165)
  0x156A4C3  call 0x156A88C       -> AActor::InitializeComponents()   <-- UDeviceComponent/UConsumableComponent::InitializeComponent run HERE
  0x156A4FE  call [vtable+0x530]  -> PostInitializeComponents (slot 166)  <-- the mod's hook runs HERE
So on the client both components initialise while `ShipData.Inventory` is still null and leave their slot arrays at the Blueprint defaults. Measured live on the client: `Devices: 1 slot(s) [0] (empty)`, `Consumables: 0 slot(s)`, while `ShipData.Inventory` held `Devices=2 (teleporter, energized_boost)` and `Consumables=4 (nanobots_small,...)`. The host, whose ShipData is set at spawn, showed `Devices: 2 slots (teleporter, energized_boost)`, `Consumables: 4 slots`.
Consequence: on the client there is essentially nothing to select and nothing to cool down. `UInventoryLib::ReinitShipAfterPotentialChanges` (which the mod already calls) only refreshes EXISTING slots via `UDeviceComponent::ReInitNewDevices` (0x178EF0C) — it filled slot[0] with "teleporter" but could not grow the array from 1 to 2, and it never touches consumables at all.

=== CAUSE B — the HUD widgets are bound to a DEAD pawn's components (UI) ===
ES2's device/consumable HUD is Blueprint: `WG_Ingame_HUD_C` (outer = the persistent `BP_GameInstance_C`, reachable as `PC->MyHUD(0x358)->IngameHudWidget`) with two `WG_HUD_Equipment_C` children, `PrimariesAndDevices` and `SecondariesAndConsumables`. Those children CACHE object pointers resolved once in their `Construct`:
  WG_HUD_Equipment_C: WeaponComponent(0x398), DeviceComponent(0x3D0), ConsumableComponent(0x3D8), NumEquipmentSlots(0x3E0), EquipmentSlotWidgets(0x3C0)
  WG_Ingame_HUD_C:    PlayerPawn(0x508), DeviceWidgets(0x510), ConsumableWidgets(0x520)
  WG_HUD_EquipmentSlot_C: ItemRef(0x478), EquipmentRef(0x480 = the ADeviceBase actor), ConsumableCompRef(0x580), CooldownRatio(0x4B8), CoolingDown(0x545), Activated(0x546)
Everything the UI shows (selection highlight, cooldown ring, charges, amount) is read from, and event-bound to, those cached per-component multicast delegates (`UDeviceComponent::OnDeviceActivated` 0x108, `OnDeviceEnteredCooldown` 0x148, `OnDeviceLeftCooldown` 0x158; `UConsumableComponent::OnConsumableUsed` 0xE0, `OnStartedUsingConsumable` 0x100).
The widget lives under the GameInstance, so it SURVIVES the client's ClientTravel into the host's world and it survives the mod's loadout pawn swap (loadout.cpp `HostTick`: UnPossess + RestartPlayer + destroy the placeholder). Nothing in ES2 rebinds it, because a single-player ES2 never replaces the player pawn actor.
Measured live, client: live pawn = `BP_Ship_Player_C_2147422329` (0x9DC34030) with `Devices0`=0x12B250C00, `Consumables0`=0x12AE87080, `PrimaryWeapons0`=0x5AC27160; but `WG_Ingame_HUD_C_2147446806::PlayerPawn` = `BP_Ship_Player_C_2147447126` (0xC2A1C010) and `PrimariesAndDevices.DeviceComponent` = `BP_Ship_Player_C_2147447126.Devices0` (0x1175C1C00) — a different, dead pawn — with `NumEquipmentSlots=1` / `0` and `NumExcessSlots=4`.
On the HOST every one of those matched exactly (`PlayerPawn` = live pawn 0x97CA8020, `DeviceComponent` = 0xCC265200 = pawn->Devices, NumEquipmentSlots 2 / 4). That is the whole host-vs-client difference.

Nothing in the input->state chain is authority-gated: `InputStartUsingDevice<N>` (0x5DF4530/44AC/44D8/4504) -> `InputStartUsingDevice(int)` (0x2B3F444) -> `StartOrStopUsingDeviceOrConsumable(pawn=PC->ESPawn[0xA40], _, bIsMovementBlocked[0x897], idx, bStart, bIsDevice)` (0x16B900C) -> sets `SelectedDeviceIndex` (0xB0) -> `StartUsingSelectedDevice` (0x16B8EF0) -> `ToggleDevice` (0x16B98F4) -> `ADeviceBase::Activate` (vtable+0x748, 0x16B94D8) + `AddCooldown` (0x16B8F84, broadcasts OnDeviceEnteredCooldown) . The only gates are `bIsMovementBlocked` (PC+0x897), `JumpDrive->bIsInCruiseMode` (0x3B0 -> +0x318), `bWaitForDeviceRelease` (PC+0xC0D), `UGameplayLib::IsPlayerRemoteControlling` (0x14E53A0) and hull-depleted. All of them are false on a client. Cooldown ticks locally too: `ADeviceBase::Tick` (0x153F794) decrements `Cooldowns`(0x4B8)/`RemainingDeviceDuration`(0x520) with no authority check, and `UConsumableComponent::TickComponent` (0x17E5AA0) decrements `UItem::CooldownRemaining` (0x204).

### Evidence

- ORDERING PROOF (disassembly): tools/disasm.py 0x156A448 (AActor::PostActorConstruction) -> 0x156A49E `call qword ptr [rdx+0x528]` (PreInitializeComponents, AActor vtable slot 165), 0x156A4C3 `call 0x14156A88C` (AActor::InitializeComponents), 0x156A4FE `call qword ptr [rax+0x530]` (PostInitializeComponents, slot 166). Confirmed by `pdb_types.py vtable AActor`: [165] PreInitializeComponents, [166] PostInitializeComponents (165*8=0x528, 166*8=0x530). The mod hooks the LAST of the three.
- GATE PROOF (disassembly): UDeviceComponent::Init rva 0x17EFE5C — `mov rdi,[rcx+0x90]` (Owner), IsChildOf(AESPawn), `mov rcx,[rdi+0x4c8]; test rcx,rcx; jne <cold>` i.e. ShipData(0x4B8)+Inventory(0x10). Cold block 0x2A86BE1: UInventory::GetItemsOfCategory(cat=8) -> CreateDeviceInfoFromItem -> `lea rcx,[rbx+0xa0]` TArray<FDeviceInfo>::operator= (DeviceSlots), then tail-jmp to RespawnDevices 0x17EFEDC. UConsumableComponent::InitializeComponent rva 0x193DA7C has the identical gate; its cold block 0x2B21E19 uses GetItemsOfCategory(cat=9) and assigns ConsumableSlots at +0xA0. Neither has any HasAuthority/NetMode check.
- LIVE, CLIENT (console 27101, `shipdata devices`): pawn BP_Ship_Player_C_2147422329 -> `Devices: 1 slot(s) [0] (empty)`, `Consumables: 0 slot(s)`, while `Inventory.Devices=2 Consumables=4` (inv.dev[0] teleporter, inv.dev[1] energized_boost, inv.con[0] nanobots_small). `shipdata local` reports applied=1 prePostInit=3 preBeginPlay=0 localShipEmpty=0, i.e. the mod DID fill ShipData in PostInitializeComponents and it still was not early enough.
- LIVE, HOST (console 27100, `shipdata devices`): pawn BP_Ship_Player_C_2147473937 -> `Devices: 2 slot(s) [0] teleporter [1] energized_boost`, `Consumables: 4 slot(s) [0] nanobots_small ...`. Same save, same ship (ship_medium_sentinel). Host correct, client wrong.
- LIVE REPAIR PROOF for cause A: `call 0x000000012B250C00 ReInit` (UDeviceComponent::ReInit, a zero-arg UFunction; exec at rva 0x5A8F830 which re-reads Owner->ShipData.Inventory(+0x4C8), GetItemsOfCategory(8), rebuilds DeviceSlots and calls RespawnDevices at execReInit+0x1f7). Before: `Devices: 1 slot(s) [0] teleporter`. After: `Devices: 2 slot(s) [0] teleporter [1] energized_boost`. `actors DeviceBase` on the client then showed BP_Device_Teleporter_C and BP_Device_Energized_Boost_C instances (7 total, same count as the host).
- HUD BINDING PROOF, HOST baseline (27100): live pawn 0x97CA8020 has PrimaryWeapons0=0x1452CD10, SecondaryWeapons0=0x1452D740, Devices0=0xCC265200, Consumables0=0xCC2AFC80. `WG_Ingame_HUD_C_2147473490` (0x8A57B890, outer BP_GameInstance_C_2147482350) has PlayerPawn=0x97CA8020; child PrimariesAndDevices (0xC94CF250) has WeaponComponent=0x1452CD10, DeviceComponent=0xCC265200, NumEquipmentSlots=2; child SecondariesAndConsumables (0xC94CEDC0) has WeaponComponent=0x1452D740, ConsumableComponent=0xCC2AFC80, NumEquipmentSlots=4. Everything matches the live pawn.
- HUD BINDING PROOF, CLIENT (27101): live pawn 0x9DC34030 (BP_Ship_Player_C_2147422329, role=2 AutonomousProxy) with Devices0=0x12B250C00, Consumables0=0x12AE87080, PrimaryWeapons0=0x5AC27160. `WG_Ingame_HUD_C_2147446806` (0xB04CD060) had PlayerPawn=BP_Ship_Player_C_2147447126 (0xC2A1C010); PrimariesAndDevices (0x12CBBB270) had DeviceComponent=BP_Ship_Player_C_2147447126.Devices0 (0x1175C1C00) and WeaponComponent=...2147447126.PrimaryWeapons0, NumEquipmentSlots=1; SecondariesAndConsumables (0x12CBBDB80) had ConsumableComponent=...2147447126.Consumables0, NumEquipmentSlots=0, NumExcessSlots=4. All of them point at a pawn that is NOT the one the player is flying.
- LIVE REPAIR PROOF for cause B, step 1: `call 0x00000000B04CD060 ReInit` (WG_Ingame_HUD_C::ReInit, zero-arg BlueprintCallable). Before: PlayerPawn=None. After: PlayerPawn=BP_Ship_Player_C_2147422329 (0x9DC34030) = the live pawn. It also fixed WG_HUD_HealthBars_C::ESPawn (0x478) to 0x9DC34030 (relevant to symptom 1). It did NOT fix the equipment children.
- LIVE REPAIR PROOF for cause B, step 2: `call 0x12CBBB270 Construct` and `call 0x12CBBDB80 Construct` (WG_HUD_Equipment_C::Construct, flags 0x8020808, zero-arg). After: PrimariesAndDevices.WeaponComponent = BP_Ship_Player_C_2147422329.PrimaryWeapons0 (0x5AC27160), .DeviceComponent = ...2147422329.Devices0 (0x12B250C00); SecondariesAndConsumables.WeaponComponent = ...SecondaryWeapons0 (0x5ACDF5D0), .ConsumableComponent = ...Consumables0 (0x12AE87080). All now match the live pawn.
- COMBINED PROOF (A then B): after UDeviceComponent::ReInit and a second WG_HUD_Equipment_C::Construct, the client's PrimariesAndDevices.NumEquipmentSlots went 1 -> 2, matching the host. Order matters: the widget snapshots the component's slot count in Construct, so the component must be rebuilt first.
- WIDGET LOOKUP PATH (both sides): APlayerController::MyHUD (offset 0x358, already in sdk/gen/offsets.h) -> the AESHUD actor (BP_HUD_Ingame_C) -> its Blueprint property `IngameHudWidget` (observed at 0x4C8) -> the live WG_Ingame_HUD_C. Verified on host (0xCAA0AAB0 -> 0x8A57B890) and client (0x6B267CC0 -> 0xB04CD060).
- INPUT CHAIN (disassembly): AESPlayerController::InputStartUsingDevice<0..3> 0x5DF4530/0x5DF44AC/0x5DF44D8/0x5DF4504 -> InputStartUsingDevice(int) 0x2B3F444 passes (pawn=[pc+0xA40], _, [pc+0x897] bIsMovementBlocked, idx, bStart=1, bIsDevice=1) -> StartOrStopUsingDeviceOrConsumable 0x16B900C. Device branch: GetDeviceInstance(idx) 0x16B9A24, bounds-check against DeviceSlots.Num ([comp+0xA8]) and FDeviceInfo::SpawnedDevice ([data + 24*idx + 0x10]), writes SelectedDeviceIndex ([comp+0xB0]), calls StartUsingSelectedDevice 0x16B8EF0 -> ToggleDevice 0x16B98F4. Consumable branch (cold block 0x29FE48E): GetConsumableAmountOfSlot 0x5D77D74 > 0 -> SelectedConsumableIndex=idx -> StartUsingSelectedConsumable 0x5D7D9B0, else StopUsingSelectedConsumable 0x5D7E02C.
- SELECTION UI ENTRY POINTS: AESHUD::OnEnterOrExitDeviceSelection 0x1717A24 (ProcessEvent wrapper; the _Implementation is ICF-folded to the empty stub 0x12386C0, i.e. Blueprint-only) is called from InputStartUsingDevice(void) 0x1555908+0xED, InputStopUsingDevice(void) 0x1717984+0x65, NavigateEquipmentWheel 0x15556AC+0x19D and DelayedReOpenOfDeviceSelection 0x17167E8+0x94. Consumable twin: AESHUD::OnEnterOrExitConsumableSelection 0x5AF7898.
- COOLDOWN STATE: device — ADeviceBase::Cooldowns TArray<float> @0x4B8, CooldownIndex @0x51C, RemainingDeviceDuration @0x520, bIsActive @0x524, DeviceCooldownFactor @0x478, bManualCooldown @0x438, DeviceOwner @0x418, DeviceItem @0x420, DeviceType @0x2B0. Ticked by ADeviceBase::Tick 0x153F794 (calls ChangeCooldown 0x153F8E4 with -dt*factor; no authority check). Set by AddCooldown 0x16B8F84 which appends GetCooldownDuration() and then broadcasts UDeviceComponent::OnDeviceEnteredCooldown (comp+0x148). Read by GetRemainingCoolDownRatio 0x16B9280.
- COOLDOWN STATE: consumable — lives on the UItem, not the component: UItem::CooldownRemaining @0x204, UItem::CooldownLastSet @0x208 (set by UConsumableComponent::IncreaseCooldown 0x5D78DFC, decremented every frame by UConsumableComponent::TickComponent 0x17E5AA0 which walks ConsumableSlots at +0xA0 with stride 24).
- NO MOD INTERFERENCE: grep of mod/src for Device/Consumable finds hits only in loadout.cpp (the `shipdata devices` inspector and the ShipData fill). authority.cpp's spawn guard is OFF by default and does not list ADeviceBase. authority.cpp's AESHUD::Tick hook only skips when the local PC has no pawn, so the HUD does tick on the client.
- ICF SAFETY: rvas 0x1502050 (AESPawn::PreInitializeComponents), 0x17EFDF0 (UDeviceComponent::InitializeComponent) and 0x193DA7C (UConsumableComponent::InitializeComponent) each map to exactly one symbol in sdk/symbols.tsv — safe to hook/call.

### Proposed fix

Two client-only changes, both already validated at runtime on the live client. Do them in this order (state first, then UI — the widget snapshots the component slot count).

--- PART 1: build the client's device/consumable slots (fixes the state) ---

1a. Add to tools/gen_sdk.py RVAS and re-run scripts/gen-sdk.sh:
    'AESPawn_PreInitializeComponents':       'public: virtual void __cdecl AESPawn::PreInitializeComponents(void)',            # 0x1502050
    'UConsumableComponent_InitializeComponent':'public: virtual void __cdecl UConsumableComponent::InitializeComponent(void)', # 0x193DA7C
    'UDeviceComponent_InitializeComponent':  'public: virtual void __cdecl UDeviceComponent::InitializeComponent(void)',       # 0x17EFDF0
  and to OFFSETS: 'AESPawn': [... , 'Devices', 'Consumables', 'PrimaryWeapons', 'SecondaryWeapons'],
                  'ADeviceBase': ['Cooldowns','CooldownIndex','RemainingDeviceDuration','bIsActive','DeviceOwner','DeviceItem','DeviceType'],
                  'UItem': [... , 'CooldownRemaining','CooldownLastSet'],
                  'UDeviceComponent': [... , 'SavedDeviceSlots','HoldToUseDuration'].

1b. In mod/src/loadout.cpp, add a hook on AESPawn::PreInitializeComponents that runs the SAME body as the existing H_PostInitComponents (client role + LooksLikeOurShip + ShipItemInstance==null -> CopyOwnShipInto(pawn)), then calls the original. Keep the PostInitializeComponents hook as a backstop (it already no-ops when ShipItemInstance is non-null):

    static Fn_PawnVoid o_PreInitComponents = nullptr;
    static uint64_t g_prePreInit = 0;
    static void H_PreInitComponents(AActor* pawn) {
        if (pawn && coop::CurrentRole() == coop::Role::Client && g_autoLocalShip && LooksLikeOurShip(pawn)) {
            void* sd = reinterpret_cast<char*>(pawn) + es2off::AESPawn::ShipData;
            if (UE_FIELD(UObject*, sd, es2off::FShipData::ShipItemInstance) == nullptr && CopyOwnShipInto(pawn)) {
                ++g_prePreInit;
                LOGF("[loadout] filled our own ShipData on %s before PreInitializeComponents", GetName((UObject*)pawn).c_str());
            }
        }
        o_PreInitComponents(pawn);
    }
    // in OnInit():
    hooks::Install("AESPawn::PreInitializeComponents", es2rva::AESPawn_PreInitializeComponents,
                   (void*)&H_PreInitComponents, (void**)&o_PreInitComponents);

  Rationale, proven by disassembly of AActor::PostActorConstruction (0x156A448): PreInitializeComponents (0x156A49E) -> InitializeComponents (0x156A4C3) -> PostInitializeComponents (0x156A4FE). Filling at the first of the three is the only point where UDeviceComponent::Init and UConsumableComponent::InitializeComponent can see ShipData.Inventory.

1c. Add a repair path for pawns that still slip through (pawn already existed before the coop role was known, or the loadout blob arrived late). Put it in loadout.cpp next to ClientBuildWeaponsTick, keyed on the same "once per pawn" guard, and only when the slot count disagrees with the inventory:

    void ClientBuildEquipmentTick() {          // client role only
        APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
        AActor* pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
        if (!pawn || pawn == g_equipBuiltFor) return;
        UObject* inv = UE_FIELD(UObject*, (char*)pawn + es2off::AESPawn::ShipData, es2off::FShipData::Inventory);
        if (!inv || !IsValidObject(inv)) return;                   // ship not materialised yet
        UObject* dev = UE_FIELD(UObject*, pawn, es2off::AESPawn::Devices);
        UObject* con = UE_FIELD(UObject*, pawn, es2off::AESPawn::Consumables);
        struct RawArr { char* Data; int32_t Num; int32_t Max; };
        if (dev && IsValidObject(dev) &&
            UE_FIELD(RawArr, dev, es2off::UDeviceComponent::DeviceSlots).Num !=
            UE_FIELD(RawArr, inv, es2off::UInventory::Devices).Num) {
            if (UFunction* f = FindFunction(dev, "ReInit")) ProcessEvent(dev, f, nullptr);   // VERIFIED LIVE: 1 -> 2 slots + spawns ADeviceBase actors
        }
        if (con && IsValidObject(con) &&
            UE_FIELD(RawArr, con, es2off::UConsumableComponent::ConsumableSlots).Num !=
            UE_FIELD(RawArr, inv, es2off::UInventory::Consumables).Num) {
            // no UFunction exists for consumables; call the native virtual directly (unique RVA, no ICF)
            Rva<void(void*)>(es2rva::UConsumableComponent_InitializeComponent)(con);
        }
        g_equipBuiltFor = pawn;
        loadout::RebindHud();                                       // part 2, must run after this
    }

--- PART 2: rebind the HUD widgets to the live pawn (fixes the UI) ---

New mod/src/hud.cpp, client-only, driven from the existing per-frame client tick. All Blueprint members are looked up BY NAME via ue::FindProperty/GetProperties — never by the observed offsets (they move with content patches).

    static AActor* g_lastPawn = nullptr;
    static int     g_retries  = 0;

    static UObject* PropObj(UObject* o, const char* name) {
        if (!o || !IsValidObject(o)) return nullptr;
        FProperty* p = FindProperty((UStruct*)GetClass(o), name);   // or scan GetProperties()
        if (!p) return nullptr;
        return UE_FIELD(UObject*, o, <offset of p>);
    }
    static void CallBP(UObject* o, const char* fn) {
        if (!o || !IsValidObject(o)) return;
        if (UFunction* f = FindFunction(o, fn)) ProcessEvent(o, f, nullptr);
    }

    void RebindHud() {
        UWorld* w = GetWorld();
        APlayerController* pc = GetFirstLocalPlayerController(w);
        if (!pc) return;
        AActor* hud = UE_FIELD(AActor*, pc, es2off::APlayerController::MyHUD);   // 0x358
        UObject* wgt = PropObj((UObject*)hud, "IngameHudWidget");                // BP_HUD_Ingame_C
        if (!wgt) { if (g_retries++ < 30) g_lastPawn = nullptr; return; }        // retry next tick
        CallBP(wgt, "ReInit");                                                   // VERIFIED: re-resolves WG_Ingame_HUD_C::PlayerPawn and WG_HUD_HealthBars_C::ESPawn
        for (const char* child : { "PrimariesAndDevices", "SecondariesAndConsumables", "EquipmentWheel" })
            CallBP(PropObj(wgt, child), "Construct");                             // VERIFIED: re-resolves WeaponComponent / DeviceComponent / ConsumableComponent and NumEquipmentSlots
        LOGF("[hud] rebound the ingame HUD to %s", GetName((UObject*)UE_FIELD(AActor*, pc, es2off::AController::Pawn)).c_str());
    }

    void ClientTick() {                       // call from the existing client tick
        if (coop::CurrentRole() != coop::Role::Client) return;
        APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
        AActor* pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
        if (!pawn || pawn == g_lastPawn) return;
        g_lastPawn = pawn; g_retries = 0;
        // defer ~0.5 s so the components have been built (part 1) before the widgets snapshot them
        ScheduleAfter(0.5, &RebindHud);
    }

  This must fire on BOTH pawn transitions a client goes through, and the poll covers both automatically:
    (i) at join — the client ClientTravels into the host's map, but WG_Ingame_HUD_C is outered to the persistent BP_GameInstance_C and survives, still holding the pre-connect pawn (measured: PlayerPawn = BP_Ship_Player_C_2147447126 while the live pawn was ..._2147422329);
    (ii) ~1 min later, when loadout.cpp's HostTick does UnPossess + RestartPlayer + K2_DestroyActor on the placeholder and the client is possessed by a second pawn.
  Also call RebindHud() after the co-op `goto`/travel reconnect.

--- PART 3: route device/consumable ACTIVATION to the host (answer to (d)) ---
Devices must stay LOCAL for the UI (selection, cooldown, charges all run with zero authority checks on the client, as proven above), but their world effect must run host-side. Mirror combat.cpp's fire forwarding:
  * client: in a hook on UDeviceComponent::ToggleDevice (0x16B98F4) / UConsumableComponent::StartUsingSelectedConsumable (0x5D7D9B0), when coop role == Client and the component's owner is our own pawn, coop::SendToServer(Format("DEV|%d", idx))  /  "CON|%d".
  * host: in net.cpp OnServerOp, resolve players::ByController(from)->pc->Pawn and call
      Rva<void(AActor*,void*,bool,int,bool,bool)>(es2rva::StartOrStopUsingDeviceOrConsumable)(pawn, nullptr, false, idx, true, /*bIsDevice=*/true);
    (rva 0x16B900C; the 2nd argument is never read by the callee — verified by disassembly — so nullptr is safe). Add the RVA to gen_sdk.py as 'StartOrStopUsingDeviceOrConsumable'.
  * keep ApplyESPointDamage no-oped in the client role and add "DeviceBase" to authority.cpp's g_blockBases only if a device is observed spawning gameplay actors client-side; otherwise the local activation stays cosmetic.
Do NOT stop running the local ToggleDevice — that is what produces the client's own cooldown and the OnDeviceEnteredCooldown broadcast the HUD listens to.

### Risks

- Blueprint offsets move with any ES2 content patch. Everything in Part 2 (IngameHudWidget, PlayerPawn, PrimariesAndDevices, SecondariesAndConsumables, DeviceComponent, ConsumableComponent, NumEquipmentSlots) MUST be resolved by property NAME through ue::FindProperty/GetProperties and by UFunction name through ue::FindFunction — the offsets quoted in the evidence are for identification only. If a lookup fails, log and skip; never poke a raw offset.
- Re-running WG_HUD_Equipment_C::Construct re-executes a Blueprint graph. In this session it was called twice on the same widget with no ill effect and EquipmentSlotWidgets stayed a fixed set of 4 named WidgetTree children (no dynamic allocation, no leak). UE's EX_AddMulticastDelegate uses AddUnique semantics so delegate bindings do not multiply, but this should be re-checked if a future ES2 build changes the graph.
- ProcessEvent on Blueprint functions must run on the game thread. Drive RebindHud from the existing per-frame client tick (the same place ClientBuildWeaponsTick runs), not from the console/net thread.
- Calling UConsumableComponent::InitializeComponent a second time replaces ConsumableSlots wholesale via TArray::operator=. Any per-slot runtime state (a consumable mid-use) would be discarded. Only call it when the slot count disagrees with ShipData.Inventory->Consumables.Num, which on a client is only ever at spawn/rebuild time.
- Moving the ShipData fill into AESPawn::PreInitializeComponents makes UInventoryLib::GetCurrentShip() (a heavy call that rebuilds an inventory) run slightly earlier in the actor's construction. It already runs a few microseconds later today with the re-entrancy guard in place, so this should be safe, but the g_inMaterialise re-entrancy guard in loadout.cpp must stay armed for the new hook too.
- Part 3 (routing) makes a consumable be consumed on BOTH machines' copies of the player: the client decrements its own UPlayerData/ship inventory locally, and the host decrements its substituted copy of that client's ship. For devices there is no resource except UPlayerData::OnDeviceUsed(FName) stat tracking (per-machine, harmless). Consumable amounts need a decision: either suppress the local UseConsumable's item consumption on the client, or suppress it on the host — do not leave both.
- Two modules must never hook the same RVA (MinHook refuses the second). AESPawn::PreInitializeComponents, UDeviceComponent::ToggleDevice and UConsumableComponent::StartUsingSelectedConsumable are currently unhooked; check hooks.cpp before adding.
- The measurements were taken while sibling agents were driving the same two game instances; the host was relaunched twice mid-investigation. The host/client A/B (PlayerPawn and DeviceComponent matching vs. not matching the live pawn) and the three repair calls were each reproduced on a single stable session, but a clean re-run of scripts/coop-session.sh --kill after implementing the fix is still warranted.
