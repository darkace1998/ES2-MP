// Host-authoritative combat.
//
// The client never damages anything itself: its weapons only exist locally for feedback. Instead the
// client forwards *fire intent* to the host, and the host presses the trigger on that player's
// server-side PlayerController — so the real shots, hits, damage and kills all happen in the host's
// simulation and reach every player through normal actor replication.
//
// Health is a plain float ratio (UHitpointComponent::HitpointRatio, 0..1) that ES2 does not replicate;
// the host broadcasts the ratio of each player's ship so partners can see each other's condition.
#include "combat.h"
#include "coop.h"
#include "players.h"
#include "console.h"
#include "log.h"
#include "hooks.h"
#include <windows.h>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <map>
#include <vector>
#include <array>
#include <algorithm>
#include <functional>
#include <cstring>

using namespace ue;
using es2coop::Format;

namespace combat {

using Fn_PCVoid = void (*)(APlayerController*);

static Fn_PCVoid o_StartFirePrimary = nullptr, o_StopFirePrimary = nullptr;
static Fn_PCVoid o_StartFireSecondary = nullptr, o_StopFireSecondary = nullptr;

static bool g_routeFire = true;      // client: forward fire intent to the host
static bool g_localFire = true;      // client: also fire locally (muzzle flashes / feedback only)
static uint64_t g_fireSent = 0, g_fireApplied = 0;
static double g_healthAccum = 0;
// Nothing gates shield regeneration on a client (UShieldComponent::TickRegeneration has no authority
// check), so the client refills its own shield between updates. Mirror fast enough that the
// authoritative value wins instead of the bar jittering against local regen.
static float g_healthHz = 10.f;
// Set by the local fire routing: aim only needs to be exact while the trigger is actually down.
static bool g_firingNow = false;

// ---------------------------------------------------------------- helpers
static APlayerController* LocalPC() { return GetFirstLocalPlayerController(GetWorld()); }

// AActor::OwnedComponents is a TSet (not a TArray), so instead we resolve the component through the
// actor's own reflected ObjectProperties, which is both simpler and stable across Blueprint layouts.
static UObject* FindComponentOfClass(AActor* actor, const char* className) {
    if (!actor) return nullptr;
    UClass* want = FindClass(className);
    if (!want) return nullptr;
    for (auto& p : GetProperties((UStruct*)GetClass((UObject*)actor), true)) {
        if (p.TypeName != "ObjectProperty") continue;
        UObject* v = UE_FIELD(UObject*, actor, p.Offset);
        if (v && IsValidObject(v) && IsA(v, want)) return v;
    }
    return nullptr;
}

// Hull, shield and armour are all UHitpointComponent subclasses, so HitpointRatio sits at the same
// offset on each. -1 means "this pawn has no such component".
static float GetRatioOf(AActor* pawn, const char* cls) {
    UObject* c = FindComponentOfClass(pawn, cls);
    if (!c) return -1.f;
    return UE_FIELD(float, c, es2off::UHealthComponent::HitpointRatio);
}
static float GetHealthRatio(AActor* pawn) { return GetRatioOf(pawn, "HealthComponent"); }
static float GetShieldRatio(AActor* pawn) { return GetRatioOf(pawn, "ShieldComponent"); }
static float GetArmorRatio(AActor* pawn)  { return GetRatioOf(pawn, "ArmorComponent"); }

// Drive a hitpoint component through the engine's own setter. Writing HitpointRatio directly leaves
// the UI stale (respawn.cpp relies on the same thing): the widgets refresh off the delegates that
// SetCurrentHitpointsWithRatio broadcasts, and a raw field write fires none of them.
// Note the setter does NOT run any depletion/death path — those live in TakeDamage/ChangeHitpoints —
// so mirroring a 0.0 hull to a client empties the bar without triggering the local game-over flow.
// The host stays the sole authority on death and respawn.
static void ApplyRatio(AActor* pawn, const char* cls, float ratio) {
    if (!pawn || ratio < 0.f) return;
    UObject* c = FindComponentOfClass(pawn, cls);
    if (!c) return;
    float cur = UE_FIELD(float, c, es2off::UHealthComponent::HitpointRatio);
    if (fabsf(cur - ratio) < 0.0005f) return;      // don't broadcast when nothing moved
    UFunction* fn = FindFunction(c, "SetCurrentHitpointsWithRatio");
    if (fn) { float r = ratio; ProcessEvent(c, fn, &r); }
    else UE_FIELD(float, c, es2off::UHealthComponent::HitpointRatio) = ratio;
}

// ---------------------------------------------------------------- fire routing
// which: 0 = primary, 1 = secondary ;  down: 1 = pressed, 0 = released
static void ApplyFireOnHost(APlayerController* pc, int which, bool down) {
    if (!pc) return;
    Fn_PCVoid fn = which == 0 ? (down ? o_StartFirePrimary : o_StopFirePrimary)
                              : (down ? o_StartFireSecondary : o_StopFireSecondary);
    // The trampolines are only valid once the hooks are installed; fall back to the raw addresses.
    if (!fn) {
        uint32_t rva = which == 0 ? (down ? es2rva::AESPlayerController_InputStartFirePrimary : es2rva::AESPlayerController_InputStopFirePrimary)
                                  : (down ? es2rva::AESPlayerController_InputStartFireSecondary : es2rva::AESPlayerController_InputStopFireSecondary);
        fn = Rva<std::remove_pointer_t<Fn_PCVoid>>(rva);
    }
    fn(pc);
    ++g_fireApplied;
}

static void SendFire(int which, bool down) {
    g_firingNow = down;
    if (!g_routeFire) return;
    coop::SendToServer(Format("F|%d|%d", which, down ? 1 : 0));
    ++g_fireSent;
}

static bool IsClient() { return coop::CurrentRole() == coop::Role::Client; }

static void H_StartFirePrimary(APlayerController* pc) {
    if (IsClient() && pc == LocalPC()) { SendFire(0, true); if (!g_localFire) return; }
    o_StartFirePrimary(pc);
}
static void H_StopFirePrimary(APlayerController* pc) {
    if (IsClient() && pc == LocalPC()) { SendFire(0, false); if (!g_localFire) return; }
    o_StopFirePrimary(pc);
}
static void H_StartFireSecondary(APlayerController* pc) {
    if (IsClient() && pc == LocalPC()) { SendFire(1, true); if (!g_localFire) return; }
    o_StartFireSecondary(pc);
}
static void H_StopFireSecondary(APlayerController* pc) {
    if (IsClient() && pc == LocalPC()) { SendFire(1, false); if (!g_localFire) return; }
    o_StopFireSecondary(pc);
}

// ---------------------------------------------------------------- NPC fire mirroring
//
// NPC weapons exist on the client (their actors spawn locally as children of the replicated pawn) but
// nothing ever pulls their trigger: the AI runs only on the host and ES2 replicates none of it, so on a
// client the enemies look inert while the host sees them shooting. Mirror just the trigger.
//
// Cross-machine identity is the hard part — the same NPC is a different UObject on each machine. UE
// already solves it: replicated actors carry a NetGUID assigned by the server's FNetGUIDCache, and the
// client can resolve it back through its own driver's cache.
using Fn_WeaponVoid = void (*)(UObject* weaponComponent);
// FNetworkGUID is 8 bytes but NOT trivially copyable, so it is returned through a hidden pointer.
// Member function => (this=RCX, sret=RDX, args...). Getting this wrong put the UObject* into the sret
// slot, which is what faulted inside FNetGUIDCache::SupportsObject earlier.
using Fn_GetNetGUID = void (*)(void* guidCache, uint64_t* sretGuid, const UObject* obj);
using Fn_GetObjectFromNetGUID = UObject* (*)(void* guidCache, const uint64_t* guid, bool ignoreDeleted);

static Fn_WeaponVoid o_StartFire = nullptr, o_StopFire = nullptr;
static bool g_mirrorNpcFire = true;
static uint64_t g_npcFireSent = 0, g_npcFireApplied = 0, g_npcFireUnresolved = 0, g_npcFireDeduped = 0;
// The AI calls StartFire/StopFire far more often than the state actually changes (8000+ calls in half a
// minute), which would swamp a reliable channel. Only transitions are sent.
static std::map<UObject*, bool> g_npcFireState;
// client: our local copy of a mirrored NPC's weapon component -> the aim the host reports for it.
// Applied every tick, because the client's own SmoothedAutoaim keeps overwriting the field with a value
// derived from an AI that is not running here.
static std::map<UObject*, FVector> g_npcAim;
// The auto-aim target is recomputed by SmoothedAutoaim every tick just like FocusLocation, so it is
// stamped the same way rather than set once. Keyed per weapon category for players, per component for
// mirrored NPCs. A reported guid of 0 means "no target" and is applied as such; a non-zero guid we
// cannot resolve leaves the local value alone, since clearing it would be strictly worse.
static std::map<int, std::array<uint64_t, 2>> g_playerAutoAim;   // playerId -> guid per category
static std::map<int, std::array<uint64_t, 2>> g_playerLock;       // playerId -> locked target guid per category
static std::map<UObject*, uint64_t> g_npcLock;
static std::map<UObject*, uint64_t> g_npcAutoAim;
static bool g_autoAimSync = true;
static uint64_t g_autoAimApplied = 0;
static double g_npcAimAccum = 0;
static float g_npcAimHz = 6.f;
static bool g_npcAimSync = true;
static bool g_lockSync = true;
// A client regenerates its own shield locally (UShieldComponent::TickRegeneration has no authority
// check), while the host streams the authoritative value. The two fight and the bar visibly saws up and
// down. On a client the host is the only thing that should move hitpoints, so the local regen is off.
using Fn_TickRegen = void (*)(UObject* comp, AActor* a, UObject* core, float f1, float f2, float f3);
static Fn_TickRegen o_TickRegen = nullptr;
static bool g_blockClientRegen = true;
static uint64_t g_regenBlocked = 0;
static void H_TickRegeneration(UObject* comp, AActor* a, UObject* core, float f1, float f2, float f3) {
    if (g_blockClientRegen && coop::CurrentRole() == coop::Role::Client) { ++g_regenBlocked; return; }
    o_TickRegen(comp, a, core, f1, f2, f3);
}

static bool g_aimAtTarget = true;   // host: aim at its own copy of the target, not the client's point
static uint64_t g_aimRetargeted = 0;      // mirror the locked target as well as the aim point
static size_t g_npcAimCursor = 0;            // round-robin so a big fight cannot flood one tick
static uint64_t g_npcAimSent = 0, g_npcAimApplied = 0;
static constexpr size_t kNpcAimPerTick = 4;


static void* LocalGuidCache() {
    UNetDriver* nd = GetNetDriver(GetWorld());
    return nd ? UE_FIELD(void*, nd, es2off::UNetDriver::GuidCache) : nullptr;   // TSharedPtr: object first

}

using Fn_GetLockedTarget = AActor* (*)(const UObject* wc);
using Fn_SetLockedTarget = void (*)(UObject* wc, AActor* target);

static AActor* GetLock(UObject* wc) {
    return Rva<std::remove_pointer_t<Fn_GetLockedTarget>>(es2rva::UWeaponComponent_GetLockedTarget)(wc);
}
// Go through ES2's own setter rather than poking LockedTarget: it drives the missile-lock timer and
// broadcasts OnNewTargetLocked / OnTargetUnLocked, which the HUD and the homing logic both rely on.
static void SetLock(UObject* wc, AActor* target) {
    Rva<std::remove_pointer_t<Fn_SetLockedTarget>>(es2rva::UWeaponComponent_SetLockedTarget)(wc, target);
}

// NetGUID of an actor as this machine knows it. Both sides agree on the value — the server assigns it
// and the client learns it through its own driver's cache — so it is the identity a lock travels as.
static uint64_t NetGuidOf(const UObject* actor) {
    void* cache = LocalGuidCache();
    if (!cache || !actor) return 0;
    uint64_t guid = 0;
    Rva<std::remove_pointer_t<Fn_GetNetGUID>>(es2rva::FNetGUIDCache_GetNetGUID)(cache, &guid, actor);
    return guid;
}
// Resolving a NetGUID back to an actor is only dependable on a CLIENT: that is the direction a client
// needs, so its cache keeps the guid -> object map populated. A server mostly needs object -> guid, and
// asking it the other way returns whatever happens to be there — observed handing back
// Default__BP_Outlaw_Scout_C (a class default object!) for a guid that genuinely belonged to a turret.
// Acting on that meant locking onto a CDO, which is why a client's lock never appeared on the host.
//
// So: try the cache, but only trust an answer that round-trips back to the same guid. Otherwise fall
// back to scanning actors and matching on the object -> guid direction, which is reliable on both sides.
// The scan only runs when a lock actually changes, not per tick.
static uint64_t g_guidScans = 0;
static AActor* ActorFromNetGuid(uint64_t guid) {
    if (!guid) return nullptr;
    void* cache = LocalGuidCache();
    if (!cache) return nullptr;
    uint64_t g = guid;
    UObject* o = Rva<std::remove_pointer_t<Fn_GetObjectFromNetGUID>>(es2rva::FNetGUIDCache_GetObjectFromNetGUID)(cache, &g, false);
    if (o && IsValidObject(o) && !(GetObjectFlags(o) & 0x10 /*RF_ClassDefaultObject*/)) {
        uint64_t back = 0;
        Rva<std::remove_pointer_t<Fn_GetNetGUID>>(es2rva::FNetGUIDCache_GetNetGUID)(cache, &back, o);
        if (back == guid) return (AActor*)o;
    }
    ++g_guidScans;
    AActor* found = nullptr;
    UClass* actorClass = FindClass("Actor");
    ForEachObject([&](UObject* obj) {
        if (!actorClass || !IsA(obj, actorClass)) return true;
        if (GetObjectFlags(obj) & 0x10) return true;                 // never a class default object
        uint64_t back = 0;
        Rva<std::remove_pointer_t<Fn_GetNetGUID>>(es2rva::FNetGUIDCache_GetNetGUID)(cache, &back, obj);
        if (back == guid) { found = (AActor*)obj; return false; }
        return true;
    });
    return found;
}

// Apply a locked target that arrived over the wire.
//
// This has to run every tick, not once on change: on the host the client's weapon component has no
// player driving it, and ES2's own tick clears the lock again immediately — measured, the host applied
// it 1562 times and still read "none" on every sample. So it is stamped at the top of each tick like
// FocusLocation, and only when it differs from what is currently there.
//
// SetLockedTarget is still the right way in (it owns the weak-pointer encoding and the OnNewTargetLocked
// / OnTargetUnLocked events), but it also restarts the missile lock — which, re-applied every tick,
// would pin the lock at zero and no missile would ever acquire. So the timer is carried across.
static uint64_t g_lockApplied = 0;
static void ApplyLock(UObject* wc, uint64_t guid) {
    if (!wc || !IsValidObject(wc)) return;
    // Attempt only when the REPORTED target changes. ES2 will not necessarily keep it (see below), and
    // retrying every tick meant calling SetLockedTarget ~60x/second to no effect.
    // Retry every tick. An earlier "only attempt when the reported guid changes" guard was the whole
    // reason this looked impossible: the first attempt resolved to a class default object (see
    // ActorFromNetGuid) and the guard then refused to ever try again. `want == GetLock(wc)` below is the
    // real guard — once the lock is in place this costs a comparison and nothing else.
    AActor* want = ActorFromNetGuid(guid);
    if (!want && guid != 0) return;              // unresolvable here: leave whatever is there alone
    if (want == GetLock(wc)) return;
    // SetLockedTarget restarts the missile lock, so carry the timer across.
    float remaining = UE_FIELD(float, wc, es2off::UWeaponComponent::RemainingMissileLockTime);
    SetLock(wc, want);
    UE_FIELD(float, wc, es2off::UWeaponComponent::RemainingMissileLockTime) = remaining;
    ++g_lockApplied;
}


// Identity across machines: the same NPC is a different UObject on each side, but UE already solved this
// — replicated actors carry a NetGUID from the server's FNetGUIDCache, and the client resolves it through
// its own driver's cache.
static void MirrorNpcFire(UObject* wc, bool down) {
    if (!g_mirrorNpcFire || coop::CurrentRole() != coop::Role::Host || !wc) return;
    AActor* owner = UE_FIELD(AActor*, wc, es2off::UActorComponent::OwnerPrivate);
    if (!owner || !IsValidObject((UObject*)owner)) return;
    if ((UE_FIELD(uint8_t, owner, es2off::AActor::bActorIsBeingDestroyed_off) & es2off::AActor::bActorIsBeingDestroyed_mask) != 0) return;
    if (players::ByPawn(owner)) return;                    // a player's own weapons already work
    if (players::Count() < 2) return;                      // nobody to tell

    auto it = g_npcFireState.find(wc);
    if (it != g_npcFireState.end() && it->second == down) { ++g_npcFireDeduped; return; }
    g_npcFireState[wc] = down;

    void* cache = LocalGuidCache();
    if (!cache) return;
    uint64_t guid = 0;
    Rva<std::remove_pointer_t<Fn_GetNetGUID>>(es2rva::FNetGUIDCache_GetNetGUID)(cache, &guid, (const UObject*)owner);
    if (!guid) return;                                     // not replicated to anyone yet
    int cat = (int)UE_FIELD(uint8_t, wc, es2off::UWeaponComponent::WeaponCategory);
    ++g_npcFireSent;
    coop::SendToAllClients(Format("WF|%llu|%d|%d", (unsigned long long)guid, cat, (int)down));
}

// A client must never apply damage: the host owns the simulation. Left unblocked, mirrored NPC fire makes
// projectiles impact locally and run ES2's damage path, which calls
// AESGameModeBase::CheckForItemDamageChangingEffects_BP on a game mode that does not exist on a client —
// an instant crash. FDamageInfo is a 12-byte POD, so the no-op just returns a zeroed one.
// The detour deliberately declares fewer parameters than the caller passes: MS x64 is caller-cleanup, so
// that is safe, but it also means we cannot forward — hence the hook is only ENABLED while we are a client.
static bool g_damageBlockOn = false;
static void* H_ApplyESPointDamage(void* sretDamageInfo) {
    if (sretDamageInfo) memset(sretDamageInfo, 0, 12);
    return sretDamageInfo;
}
// Radial damage reaches the same dead end by a different road: AProjectileBase::OnImpact -> Explode ->
// ApplyESRadialDamage -> AWeaponBase::CheckForItemDamageChangingEffects ->
// AESGameModeBase::CheckForItemDamageChangingEffects_BP on a null game mode. Blocking only the point
// path left every explosive impact near a client fatal (observed: AV reading 0x10 with that stack).
// Returns bool rather than a struct, so the no-op is just "false" — no sret to fill.
static bool H_ApplyESRadialDamage() { return false; }
void SetClientDamageBlock(bool on) {
    if (g_damageBlockOn == on) return;
    g_damageBlockOn = on;
    hooks::Enable("UGameplayLib::ApplyESPointDamage", on);
    hooks::Enable("UGameplayLib::ApplyESRadialDamage", on);
    LOGF("[combat] client-side damage %s", on ? "BLOCKED (host is authoritative)" : "allowed");
}

static void H_StartFire(UObject* wc) { o_StartFire(wc); MirrorNpcFire(wc, true); }
static void H_StopFire(UObject* wc)  { o_StopFire(wc);  MirrorNpcFire(wc, false); }

// Resolve the weapon component a mirrored message refers to: NetGUID -> our own copy of that actor ->
// the component for that weapon category.
static UObject* ResolveNpcWeapon(unsigned long long guid, int cat) {
    void* cache = LocalGuidCache();
    if (!cache) return nullptr;
    uint64_t g = guid;
    UObject* obj = Rva<std::remove_pointer_t<Fn_GetObjectFromNetGUID>>(es2rva::FNetGUIDCache_GetObjectFromNetGUID)(cache, &g, false);
    if (!obj || !IsValidObject(obj)) return nullptr;
    const char* prop = cat == 0 ? "PrimaryWeapons" : "SecondaryWeapons";
    for (auto& p : GetProperties((UStruct*)GetClass(obj), true))
        if (p.Name == prop) {
            UObject* wc = UE_FIELD(UObject*, obj, p.Offset);
            return (wc && IsValidObject(wc)) ? wc : nullptr;
        }
    return nullptr;
}

static bool ApplyNpcAim(const std::string& body) {
    unsigned long long guid = 0, lock = 0, aa = 0; int cat = 0; double x = 0, y = 0, z = 0;
    int n = sscanf(body.c_str(), "%llu|%d|%lf|%lf|%lf|%llu|%llu", &guid, &cat, &x, &y, &z, &lock, &aa);
    if (n < 5) return true;
    UObject* wc = ResolveNpcWeapon(guid, cat);
    if (!wc) return true;
    g_npcAim[wc] = FVector{x, y, z};
    if (n >= 6) g_npcLock[wc] = lock;               // stamped per tick, see ApplyLock
    if (n >= 7) g_npcAutoAim[wc] = aa;
    return true;
}

static bool ApplyNpcFire(const std::string& body) {
    unsigned long long guid = 0; int cat = 0, down = 0;
    if (sscanf(body.c_str(), "%llu|%d|%d", &guid, &cat, &down) != 3) return true;
    void* cache = LocalGuidCache();
    if (!cache) return true;
    uint64_t g = guid;
    UObject* obj = Rva<std::remove_pointer_t<Fn_GetObjectFromNetGUID>>(es2rva::FNetGUIDCache_GetObjectFromNetGUID)(cache, &g, false);
    AActor* best = (obj && IsValidObject(obj)) ? (AActor*)obj : nullptr;
    if (!best) { ++g_npcFireUnresolved; return true; }
    const char* prop = cat == 0 ? "PrimaryWeapons" : "SecondaryWeapons";
    UObject* wc = nullptr;
    for (auto& p : GetProperties((UStruct*)GetClass((UObject*)best), true))
        if (p.Name == prop) { wc = UE_FIELD(UObject*, best, p.Offset); break; }
    if (!wc || !IsValidObject(wc)) { ++g_npcFireUnresolved; return true; }
    if (!down) { g_npcAim.erase(wc); g_npcAutoAim.erase(wc); g_npcLock.erase(wc); }                        // stop tracking a weapon that went quiet
    if (down) o_StartFire(wc); else o_StopFire(wc);        // originals: never re-enter our own hook
    ++g_npcFireApplied;
    return true;
}

// ---------------------------------------------------------------- aim sync
//
// The host is the one that actually pulls a client's trigger, but on the host the client's weapon
// component has no player behind it: SmoothedAutoaim's inputs come from a controller with no camera or
// crosshair, and it writes FocusLocation as NaN. Measured on a live session — client focus
// (203550, -62014, 33505) vs the host's copy (nan, nan, nan). AWeaponBase::GetAimDirection reads exactly
// that field, so the authoritative shot had no idea where the player was pointing.
//
// So the client streams its own FocusLocation and the host stamps it into the server-side components
// each tick, just before ES2 uses it. SmoothedAutoaim overwrites the field again later in the same
// tick; we simply write it again next tick, which is enough because the shot is taken before that.
using Fn_TickComponent = void (*)(UObject* comp, float dt, int tickType, void* tickFn);
static Fn_TickComponent o_WeaponTick = nullptr;
static std::map<int, FVector> g_aim;        // host: playerId -> the aim point that player reports
static double g_aimAccum = 0;
static float g_aimHz = 20.f;      // while firing
static float g_aimIdleHz = 4.f;   // otherwise: enough to orient weapons, far less channel pressure
static bool g_aimSync = true;
static uint64_t g_aimSent = 0, g_aimApplied = 0;

// Stamp an auto-aim target reported over the wire onto a weapon component.
static void ApplyAutoAim(UObject* wc, uint64_t guid) {
    if (!g_autoAimSync || !wc) return;
    if (guid == 0) {
        UE_FIELD(AActor*, wc, es2off::UWeaponComponent::CurrentAutoAimTarget) = nullptr;
        ++g_autoAimApplied;
        return;
    }
    if (AActor* t = ActorFromNetGuid(guid)) {
        UE_FIELD(AActor*, wc, es2off::UWeaponComponent::CurrentAutoAimTarget) = t;
        ++g_autoAimApplied;
    }
}

static bool IsFinite3(const FVector& v) {
    return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
}

// Every weapon component on a pawn (primary AND secondary), by reflected object property.
static void ForEachWeaponComponent(AActor* pawn, const std::function<void(UObject*)>& fn) {
    if (!pawn || !IsValidObject((UObject*)pawn)) return;
    UClass* want = FindClass("WeaponComponent");
    if (!want) return;
    for (auto& p : GetProperties((UStruct*)GetClass((UObject*)pawn), true)) {
        if (p.TypeName != "ObjectProperty") continue;
        UObject* v = UE_FIELD(UObject*, pawn, p.Offset);
        if (v && IsValidObject(v) && IsA(v, want)) fn(v);
    }
}

// Host: NPC aim. Only components whose trigger is currently down are worth sending, and only a few per
// tick — a busy fight has plenty of shooters and this shares the reliable channel with everything else.
static void HostNpcAimTick(float dt) {
    if (!g_npcAimSync || !g_mirrorNpcFire || players::Count() < 2) return;
    g_npcAimAccum += dt;
    if (g_npcAimAccum < 1.0 / g_npcAimHz) return;
    g_npcAimAccum = 0;

    // collect the live, currently-firing components (and drop dead entries while we are here)
    std::vector<UObject*> firing;
    for (auto it = g_npcFireState.begin(); it != g_npcFireState.end(); ) {
        if (!it->first || !IsValidObject(it->first)) { it = g_npcFireState.erase(it); continue; }
        if (it->second) firing.push_back(it->first);
        ++it;
    }
    if (firing.empty()) { g_npcAimCursor = 0; return; }

    void* cache = LocalGuidCache();
    if (!cache) return;
    for (size_t n = 0; n < kNpcAimPerTick && n < firing.size(); ++n) {
        UObject* wc = firing[(g_npcAimCursor + n) % firing.size()];
        AActor* owner = UE_FIELD(AActor*, wc, es2off::UActorComponent::OwnerPrivate);
        if (!owner || !IsValidObject((UObject*)owner)) continue;
        const FVector& f = UE_FIELD(FVector, wc, es2off::UWeaponComponent::FocusLocation);
        if (!IsFinite3(f)) continue;
        uint64_t guid = 0;
        Rva<std::remove_pointer_t<Fn_GetNetGUID>>(es2rva::FNetGUIDCache_GetNetGUID)(cache, &guid, (const UObject*)owner);
        if (!guid) continue;
        int cat = (int)UE_FIELD(uint8_t, wc, es2off::UWeaponComponent::WeaponCategory);
        uint64_t lock = 0, aa = 0;
        if (AActor* t = GetLock(wc)) lock = NetGuidOf((const UObject*)t);
        AActor* a = UE_FIELD(AActor*, wc, es2off::UWeaponComponent::CurrentAutoAimTarget);
        if (a && IsValidObject((UObject*)a)) aa = NetGuidOf((const UObject*)a);
        coop::SendToAllClients(Format("WA|%llu|%d|%.1f|%.1f|%.1f|%llu|%llu", (unsigned long long)guid, cat,
                                      f.X, f.Y, f.Z, (unsigned long long)lock, (unsigned long long)aa));
        ++g_npcAimSent;
    }
    g_npcAimCursor = (g_npcAimCursor + kNpcAimPerTick) % firing.size();
}

// Client: report where our own weapons are pointing.
void ClientAimTick(float dt) {
    if (!g_aimSync || coop::CurrentRole() != coop::Role::Client) return;
    g_aimAccum += dt;
    // Streaming 20 Hz constantly measurably starved actor replication: mean NPC position error between
    // host and client was 1016 uu with the aim streams on versus 660 uu with them off. Aim only has to
    // be exact while actually shooting.
    if (g_aimAccum < 1.0 / (g_firingNow ? g_aimHz : g_aimIdleHz)) return;
    g_aimAccum = 0;
    APlayerController* pc = LocalPC();
    AActor* pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
    if (!pawn || !IsValidObject((UObject*)pawn)) return;
    FVector focus{};
    bool got = false;
    ForEachWeaponComponent(pawn, [&](UObject* wc) {
        if (got) return;
        const FVector& f = UE_FIELD(FVector, wc, es2off::UWeaponComponent::FocusLocation);
        if (IsFinite3(f)) { focus = f; got = true; }
    });
    if (!got) return;
    // Carry the locked target per weapon category as well: missiles home on it, and on the host our
    // weapon component has no player to have locked anything.
    uint64_t lock[2] = {0, 0}, aa[2] = {0, 0};
    ForEachWeaponComponent(pawn, [&](UObject* wc) {
        int cat = (int)UE_FIELD(uint8_t, wc, es2off::UWeaponComponent::WeaponCategory);
        if (cat < 0 || cat > 1) return;
        if (AActor* t = GetLock(wc)) lock[cat] = NetGuidOf((const UObject*)t);
        AActor* a = UE_FIELD(AActor*, wc, es2off::UWeaponComponent::CurrentAutoAimTarget);
        if (a && IsValidObject((UObject*)a)) aa[cat] = NetGuidOf((const UObject*)a);
    });
    coop::SendToServer(Format("AIM|%.1f|%.1f|%.1f|%llu|%llu|%llu|%llu", focus.X, focus.Y, focus.Z,
                              (unsigned long long)lock[0], (unsigned long long)lock[1],
                              (unsigned long long)aa[0], (unsigned long long)aa[1]));
    ++g_aimSent;
}

// Host: stamp the reporting player's aim onto their server-side weapons before ES2 reads it.
static void H_WeaponTick(UObject* comp, float dt, int tickType, void* tickFn) {
    // Client: a mirrored NPC's aim, reported by the host. Its own AI is not running here, so without
    // this its weapons point wherever the client's idle copy happens to face.
    if (g_npcAimSync && comp && !g_npcAim.empty() && coop::CurrentRole() == coop::Role::Client) {
        auto it = g_npcAim.find(comp);
        if (it != g_npcAim.end()) {
            UE_FIELD(FVector, comp, es2off::UWeaponComponent::FocusLocation) = it->second;
            UE_FIELD(FVector, comp, es2off::UWeaponComponent::ClampedNonAutoAimedFocusLocation) = it->second;
            ++g_npcAimApplied;
            auto aa = g_npcAutoAim.find(comp);
            if (aa != g_npcAutoAim.end()) ApplyAutoAim(comp, aa->second);
            auto lk = g_npcLock.find(comp);
            if (lk != g_npcLock.end() && g_lockSync) ApplyLock(comp, lk->second);
        }
    }
    if (g_aimSync && !g_aim.empty() && comp && coop::CurrentRole() == coop::Role::Host) {
        AActor* owner = UE_FIELD(AActor*, comp, es2off::UActorComponent::OwnerPrivate);
        if (owner) {
            if (players::Player* p = players::ByPawn(owner)) {
                auto it = g_aim.find(p->id);
                if (it != g_aim.end() && !p->local) {
                    int cat0 = (int)UE_FIELD(uint8_t, comp, es2off::UWeaponComponent::WeaponCategory);
                    // Aim at the TARGET, not at the world point the client reported.
                    //
                    // A client's copy of a moving NPC trails the host's by roughly the network latency —
                    // measured 660-1000 uu at ~10000 uu/s, which is many ship-lengths. Firing along a
                    // ray to where the CLIENT saw the ship therefore misses on the host every time, and
                    // that is exactly why a client could kill a stationary turret but never a moving one.
                    // When we know which actor the player is on, aim at where the HOST has it instead;
                    // the reported point is only the fallback for aiming at empty space.
                    FVector focus = it->second;
                    if (g_aimAtTarget && cat0 >= 0 && cat0 <= 1) {
                        uint64_t tg = 0;
                        auto aa2 = g_playerAutoAim.find(p->id);
                        if (aa2 != g_playerAutoAim.end()) tg = aa2->second[cat0];
                        if (!tg) { auto lk2 = g_playerLock.find(p->id); if (lk2 != g_playerLock.end()) tg = lk2->second[cat0]; }
                        if (tg) {
                            if (AActor* t = ActorFromNetGuid(tg)) {
                                focus = GetActorTransform(t).Translation;
                                ++g_aimRetargeted;
                            }
                        }
                    }
                    UE_FIELD(FVector, comp, es2off::UWeaponComponent::FocusLocation) = focus;
                    UE_FIELD(FVector, comp, es2off::UWeaponComponent::ClampedNonAutoAimedFocusLocation) = focus;
                    ++g_aimApplied;
                    int cat = (int)UE_FIELD(uint8_t, comp, es2off::UWeaponComponent::WeaponCategory);
                    auto aa = g_playerAutoAim.find(p->id);
                    if (aa != g_playerAutoAim.end() && cat >= 0 && cat <= 1) ApplyAutoAim(comp, aa->second[cat]);
                    auto lk = g_playerLock.find(p->id);
                    if (lk != g_playerLock.end() && g_lockSync && cat >= 0 && cat <= 1) ApplyLock(comp, lk->second[cat]);
                }
            }
        }
    }
    o_WeaponTick(comp, dt, tickType, tickFn);
}

// ---------------------------------------------------------------- NPC motion (client)
//
// NPC ships replicate with bRepPhysics, so the client integrates their physics locally between updates
// and lurches whenever a correction lands. Measured per-frame on one machine (`jitter`): the host moves a
// scout with a worst frame 1.26x its median step; the client's worst is 3.1x.
//
// Trying to intercept the correction failed — UE5 applies physics replication through its own physics
// path, so neither AActor::PostNetReceiveLocationAndRotation nor PostNetReceivePhysicState ever fires —
// and absorbing the jump after the fact made things worse, because the body keeps simulating underneath.
//
// So stop reacting to physics and drive the actor instead. AActor::ReplicatedMovement is the authoritative
// state the client already receives; follow it with the exact interpolate-and-extrapolate the mod uses for
// remote player pawns, overriding the transform every frame so the local simulation cannot diverge.
static bool g_npcFollow = true;
static float g_npcFollowRate = 12.f;      // how fast we converge on the target
static double g_npcMaxExtrap = 0.25;      // cap dead-reckoning; a stale target must not fly away
static double g_npcSnapDist = 15000.0;    // past this, jump rather than slide
static uint64_t g_npcFollowed = 0, g_npcFollowSnaps = 0;
static double g_npcNow = 0;

struct NpcTarget {
    FVector loc{}, vel{};
    double at = 0;
    bool has = false;
};
static std::map<AActor*, NpcTarget> g_npcTargets;
static std::vector<AActor*> g_npcCache;
static double g_npcCacheAge = 0;

void NpcFollowTick(float dt) {
    if (coop::CurrentRole() != coop::Role::Client || dt <= 0) return;
    g_npcNow += dt;

    // GetAllActorsOfClass walks every actor in the world, so refresh the list occasionally rather than
    // every frame; entries are revalidated on use.
    g_npcCacheAge -= dt;
    if (g_npcCacheAge <= 0) {
        g_npcCacheAge = 0.5;
        UClass* pawnClass = FindClass("ESPawn");
        g_npcCache.clear();
        if (pawnClass)
            for (AActor* a : GetAllActorsOfClass(GetWorld(), pawnClass))
                if (a && !players::ByPawn(a)) g_npcCache.push_back(a);
    }
    if (!g_npcFollow) return;

    double a = dt * g_npcFollowRate;
    if (a > 1) a = 1;

    for (AActor* act : g_npcCache) {
        if (!act || !IsValidObject((UObject*)act)) continue;
        if (GetRole(act) != 1 /*ROLE_SimulatedProxy*/) continue;      // only proxies the server owns

        void* rm = reinterpret_cast<char*>(act) + es2off::AActor::ReplicatedMovement;
        const FVector& rl = UE_FIELD(FVector, rm, es2off::FRepMovement::Location);
        const FVector& rv = UE_FIELD(FVector, rm, es2off::FRepMovement::LinearVelocity);
        if (rl.X == 0 && rl.Y == 0 && rl.Z == 0) continue;            // nothing replicated yet

        NpcTarget& t = g_npcTargets[act];
        if (!t.has || rl.X != t.loc.X || rl.Y != t.loc.Y || rl.Z != t.loc.Z) {
            t.loc = rl; t.vel = rv; t.at = g_npcNow; t.has = true;    // a fresh update landed
        }

        double age = g_npcNow - t.at;
        if (age > g_npcMaxExtrap) age = g_npcMaxExtrap;
        FVector predicted{ t.loc.X + t.vel.X * age, t.loc.Y + t.vel.Y * age, t.loc.Z + t.vel.Z * age };

        FTransform cur = GetActorTransform(act);
        double dx = predicted.X - cur.Translation.X, dy = predicted.Y - cur.Translation.Y, dz = predicted.Z - cur.Translation.Z;
        double err = sqrt(dx * dx + dy * dy + dz * dz);
        FTransform next = cur;
        if (err > g_npcSnapDist) { next.Translation = predicted; ++g_npcFollowSnaps; }
        else next.Translation = FVector{ cur.Translation.X + dx * a, cur.Translation.Y + dy * a, cur.Translation.Z + dz * a };
        SetActorTransform(act, next, false, 1 /*TeleportPhysics*/);
        ++g_npcFollowed;
    }
    if (g_npcTargets.size() > 512) g_npcTargets.clear();
}

// ---------------------------------------------------------------- messages
bool OnServerOp(APlayerController* from, const std::string& op, const std::string& body) {
    if (op == "AIM") {
        double x = 0, y = 0, z = 0; unsigned long long l0 = 0, l1 = 0, a0 = 0, a1 = 0;
        int n = sscanf(body.c_str(), "%lf|%lf|%lf|%llu|%llu|%llu|%llu", &x, &y, &z, &l0, &l1, &a0, &a1);
        if (n < 3) return true;
        players::Player* p = players::ByController(from);
        if (!p) return true;
        g_aim[p->id] = FVector{x, y, z};
        if (n >= 7) g_playerAutoAim[p->id] = {a0, a1};
        if (n >= 5) g_playerLock[p->id] = {l0, l1};   // stamped per tick, see ApplyLock
        return true;
    }
    if (op == "F") {
        int which = 0, down = 0;
        if (sscanf(body.c_str(), "%d|%d", &which, &down) != 2) return true;
        ApplyFireOnHost(from, which, down != 0);
        return true;
    }
    return false;
}

bool OnClientOp(const std::string& op, const std::string& body) {
    if (op == "WF") return ApplyNpcFire(body);
    if (op == "WA") return ApplyNpcAim(body);
    if (op == "HP") {
        // HP|<playerId>|<hull>|<shield>|<armor>
        // This used to skip the local player, which meant a client never saw its OWN bars move:
        // ES2 replicates no hitpoint state and the client no-ops its own damage, so this message is
        // the only source of truth a client has for its condition. A missing armour field (-1) or an
        // absent component is simply skipped.
        int id = 0; float hp = -1, sh = -1, ar = -1;
        int n = sscanf(body.c_str(), "%d|%f|%f|%f", &id, &hp, &sh, &ar);
        if (n < 3) return true;
        players::Player* p = players::ById(id);
        AActor* pawn = p ? p->pawn : nullptr;
        // The partner's slot is empty on a client (only the local slot is registered there), so fall
        // back to the local controller's pawn when the message is about us.
        if (!pawn && id == players::LocalId()) {
            APlayerController* pc = LocalPC();
            pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
        }
        if (!pawn || !IsValidObject((UObject*)pawn)) return true;
        ApplyRatio(pawn, "HealthComponent", hp);
        ApplyRatio(pawn, "ShieldComponent", sh);
        ApplyRatio(pawn, "ArmorComponent", ar);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- tick
void Tick(float dt, bool isHost) {
    if (!isHost) return;
    HostNpcAimTick(dt);
    g_healthAccum += dt;
    if (g_healthAccum < 1.0 / g_healthHz) return;
    g_healthAccum = 0;
    for (auto* p : players::All()) {
        if (!p->pawn) continue;
        float hp = GetHealthRatio(p->pawn), sh = GetShieldRatio(p->pawn), ar = GetArmorRatio(p->pawn);
        if (hp < 0) continue;
        coop::SendToAllClients(Format("HP|%d|%.3f|%.3f|%.3f", p->id, hp, sh, ar));
    }
}

// ---------------------------------------------------------------- commands
static void CmdFire(const console::Args& a, std::string& out) {
    // fire [primary|secondary] [on|off]  — presses the trigger on the LOCAL controller (goes through the
    // same hooks a real key press would, so on a client it routes to the host).
    int which = (a.size() > 1 && a[1].rfind("sec", 0) == 0) ? 1 : 0;
    bool down = !(a.size() > 2 && (a[2] == "off" || a[2] == "0"));
    APlayerController* pc = LocalPC();
    if (!pc) { out = "no local player controller\n"; return; }
    Fn_PCVoid fn = which == 0 ? (down ? o_StartFirePrimary : o_StopFirePrimary)
                              : (down ? o_StartFireSecondary : o_StopFireSecondary);
    // deliberately call the DETOUR path so a client's press is routed like a real one
    if (which == 0) { if (down) H_StartFirePrimary(pc); else H_StopFirePrimary(pc); }
    else            { if (down) H_StartFireSecondary(pc); else H_StopFireSecondary(pc); }
    out += Format("%s %s (routed=%d)\n", which ? "secondary" : "primary", down ? "DOWN" : "UP", (int)(IsClient() && g_routeFire));
}

// input <what> — call ES2's own native input handlers on the local controller. They are not
// UFunctions, so the console's `call` cannot reach them; this is the only faithful way to exercise a
// weapon swap or a travel/cruise charge from a test without a keyboard.
static void CmdInput(const console::Args& a, std::string& out) {
    APlayerController* pc = LocalPC();
    if (!pc) { out = "no local player controller\n"; return; }
    if (a.size() < 2) { out = "usage: input <nextprimary|prevprimary|nextsecondary|travel on/off|cruise on/off>\n"; return; }
    const std::string& what = a[1];
    bool down = !(a.size() > 2 && (a[2] == "off" || a[2] == "0"));
    uint32_t rva = 0;
    if (what == "nextprimary")        rva = es2rva::AESPlayerController_InputNextPrimaryWeapon;
    else if (what == "prevprimary")   rva = es2rva::AESPlayerController_InputPreviousPrimaryWeapon;
    else if (what == "nextsecondary") rva = es2rva::AESPlayerController_InputNextSecondaryWeapon;
    else if (what == "travel")        rva = down ? es2rva::AESPlayerController_InputChargeTravelMode
                                                 : es2rva::AESPlayerController_InputChargeTravelModeReleased;
    else if (what == "cruise")        rva = down ? es2rva::AESPlayerController_InputChargeCruiseMode
                                                 : es2rva::AESPlayerController_InputChargeCruiseModeReleased;
    else { out = "unknown input: " + what + "\n"; return; }
    Rva<std::remove_pointer_t<Fn_PCVoid>>(rva)(pc);
    out += Format("input %s%s -> %s\n", what.c_str(),
                  (what == "travel" || what == "cruise") ? (down ? " down" : " up") : "",
                  GetName((UObject*)pc).c_str());
}

// aim [playerId] — what each player's weapons are actually pointing at on THIS machine.
// UWeaponComponent::FocusLocation is the world point AWeaponBase::GetAimDirection reads, so comparing
// it between host and client is the direct measure of whether a client's shots go where it aims.
// guid <n> — resolve a NetGUID back to an actor on THIS machine. The reverse lookup is the half of
// FNetGUIDCache a server does not normally need, so this checks whether it works host-side at all.
// npcpos — every replicated NPC pawn with its NetGUID and position, so the same ship can be compared
// between host and client (their UObject names differ, the guid does not).
// jitter <guid> [seconds] — measure what the player actually sees, entirely on ONE machine.
//
// Comparing a host reading against a client reading is worthless here: the two console round-trips are
// tens of milliseconds apart and at ~10000 uu/s that skew alone is worth hundreds of units, which is the
// same order as the thing being measured. Instead sample one NPC every frame locally and look at the
// frame-to-frame step. Smooth motion gives a steady step; a replication snap shows up as a step many
// times the median. That is the jitter, and it needs no second machine.
static AActor* g_jitterTarget = nullptr;
static FVector g_jitterLast{};
static bool g_jitterHasLast = false;
static double g_jitterLeft = 0;
static std::vector<double> g_jitterSteps;
// Deviation of the displayed position from the authoritative ReplicatedMovement, read in the SAME tick.
// This is the fidelity question ("is the ship where the server says?") and, unlike anything cross-machine,
// it carries no sampling skew at all.
static std::vector<double> g_jitterDev;

void JitterTick(float dt) {
    if (g_jitterLeft <= 0 || !g_jitterTarget) return;
    if (!IsValidObject((UObject*)g_jitterTarget)) { g_jitterLeft = 0; return; }
    g_jitterLeft -= dt;
    FVector p = GetActorTransform(g_jitterTarget).Translation;
    {
        void* rm = reinterpret_cast<char*>(g_jitterTarget) + es2off::AActor::ReplicatedMovement;
        const FVector& rl = UE_FIELD(FVector, rm, es2off::FRepMovement::Location);
        if (!(rl.X == 0 && rl.Y == 0 && rl.Z == 0) && g_jitterDev.size() < 4000)
            g_jitterDev.push_back(sqrt((p.X - rl.X) * (p.X - rl.X) + (p.Y - rl.Y) * (p.Y - rl.Y) + (p.Z - rl.Z) * (p.Z - rl.Z)));
    }
    if (g_jitterHasLast) {
        double d = sqrt((p.X - g_jitterLast.X) * (p.X - g_jitterLast.X) +
                        (p.Y - g_jitterLast.Y) * (p.Y - g_jitterLast.Y) +
                        (p.Z - g_jitterLast.Z) * (p.Z - g_jitterLast.Z));
        if (g_jitterSteps.size() < 4000) g_jitterSteps.push_back(d);
    }
    g_jitterLast = p;
    g_jitterHasLast = true;
}

static void CmdJitter(const console::Args& a, std::string& out) {
    if (a.size() > 1) {
        uint64_t g = strtoull(a[1].c_str(), nullptr, 10);
        AActor* t = ActorFromNetGuid(g);
        if (!t) { out = Format("guid %llu not found here\n", (unsigned long long)g); return; }
        g_jitterTarget = t;
        g_jitterSteps.clear();
        g_jitterDev.clear();
        g_jitterHasLast = false;
        g_jitterLeft = a.size() > 2 ? atof(a[2].c_str()) : 5.0;
        out += Format("sampling %s for %.1fs\n", GetName((UObject*)t).c_str(), g_jitterLeft);
        return;
    }
    if (g_jitterSteps.empty()) { out = "no samples — run: jitter <guid> [seconds]\n"; return; }
    std::vector<double> v = g_jitterSteps;
    std::sort(v.begin(), v.end());
    double med = v[v.size() / 2];
    double mx = v.back();
    double sum = 0; for (double d : v) sum += d;
    int snaps = 0; for (double d : v) if (med > 0.01 && d > med * 4) ++snaps;
    // Step size alone is a poor smoothness measure: an actor that lags behind shows SMALLER steps
    // without looking any smoother. What the eye reads as jitter is the step changing abruptly, so also
    // report the spread of steps and the worst frame-to-frame change (jerk), both relative to the mean.
    double mean = sum / v.size();
    double var = 0; for (double d : v) var += (d - mean) * (d - mean);
    double sd = sqrt(var / v.size());
    double jerk = 0;
    for (size_t i = 1; i < g_jitterSteps.size(); ++i) {
        double j = fabs(g_jitterSteps[i] - g_jitterSteps[i - 1]);
        if (j > jerk) jerk = j;
    }
    out += Format("samples=%d  median=%.0f mean=%.0f max=%.0f u | spread sd/mean=%.2f  worst jerk=%.0f u (%.2f x mean)\n",
                  (int)v.size(), med, mean, mx, mean > 0 ? sd / mean : 0.0, jerk, mean > 0 ? jerk / mean : 0.0);
    if (!g_jitterDev.empty()) {
        std::vector<double> d = g_jitterDev;
        std::sort(d.begin(), d.end());
        double ds = 0; for (double x : d) ds += x;
        out += Format("  deviation from replicated state: mean=%.0f u  median=%.0f u  max=%.0f u\n",
                      ds / d.size(), d[d.size() / 2], d.back());
    }
    out += Format("  still sampling: %s\n", g_jitterLeft > 0 ? "yes" : "no");
}

static void CmdNpcPos(const console::Args& a, std::string& out) {
    UClass* pawnClass = FindClass("ESPawn");
    if (!pawnClass) { out = "no ESPawn class\n"; return; }
    int shown = 0, max = a.size() > 1 ? atoi(a[1].c_str()) : 10;
    for (AActor* act : GetAllActorsOfClass(GetWorld(), pawnClass)) {
        if (shown >= max || !act || !IsValidObject((UObject*)act)) continue;
        if (players::ByPawn(act)) continue;
        uint64_t g = NetGuidOf((const UObject*)act);
        if (!g) continue;
        FTransform t = GetActorTransform(act);
        out += Format("  guid=%-6llu %-28s pos=(%.0f, %.0f, %.0f) role=%d\n", (unsigned long long)g,
                      GetName((UObject*)act).c_str(), t.Translation.X, t.Translation.Y, t.Translation.Z,
                      (int)GetRole(act));
        ++shown;
    }
    if (!shown) out += "  (no replicated NPC pawns)\n";
}

static void CmdGuid(const console::Args& a, std::string& out) {
    if (a.size() < 2) { out = "usage: guid <netguid>\n"; return; }
    uint64_t g = strtoull(a[1].c_str(), nullptr, 10);
    AActor* r = ActorFromNetGuid(g);
    out += Format("guid %llu -> %s\n", (unsigned long long)g,
                  r ? GetFullName((UObject*)r).c_str() : "NOT RESOLVED on this machine");
    if (r) out += Format("  reverse check: NetGuidOf(that actor) = %llu\n",
                         (unsigned long long)NetGuidOf((const UObject*)r));
}

// locktest <playerId> <targetGuid> — set a lock on that player's server-side weapons and read it back
// IN THE SAME CALL, which separates "SetLockedTarget refused it" from "something cleared it later".
static void CmdLockTest(const console::Args& a, std::string& out) {
    if (a.size() < 3) { out = "usage: locktest <playerId> <targetGuid>\n"; return; }
    int id = atoi(a[1].c_str());
    uint64_t g = strtoull(a[2].c_str(), nullptr, 10);
    players::Player* p = players::ById(id);
    if (!p || !p->pawn || !IsValidObject((UObject*)p->pawn)) { out = "no such player / pawn\n"; return; }
    AActor* want = ActorFromNetGuid(g);
    out += Format("target guid %llu -> %s\n", (unsigned long long)g, want ? GetName((UObject*)want).c_str() : "UNRESOLVED");
    if (!want) return;
    ForEachWeaponComponent(p->pawn, [&](UObject* wc) {
        int cat = (int)UE_FIELD(uint8_t, wc, es2off::UWeaponComponent::WeaponCategory);
        AActor* owner = UE_FIELD(AActor*, wc, es2off::UActorComponent::OwnerPrivate);
        out += Format("  cat=%d owner=%s selfLockGuard=%s\n", cat, GetName((UObject*)owner).c_str(),
                      owner == want ? "WOULD TRIP (target is the owner)" : "ok");
        AActor* before = GetLock(wc);
        SetLock(wc, want);
        AActor* after = GetLock(wc);
        int32_t idx = UE_FIELD(int32_t, wc, es2off::UWeaponComponent::LockedTarget);
        int32_t ser = UE_FIELD(int32_t, wc, es2off::UWeaponComponent::LockedTarget + 4);
        out += Format("    before=%s  after=%s   weakptr{index=%d serial=%d}\n",
                      before ? GetName((UObject*)before).c_str() : "none",
                      after ? GetName((UObject*)after).c_str() : "none", idx, ser);
    });
}

static void CmdAimInfo(const console::Args& a, std::string& out) {
    for (auto* p : players::All()) {
        if (!p->pawn || !IsValidObject((UObject*)p->pawn)) continue;
        UObject* wc = FindComponentOfClass(p->pawn, "WeaponComponent");
        if (!wc) { out += Format("  p%d %-24s (no weapon component)\n", p->id, GetName((UObject*)p->pawn).c_str()); continue; }
        FVector& focus = UE_FIELD(FVector, wc, es2off::UWeaponComponent::FocusLocation);
        UObject* aim = UE_FIELD(UObject*, wc, es2off::UWeaponComponent::CurrentAutoAimTarget);
        FTransform t = GetActorTransform(p->pawn);
        out += Format("  p%d %-22s pos=(%.0f, %.0f, %.0f)\n", p->id, GetName((UObject*)p->pawn).c_str(),
                      t.Translation.X, t.Translation.Y, t.Translation.Z);
        std::string locks;
        ForEachWeaponComponent(p->pawn, [&](UObject* w) {
            int cat = (int)UE_FIELD(uint8_t, w, es2off::UWeaponComponent::WeaponCategory);
            AActor* t = GetLock(w);
            locks += Format(" %s=%s(guid %llu)", cat == 0 ? "primary" : "secondary",
                            t ? GetName((UObject*)t).c_str() : "none",
                            (unsigned long long)(t ? NetGuidOf((const UObject*)t) : 0));
        });
        out += Format("     focus=(%.0f, %.0f, %.0f) autoAimTarget=%s\n", focus.X, focus.Y, focus.Z,
                      (aim && IsValidObject(aim)) ? Format("%s(guid %llu)", GetName(aim).c_str(),
                          (unsigned long long)NetGuidOf(aim)).c_str() : "none");
        out += Format("     lock:%s\n", locks.c_str());
    }
    if (out.empty()) out = "no players\n";

    // `aiminfo npc` — the same question for mirrored NPCs. Keyed by NetGUID, because the same enemy is a
    // different UObject on each machine and the GUID is the only identity both sides agree on.
    if (a.size() > 1 && a[1] == "npc") {
        void* cache = LocalGuidCache();
        if (!cache) { out += "no guid cache (not in a session?)\n"; return; }
        UClass* wcClass = FindClass("WeaponComponent");
        int shown = 0;
        ForEachObject([&](UObject* o) {
            if (shown >= 40 || !wcClass || !IsA(o, wcClass)) return true;
            AActor* owner = UE_FIELD(AActor*, o, es2off::UActorComponent::OwnerPrivate);
            if (!owner || !IsValidObject((UObject*)owner) || players::ByPawn(owner)) return true;
            const FVector& f = UE_FIELD(FVector, o, es2off::UWeaponComponent::FocusLocation);
            if (!IsFinite3(f)) return true;
            uint64_t guid = 0;
            Rva<std::remove_pointer_t<Fn_GetNetGUID>>(es2rva::FNetGUIDCache_GetNetGUID)(cache, &guid, (const UObject*)owner);
            if (!guid) return true;
            // '*' marks a component whose aim we are receiving from the host, i.e. an actually
            // mirrored shooter — those are the entries worth comparing between the two machines.
            FTransform ot = GetActorTransform(owner);
            out += Format("  %sguid=%-6llu %-26s pos=(%.0f, %.0f, %.0f) focus=(%.0f, %.0f, %.0f)\n",
                          g_npcAim.count(o) ? "*" : " ", (unsigned long long)guid,
                          GetName((UObject*)owner).c_str(),
                          ot.Translation.X, ot.Translation.Y, ot.Translation.Z, f.X, f.Y, f.Z);
            ++shown;
            return true;
        });
        if (!shown) out += "  (no mirrored NPC weapons with a valid aim here)\n";
    }
}

static void CmdCombat(const console::Args& a, std::string& out) {
    if (a.size() > 2 && a[1] == "route") g_routeFire = a[2] == "1";
    if (a.size() > 2 && a[1] == "localfire") g_localFire = a[2] == "1";
    if (a.size() > 2 && a[1] == "npcfollow") { g_npcFollow = a[2] == "1"; out += Format("npc follow %d\n", (int)g_npcFollow); return; }
    if (a.size() > 2 && a[1] == "followrate") { g_npcFollowRate = (float)atof(a[2].c_str()); out += Format("rate %.1f\n", g_npcFollowRate); return; }
    if (a.size() > 2 && a[1] == "clientregen") { g_blockClientRegen = a[2] == "0"; out += Format("client regen blocked %d\n", (int)g_blockClientRegen); return; }
    if (a.size() > 2 && a[1] == "aimattarget") { g_aimAtTarget = a[2] == "1"; out += Format("aim-at-target %d\n", (int)g_aimAtTarget); return; }
    if (a.size() > 2 && a[1] == "autoaim") { g_autoAimSync = a[2] == "1"; out += Format("autoaim sync %d\n", (int)g_autoAimSync); return; }
    if (a.size() > 2 && a[1] == "locksync") { g_lockSync = a[2] == "1"; out += Format("lock sync %d\n", (int)g_lockSync); return; }
    if (a.size() > 2 && a[1] == "npcaim") { g_npcAimSync = a[2] == "1"; out += Format("npc aim sync %d\n", (int)g_npcAimSync); return; }
    if (a.size() > 2 && a[1] == "aimsync") { g_aimSync = a[2] == "1"; out += Format("aim sync %d\n", (int)g_aimSync); return; }
    if (a.size() > 2 && a[1] == "hphz") g_healthHz = (float)atof(a[2].c_str());
    if (a.size() > 2 && a[1] == "npcfire") g_mirrorNpcFire = a[2] == "1";
    out += Format("npcFireMirror=%d sent=%llu applied=%llu unresolved=%llu deduped=%llu\n", (int)g_mirrorNpcFire,
                  (unsigned long long)g_npcFireSent, (unsigned long long)g_npcFireApplied,
                  (unsigned long long)g_npcFireUnresolved, (unsigned long long)g_npcFireDeduped);
    out += Format("autoAimSync=%d autoAimApplied=%llu\n", (int)g_autoAimSync, (unsigned long long)g_autoAimApplied);
    out += Format("lockSync=%d lockApplied=%llu guidScans=%llu\n", (int)g_lockSync,
                  (unsigned long long)g_lockApplied, (unsigned long long)g_guidScans);
    out += Format("npcAimSync=%d npcAimSent=%llu npcAimApplied=%llu npcAimTracked=%d\n",
                  (int)g_npcAimSync, (unsigned long long)g_npcAimSent, (unsigned long long)g_npcAimApplied, (int)g_npcAim.size());
    out += Format("npcFollow=%d rate=%.1f followed=%llu snaps=%llu tracked=%d\n", (int)g_npcFollow,
                  g_npcFollowRate, (unsigned long long)g_npcFollowed, (unsigned long long)g_npcFollowSnaps,
                  (int)g_npcCache.size());
    out += Format("blockClientRegen=%d regenBlocked=%llu\n", (int)g_blockClientRegen, (unsigned long long)g_regenBlocked);
    out += Format("aimAtTarget=%d retargeted=%llu\n", (int)g_aimAtTarget, (unsigned long long)g_aimRetargeted);
    out += Format("aimSync=%d aimSent=%llu aimApplied=%llu aimKnown=%d\n",
                  (int)g_aimSync, (unsigned long long)g_aimSent, (unsigned long long)g_aimApplied, (int)g_aim.size());
    out += Format("routeFire=%d localFire=%d healthHz=%.0f fireSent=%llu fireApplied=%llu\n",
                  (int)g_routeFire, (int)g_localFire, g_healthHz,
                  (unsigned long long)g_fireSent, (unsigned long long)g_fireApplied);
    for (auto* p : players::All()) {
        if (!p->pawn) continue;
        out += Format("  p%d %-22s hp=%.3f shield=%.3f\n", p->id, GetName((UObject*)p->pawn).c_str(),
                      GetHealthRatio(p->pawn), GetShieldRatio(p->pawn));
    }
}

// god [0|1] - make the local ship's hull and shield undepletable. Test aid: an idle host parked next to
// enemies otherwise dies mid-experiment and invalidates the run.
static void CmdGod(const console::Args& a, std::string& out) {
    bool on = !(a.size() > 1 && (a[1] == "0" || a[1] == "off"));
    APlayerController* pc = LocalPC();
    AActor* pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
    if (!pawn) { out = "no local pawn\n"; return; }
    int n = 0;
    for (const char* cls : {"HealthComponent", "ShieldComponent", "ArmorComponent"}) {
        UObject* c = FindComponentOfClass(pawn, cls);
        if (!c) continue;
        UE_FIELD(bool, c, es2off::UHealthComponent::CannotDeplete) = on;
        ++n;
    }
    out += Format("god=%d applied to %d component(s) of %s\n", (int)on, n, GetName((UObject*)pawn).c_str());
}

// lock [playerId] - make a ship acquire its closest target so routed fire actually has something to aim at.
// On the host, `lock <id>` locks on THAT player's server-side pawn (the one whose weapons really fire).
using Fn_PawnVoid = void (*)(AActor*);
static void CmdLock(const console::Args& a, std::string& out) {
    AActor* pawn = nullptr;
    if (a.size() > 1) {
        players::Player* p = players::ById(atoi(a[1].c_str()));
        if (p) pawn = p->pawn;
    } else {
        APlayerController* pc = LocalPC();
        pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
    }
    if (!pawn) { out = "no pawn\n"; return; }
    Rva<std::remove_pointer_t<Fn_PawnVoid>>(es2rva::AESPawn_LockClosestTarget)(pawn);
    using Fn_GetLocked = AActor* (*)(AActor*);
    AActor* t = Rva<std::remove_pointer_t<Fn_GetLocked>>(es2rva::AESPawn_GetLockedTarget)(pawn);
    out += Format("%s -> locked target: %s\n", GetName((UObject*)pawn).c_str(), GetFullName((UObject*)t).c_str());
}

// aim [playerId] - lock the closest target AND point the hull at it. ES2's auto-aim only covers a narrow
// cone, so a ship parked facing the wrong way never lands a shot; this makes automated combat tests possible.
static void CmdAim(const console::Args& a, std::string& out) {
    AActor* pawn = nullptr;
    if (a.size() > 1) { players::Player* p = players::ById(atoi(a[1].c_str())); if (p) pawn = p->pawn; }
    else { APlayerController* pc = LocalPC(); pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr; }
    if (!pawn) { out = "no pawn\n"; return; }
    Rva<std::remove_pointer_t<Fn_PawnVoid>>(es2rva::AESPawn_LockClosestTarget)(pawn);
    using Fn_GetLocked = AActor* (*)(AActor*);
    AActor* t = Rva<std::remove_pointer_t<Fn_GetLocked>>(es2rva::AESPawn_GetLockedTarget)(pawn);
    if (!t || !IsValidObject((UObject*)t)) { out = "no target locked\n"; return; }
    FTransform mine = GetActorTransform(pawn), theirs = GetActorTransform(t);
    double dx = theirs.Translation.X - mine.Translation.X;
    double dy = theirs.Translation.Y - mine.Translation.Y;
    double dz = theirs.Translation.Z - mine.Translation.Z;
    double len = sqrt(dx*dx + dy*dy + dz*dz);
    if (len < 1e-3) { out = "target is on top of us\n"; return; }
    dx /= len; dy /= len; dz /= len;
    // UE FRotator (radians here) -> FQuat, roll = 0
    double yaw = atan2(dy, dx);
    double pitch = atan2(dz, sqrt(dx*dx + dy*dy));
    double sp = sin(pitch * 0.5), cp = cos(pitch * 0.5);
    double sy = sin(yaw * 0.5),   cy = cos(yaw * 0.5);
    FTransform next = mine;
    next.Rotation = FQuat{  sp * sy, -sp * cy,  cp * sy,  cp * cy };
    SetActorTransform(pawn, next, false, 1);
    out += Format("%s aimed at %s (%.0f uu away)\n", GetName((UObject*)pawn).c_str(), GetName((UObject*)t).c_str(), len);
}

static void CmdHp(const console::Args& a, std::string& out) {
    // hp [ClassFilter] — health/shield of matching pawns in the world
    std::string cls = a.size() > 1 ? a[1] : "ESPawn";
    UClass* c = FindClass(cls);
    if (!c) { out = "class not found\n"; return; }
    for (AActor* act : GetAllActorsOfClass(GetWorld(), c)) {
        float hp = GetHealthRatio(act);
        if (hp < 0) continue;
        out += Format("  %-40s hp=%.3f shield=%.3f role=%d\n", GetName((UObject*)act).c_str(), hp, GetShieldRatio(act), GetRole(act));
    }
}

void Register() {
    console::Register("fire", "fire [primary|secondary] [on|off] - press the local fire trigger (routes to host on a client)", CmdFire);
    console::Register("input", "input <nextprimary|prevprimary|nextsecondary|travel on/off|cruise on/off> - press a real input handler (test aid)", CmdInput);
    console::Register("combat", "combat [route 0/1|localfire 0/1|hphz N] - fire-routing status and player health", CmdCombat);
    console::Register("locktest", "locktest <playerId> <targetGuid> - set+read a lock in one call", CmdLockTest);
    console::Register("jitter", "jitter <guid> [sec] - per-frame motion steps of one NPC on THIS machine", CmdJitter);
    console::Register("npcpos", "npcpos [max] - NPC pawn positions keyed by NetGUID", CmdNpcPos);
    console::Register("guid", "guid <n> - resolve a NetGUID to an actor on this machine", CmdGuid);
    console::RegisterTick("jitter", JitterTick);
    console::RegisterTick("npcfollow", NpcFollowTick);
    console::Register("aiminfo", "aiminfo [npc] - weapon FocusLocation per player, or per NPC keyed by NetGUID", CmdAimInfo);
    console::Register("hp", "hp [Class] - health/shield ratios of pawns in the world", CmdHp);
    console::Register("lock", "lock [playerId] - acquire closest target (host: on that player's server-side pawn)", CmdLock);
    console::Register("aim", "aim [playerId] - lock closest target and point the hull at it (test aid)", CmdAim);
    console::Register("god", "god [0|1] - make the local ship undepletable (test aid)", CmdGod);
}

void OnInit() {
    // installed but left OFF; coop enables it only while this process is a network client
    { void* orig = nullptr;
      hooks::Install("UGameplayLib::ApplyESPointDamage", es2rva::UGameplayLib_ApplyESPointDamage, (void*)&H_ApplyESPointDamage, &orig);
      hooks::Enable("UGameplayLib::ApplyESPointDamage", false); }
    { void* orig = nullptr;
      hooks::Install("UGameplayLib::ApplyESRadialDamage", es2rva::UGameplayLib_ApplyESRadialDamage, (void*)&H_ApplyESRadialDamage, &orig);
      hooks::Enable("UGameplayLib::ApplyESRadialDamage", false); }
    hooks::Install("UShieldComponent::TickRegeneration", es2rva::UShieldComponent_TickRegeneration, (void*)&H_TickRegeneration, (void**)&o_TickRegen);
    hooks::Install("UWeaponComponent::TickComponent", es2rva::UWeaponComponent_TickComponent, (void*)&H_WeaponTick, (void**)&o_WeaponTick);
    hooks::Install("UWeaponComponent::StartFire", es2rva::UWeaponComponent_StartFire, (void*)&H_StartFire, (void**)&o_StartFire);
    hooks::Install("UWeaponComponent::StopFire", es2rva::UWeaponComponent_StopFire, (void*)&H_StopFire, (void**)&o_StopFire);
    hooks::Install("AESPlayerController::InputStartFirePrimary", es2rva::AESPlayerController_InputStartFirePrimary, (void*)&H_StartFirePrimary, (void**)&o_StartFirePrimary);
    hooks::Install("AESPlayerController::InputStopFirePrimary", es2rva::AESPlayerController_InputStopFirePrimary, (void*)&H_StopFirePrimary, (void**)&o_StopFirePrimary);
    hooks::Install("AESPlayerController::InputStartFireSecondary", es2rva::AESPlayerController_InputStartFireSecondary, (void*)&H_StartFireSecondary, (void**)&o_StartFireSecondary);
    hooks::Install("AESPlayerController::InputStopFireSecondary", es2rva::AESPlayerController_InputStopFireSecondary, (void*)&H_StopFireSecondary, (void**)&o_StopFireSecondary);
}
}
