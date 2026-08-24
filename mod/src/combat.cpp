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
static uint64_t g_aimRetargeted = 0;
static std::map<int, uint64_t> g_playerTargetGuid;   // what the player is aiming at
static std::map<int, FVector> g_playerTargetSeen;   // and where the CLIENT sees it      // mirror the locked target as well as the aim point
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

// ---------------------------------------------------------------- unreplicated level actors
//
// ES2's plant enemies (BP_Cave_Anemone_*), proximity mines and similar props are level actors with
// bReplicates=false and bNetLoadOnClient=true: every machine loads its OWN copy out of the map and
// simulates it privately. Nothing about them is networked, and the NPC mirroring below cannot help --
// it is keyed on a FNetworkGUID, and an unreplicated actor has none (`if (!guid) return`).
//
// That leaves a client unable to damage them at all. Two separate failure modes, both reported as
// "the client can't damage the plants or mines":
//
//   * GHOSTS. Anything the host already destroyed -- cleared before the client joined, or wiped by the
//     save state at level load -- still exists on the client, forever. Measured in S01L01 with a
//     progressed save: host 0 anemones, client 16. Shooting one does nothing on either machine, because
//     the host has no such actor to shoot.
//   * NO DAMAGE FEEDBACK. Where both machines do have the actor, the client's fire reaches the host and
//     kills the host's copy, but the client's copy never hears about it and stays whole.
//
// Identity is the actor's path name: level actors are stably named, and the two machines load the same
// map, so `/Game/Maps/.../S01L01:PersistentLevel.BP_Cave_Anemone_patch2` means the same anemone on both
// (verified live). Only PersistentLevel actors are reconciled -- streamed sublevels can legitimately
// differ between machines, and "the host does not have it" would then be the wrong conclusion.
static uint64_t g_ghostAsked = 0, g_ghostKilled = 0, g_ghostReported = 0;
static uint64_t g_unrepHpSent = 0, g_unrepHpApplied = 0;
static bool g_reconcile = true;
static bool g_reconcileAnswered = false;
static int g_reconcileTries = 0;
static double g_reconcileWait = 0;

// bNetStartup is UE's own "placed in the level, not spawned at runtime" flag, and it is one byte -- worth
// having because UWorld::DestroyActor below runs for every projectile and effect in a firefight, and
// building a path name for each of those would be absurd.
static inline bool IsLevelPlaced(AActor* a) {
    return (UE_FIELD(uint8_t, a, es2off::AActor::bNetStartup_off) & es2off::AActor::bNetStartup_mask) != 0;
}

static bool IsUnreplicatedLevelActor(AActor* a) {
    if (!a || !IsValidObject((UObject*)a)) return false;
    if (!IsLevelPlaced(a) || GetReplicates(a)) return false;
    return GetPathName((UObject*)a).find(":PersistentLevel.") != std::string::npos;
}

// The classes that suffer from this. ESPawn covers the plant enemies (cave anemones); mines and loot
// containers derive straight from AActor, so scanning pawns alone missed them entirely -- mines stayed
// indestructible from a client, and a client kept every container the host had already looted.
static const char* const kUnreplicatedClasses[] = {"ESPawn", "ProximityMineBase", "ItemContainer"};

static std::vector<AActor*> UnreplicatedLevelActors() {
    std::vector<AActor*> out;
    for (const char* n : kUnreplicatedClasses) {
        UClass* c = FindClass(n);
        if (!c) continue;
        for (AActor* a : GetAllActorsOfClass(GetWorld(), c))
            if (IsUnreplicatedLevelActor(a) && !players::ByPawn(a)) out.push_back(a);
    }
    return out;
}

// Client: ask the host which of our unreplicated level NPCs it does not have.
static void SendGhostQuery() {
    std::string batch;
    int inBatch = 0;
    for (AActor* a : UnreplicatedLevelActors()) {
        batch += "|" + GetPathName((UObject*)a);
        ++inBatch; ++g_ghostAsked;
        if (inBatch >= 4) { coop::SendToServer("NRQ" + batch); batch.clear(); inBatch = 0; }
    }
    if (inBatch) coop::SendToServer("NRQ" + batch);
    LOGF("[dmg] asked the host about %llu unreplicated level actor(s)", (unsigned long long)g_ghostAsked);
}

// Host: answer with the ones that are not here. Absent means the host destroyed it (or never spawned it
// from its save state), so the client is holding a ghost.
static bool AnswerGhostQuery(APlayerController* from, const std::string& body) {
    std::string dead;
    int seen = 0;
    LOGF("[dmg] ghost query: %zu bytes", body.size());
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t bar = body.find('|', pos);
        std::string path = body.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
        if (!path.empty()) {
            ++seen;
            UObject* o = FindObject(path);
            if (!o || IsGarbage(o)) { dead += "|" + path; ++g_ghostReported; }
        }
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    LOGF("[dmg] ghost query: %d path(s), %s", seen, dead.empty() ? "all alive here" : "some are gone");
    coop::SendToClient(from, "NRD" + dead);      // always answer, even with nothing: it stops the retries
    return true;
}

// Client: retire the ghosts through ES2's own destroy so its Blueprint teardown runs.
static bool ApplyGhostList(const std::string& body) {
    g_reconcileAnswered = true;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t bar = body.find('|', pos);
        std::string path = body.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
        if (!path.empty()) {
            if (UObject* o = FindObject(path)) {
                if (UFunction* destroy = FindFunction(o, "K2_DestroyActor")) {
                    ProcessEvent(o, destroy, nullptr);
                    ++g_ghostKilled;
                }
            }
        }
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    LOGF("[dmg] retired %llu ghost actor(s) the host does not have", (unsigned long long)g_ghostKilled);
    return true;
}

void ClientAimTick(float dt) {
    // Ask once the handshake is done, and keep asking until answered. An early send is simply dropped --
    // the same way the first HELLO is (see coop.cpp): the client has a local controller well before the
    // channel will actually carry anything, and the first attempt here vanished silently.
    if (g_reconcile && !g_reconcileAnswered && coop::CurrentRole() == coop::Role::Client
        && players::LocalId() > 0 && g_reconcileTries < 5) {
        g_reconcileWait += dt;
        if (g_reconcileWait >= 4.0) { g_reconcileWait = 0; ++g_reconcileTries; SendGhostQuery(); }
    }
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
    AActor* primary = nullptr;
    ForEachWeaponComponent(pawn, [&](UObject* wc) {
        int cat = (int)UE_FIELD(uint8_t, wc, es2off::UWeaponComponent::WeaponCategory);
        if (cat < 0 || cat > 1) return;
        AActor* lt = GetLock(wc);
        if (lt) lock[cat] = NetGuidOf((const UObject*)lt);
        AActor* a = UE_FIELD(AActor*, wc, es2off::UWeaponComponent::CurrentAutoAimTarget);
        if (a && IsValidObject((UObject*)a)) aa[cat] = NetGuidOf((const UObject*)a);
        if (!primary) primary = a ? a : lt;
    });
    // Send where WE see that target too. The host cannot correct for the position desync without it,
    // and correcting by snapping onto its own copy (which is what this used to do) turned every shot
    // into a guided one — you could fire well beside an enemy and still hit.
    uint64_t tguid = primary ? NetGuidOf((const UObject*)primary) : 0;
    FVector tpos{};
    if (primary) tpos = GetActorTransform(primary).Translation;
    coop::SendToServer(Format("AIM|%.1f|%.1f|%.1f|%llu|%llu|%llu|%llu|%llu|%.1f|%.1f|%.1f",
                              focus.X, focus.Y, focus.Z,
                              (unsigned long long)lock[0], (unsigned long long)lock[1],
                              (unsigned long long)aa[0], (unsigned long long)aa[1],
                              (unsigned long long)tguid, tpos.X, tpos.Y, tpos.Z));
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
                        auto tgIt = g_playerTargetGuid.find(p->id);
                        auto seenIt = g_playerTargetSeen.find(p->id);
                        if (tgIt != g_playerTargetGuid.end() && tgIt->second &&
                            seenIt != g_playerTargetSeen.end()) {
                            if (AActor* t = ActorFromNetGuid(tgIt->second)) {
                                // Shift the player's own aim by however far this target has drifted
                                // between the two machines. Aiming dead-on still hits; aiming beside it
                                // still misses by exactly as much as the player missed by.
                                FVector here = GetActorTransform(t).Translation;
                                const FVector& there = seenIt->second;
                                focus = FVector{ focus.X + (here.X - there.X),
                                                 focus.Y + (here.Y - there.Y),
                                                 focus.Z + (here.Z - there.Z) };
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
// Both OFF by default — kept as instrumented experiments, because measurement did not support them.
//
// The hypothesis was that correcting POSITION alone leaves the proxy's rigid body integrating with its
// own stale velocity (these really do simulate on the client: bSimulatePhysics=true, bRepPhysics=true),
// so body and corrector fight and that fight is the jitter — the remote-player path writes the body's
// velocity too (coop.cpp SmoothRemotePawns -> SetPhysVelocity). Measured on a moving scout, interleaved,
// two rounds each: deviation from the replicated state 513 u (position only) vs 580 u (position+velocity
// +rotation), step spread sd/mean 0.67 vs 0.68. No improvement, if anything slightly worse — and the
// reason is the ordering: NpcFollowTick runs from the UGameEngine::Tick detour BEFORE the world ticks,
// so physics then integrates a whole frame AFTER our write, and handing it the full replicated velocity
// makes it overshoot the target by ~v*dt (~600 u at the 20 fps this box manages with two instances).
//
// What DID work is making the proxy kinematic (`call 0x<rootcomp> SetSimulatePhysics 0`), i.e. removing
// the competing integration altogether: sd/mean 0.74 -> 0.30 and worst jerk 3.30x -> 1.87x mean on the
// same actor. See docs/NOTES.md; that is the direction to take, and rotation following becomes mandatory
// there because nothing else would orient the proxy.
static bool g_npcFollowVel = false;
static bool g_npcFollowRot = false;

// THE fix for NPC jitter: take the local physics simulation out of the loop entirely on the client.
//
// These proxies really do simulate here (bSimulatePhysics=true, bRepPhysics=true), so every frame the
// body integrates itself and our correction drags it back — and because NpcFollowTick runs from the
// UGameEngine::Tick detour BEFORE the world ticks, physics always gets the last word. Correcting harder
// cannot win that (measured: writing the authoritative velocity made it slightly WORSE). Making the body
// kinematic removes the competing integration, and then the transform we write IS what gets drawn.
// Measured on one moving scout, same actor, back to back: step spread sd/mean 0.74 -> 0.30 and worst
// frame-to-frame jerk 3.30x -> 1.87x mean. The cost is a little more lag, which is now purely the chase
// rate (`combat followrate N`) since nothing else moves the actor.
//
// Applies to every simulated proxy this tick drives, which on a client includes the OTHER player's ship
// (a client's registry holds only itself, so partner ships come through here too and have exactly the
// same problem). It never touches our own ship: that is ROLE_AutonomousProxy and is filtered out below,
// and it must keep simulating because the client is authoritative over its own movement.
static bool g_npcKinematic = true;
static bool g_npcLeadFrame = true;        // predict one frame ahead (we write before the world ticks)
static uint64_t g_kinematicSet = 0, g_kinematicRestored = 0;
static std::vector<AActor*> g_madeKinematic;      // so the toggle can put them back

static void* RootPrim(AActor* a) { return a ? UE_FIELD(void*, a, es2off::AActor::RootComponent) : nullptr; }
// Read the flag rather than calling IsSimulatingPhysics: this runs per proxy per frame, and a field read
// costs nothing.
static bool IsSimulating(AActor* a) {
    void* root = RootPrim(a);
    if (!root) return false;
    return (UE_FIELD(uint8_t, root, es2off::UPrimitiveComponent::BodyInstance + es2off::FBodyInstance::bSimulatePhysics_off)
            & es2off::FBodyInstance::bSimulatePhysics_mask) != 0;
}
// VIRTUAL, and ES2 overrides it (UMovementRootComponent::SetSimulatePhysics) — these ship roots are
// exactly that class, so this must dispatch through the vtable. Calling the UPrimitiveComponent RVA
// directly would silently run the base implementation instead of ES2's.
static void SetSimulate(AActor* a, bool sim) {
    if (void* root = RootPrim(a))
        VCall<void, bool>(root, vt::UPrimitiveComponent_SetSimulatePhysics, sim);
}
// Restore everything we converted (toggle off / leaving the role). Actors that died in the meantime are
// simply skipped -- they are gone, and their bodies with them.
static void RestoreSimulation() {
    for (AActor* a : g_madeKinematic) {
        if (!a || !IsValidObject((UObject*)a)) continue;
        SetSimulate(a, true);
        ++g_kinematicRestored;
    }
    g_madeKinematic.clear();
}
static float g_npcRotRate = 18.f;         // rotation convergence, matching the remote-player smoother
static uint64_t g_npcVelWrites = 0, g_npcRotWrites = 0;

using Fn_SetPhysLinVel = void (*)(void* prim, const FVector* v, bool add, FName bone);
static void SetPhysVelocityOn(AActor* a, const FVector& v) {
    void* root = UE_FIELD(void*, a, es2off::AActor::RootComponent);
    if (root) Rva<std::remove_pointer_t<Fn_SetPhysLinVel>>(es2rva::UPrimitiveComponent_SetPhysicsLinearVelocity)(root, &v, false, FName{});
}

// FRepMovement carries the rotation as an FRotator in DEGREES; FTransform wants a quaternion.
// This is UE's own FRotator::Quaternion() term for term.
static FQuat RotatorToQuat(const FRotator& r) {
    const double k = 3.14159265358979323846 / 360.0;      // deg -> rad, halved
    double sp = sin(r.Pitch * k), cp = cos(r.Pitch * k);
    double sy = sin(r.Yaw   * k), cy = cos(r.Yaw   * k);
    double sr = sin(r.Roll  * k), cr = cos(r.Roll  * k);
    return FQuat{ cr * sp * sy - sr * cp * cy,
                 -cr * sp * cy - sr * cp * sy,
                  cr * cp * sy - sr * sp * cy,
                  cr * cp * cy + sr * sp * sy };
}
static FQuat SlerpQuat(FQuat a, const FQuat& b, double t) {
    double d = a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    if (d < 0) { a.X = -a.X; a.Y = -a.Y; a.Z = -a.Z; a.W = -a.W; d = -d; }
    FQuat r;
    if (d > 0.9995) {                                     // nearly parallel: lerp + normalise
        r.X = a.X + (b.X - a.X) * t; r.Y = a.Y + (b.Y - a.Y) * t;
        r.Z = a.Z + (b.Z - a.Z) * t; r.W = a.W + (b.W - a.W) * t;
    } else {
        double th0 = acos(d), th = th0 * t, s0 = sin(th0);
        double sa = sin(th0 - th) / s0, sb = sin(th) / s0;
        r.X = a.X * sa + b.X * sb; r.Y = a.Y * sa + b.Y * sb;
        r.Z = a.Z * sa + b.Z * sb; r.W = a.W * sa + b.W * sb;
    }
    double n = sqrt(r.X * r.X + r.Y * r.Y + r.Z * r.Z + r.W * r.W);
    if (n > 1e-9) { r.X /= n; r.Y /= n; r.Z /= n; r.W /= n; }
    return r;
}

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

        // Checked per frame rather than once, so this also self-heals: whatever re-enables simulation
        // (a replicated physics update, ES2's own code, a level transition) is simply undone next frame.
        // IsSimulatingPhysics is a virtual reading a flag on the body -- cheap enough for a handful of NPCs.
        if (g_npcKinematic && IsSimulating(act)) {
            SetSimulate(act, false);
            ++g_kinematicSet;
            bool known = false;
            for (AActor* k : g_madeKinematic) if (k == act) { known = true; break; }
            if (!known && g_madeKinematic.size() < 256) g_madeKinematic.push_back(act);
        }

        void* rm = reinterpret_cast<char*>(act) + es2off::AActor::ReplicatedMovement;
        const FVector& rl = UE_FIELD(FVector, rm, es2off::FRepMovement::Location);
        const FVector& rv = UE_FIELD(FVector, rm, es2off::FRepMovement::LinearVelocity);
        const FRotator& rr = UE_FIELD(FRotator, rm, es2off::FRepMovement::Rotation);
        if (rl.X == 0 && rl.Y == 0 && rl.Z == 0) continue;            // nothing replicated yet

        NpcTarget& t = g_npcTargets[act];
        if (!t.has || rl.X != t.loc.X || rl.Y != t.loc.Y || rl.Z != t.loc.Z) {
            t.loc = rl; t.vel = rv; t.at = g_npcNow; t.has = true;    // a fresh update landed
        }

        // + dt: this tick runs from the UGameEngine::Tick detour, i.e. BEFORE the world ticks and draws,
        // so whatever we write here is what the player sees one frame from now — aim the prediction at
        // that moment. Costs nothing in smoothness (it is a constant lead, not a faster chase) and takes
        // out most of the standing lag, which matters more now that no physics carries the proxy forward.
        double age = (g_npcNow - t.at) + (g_npcLeadFrame ? dt : 0.0);
        if (age > g_npcMaxExtrap) age = g_npcMaxExtrap;
        FVector predicted{ t.loc.X + t.vel.X * age, t.loc.Y + t.vel.Y * age, t.loc.Z + t.vel.Z * age };

        FTransform cur = GetActorTransform(act);
        double dx = predicted.X - cur.Translation.X, dy = predicted.Y - cur.Translation.Y, dz = predicted.Z - cur.Translation.Z;
        double err = sqrt(dx * dx + dy * dy + dz * dz);
        FTransform next = cur;
        if (err > g_npcSnapDist) { next.Translation = predicted; ++g_npcFollowSnaps; }
        else next.Translation = FVector{ cur.Translation.X + dx * a, cur.Translation.Y + dy * a, cur.Translation.Z + dz * a };
        // A kinematic proxy has nothing else to orient it (its AI runs on the host), so rotation
        // following is not optional there.
        if (g_npcFollowRot || g_npcKinematic) {
            FQuat want = RotatorToQuat(rr);
            // A zeroed rotator is what an actor that has not replicated its rotation yet looks like;
            // snapping such a proxy to identity would be worse than leaving it where it is.
            if (!(rr.Pitch == 0 && rr.Yaw == 0 && rr.Roll == 0)) {
                double ra = dt * g_npcRotRate; if (ra > 1) ra = 1;
                next.Rotation = (err > g_npcSnapDist) ? want : SlerpQuat(cur.Rotation, want, ra);
                ++g_npcRotWrites;
            }
        }
        SetActorTransform(act, next, false, 1 /*TeleportPhysics*/);
        // Only meaningful while the body still simulates; see the measurement note above (it did not help).
        if (g_npcFollowVel && !g_npcKinematic) { SetPhysVelocityOn(act, rv); ++g_npcVelWrites; }
        ++g_npcFollowed;
    }
    if (g_npcTargets.size() > 512) g_npcTargets.clear();
}

// ---------------------------------------------------------------- NPC hitpoints
//
// ES2 replicates no hitpoint state at all, so a client's copy of every enemy sits at full health however
// hard the host is hammering it — no bars moving, no damage read-out, nothing to show a fight is going
// the player's way. The host therefore reports each NPC's hull/shield/armour, but only when it actually
// changes, which keeps a busy fight down to a handful of messages instead of a stream per enemy.
static bool g_npcHpSync = true;
static double g_npcHpAccum = 0;
static float g_npcHpHz = 8.f;
static constexpr size_t kNpcHpPerTick = 8;
static uint64_t g_npcHpSent = 0, g_npcHpApplied = 0;
static uint64_t g_npcDeathsSent = 0, g_npcDeathsPlayed = 0;
static bool g_npcDeathFx = true;
// An NPC dying is invisible on a client: the host runs the death Blueprint (which is what spawns the
// explosion) and the client merely has the actor replicated away, so enemies just blink out. The host
// announces the death as the hull reaches zero — deliberately BEFORE the actor is destroyed, while the
// client can still resolve the NetGUID — and the client plays its own copy's Die, which is a
// BlueprintNativeEvent whose Blueprint half carries the FX. Verified safe to call client-side.
struct NpcHp { float hull = -1, shield = -1, armor = -1; bool announcedDead = false; };
static std::map<AActor*, NpcHp> g_npcHpLast;
static std::vector<AActor*> g_hostNpcCache;
static double g_hostNpcCacheAge = 0;


// Announcing a death by POLLING is too late. HostNpcHpTick runs at 8 Hz over 8 NPCs a tick, so an ND
// could go out ~125 ms after the kill, and the client then defers one more frame off the receive path.
// By then the host has usually destroyed the actor and replication has taken the client's copy with it,
// so ActorFromNetGuid finds nothing and no explosion plays -- measured as 3 deaths, 1 explosion.
//
// Hooking the depletion delegate announces it in the frame it happens. The polled path below stays as a
// backstop (and still owns the hitpoint stream); both share the announcedDead latch so nothing doubles.
//
// The SAME delegate type is used for shield and armor depletion, so the owner's HULL must be checked --
// a dropped shield is not a death.
static void AnnounceNpcDeath(AActor* act) {
    if (!act || !IsValidObject((UObject*)act)) return;
    if (!g_npcHpSync || players::Count() < 2) return;
    if (players::ByPawn(act)) return;                       // a player dying is the respawn path's job
    if (GetHealthRatio(act) > 0.001f) return;               // shield/armor depleted, not the hull
    NpcHp& last = g_npcHpLast[act];
    if (last.announcedDead) return;
    uint64_t guid = NetGuidOf((const UObject*)act);
    if (!guid) return;
    last.announcedDead = true;
    coop::SendToAllClients(Format("ND|%llu", (unsigned long long)guid));
    ++g_npcDeathsSent;
}

using Fn_HealthDepleted = void (*)(const void* self, AActor* owner, AActor* causer, void* instigator, float f);
static Fn_HealthDepleted o_HealthDepleted = nullptr;
static void H_HealthDepleted(const void* self, AActor* owner, AActor* causer, void* instigator, float f) {
    AnnounceNpcDeath(owner);
    o_HealthDepleted(self, owner, causer, instigator, f);
}

static void HostNpcHpTick(float dt) {
    if (!g_npcHpSync || players::Count() < 2) return;
    g_npcHpAccum += dt;
    if (g_npcHpAccum < 1.0 / g_npcHpHz) return;
    g_npcHpAccum = 0;

    g_hostNpcCacheAge -= 1.0 / g_npcHpHz;
    if (g_hostNpcCacheAge <= 0) {
        g_hostNpcCacheAge = 1.0;
        UClass* pawnClass = FindClass("ESPawn");
        g_hostNpcCache.clear();
        if (pawnClass)
            for (AActor* a : GetAllActorsOfClass(GetWorld(), pawnClass))
                if (a && !players::ByPawn(a)) g_hostNpcCache.push_back(a);
        // Mines are not pawns, but they carry a Health component and a client shoots at them like
        // anything else, so their damage has to mirror too.
        if (UClass* mine = FindClass("ProximityMineBase"))
            for (AActor* a : GetAllActorsOfClass(GetWorld(), mine))
                if (a) g_hostNpcCache.push_back(a);
    }

    size_t sent = 0;
    for (AActor* act : g_hostNpcCache) {
        if (sent >= kNpcHpPerTick) break;
        if (!act || !IsValidObject((UObject*)act)) continue;
        float hull = GetHealthRatio(act);
        if (hull < 0) continue;
        float shield = GetShieldRatio(act), armor = GetArmorRatio(act);
        NpcHp& last = g_npcHpLast[act];
        auto same = [](float a, float b) { return fabsf(a - b) < 0.002f; };
        if (same(hull, last.hull) && same(shield, last.shield) && same(armor, last.armor)) continue;
        uint64_t guid = NetGuidOf((const UObject*)act);
        bool wasDead = last.announcedDead;
        if (!guid) {
            // An unreplicated level actor (plants, mines, props) has no guid to key on, so key it by path
            // like the reconcile above. Without this the client's copy stays undamaged right up until the
            // host destroys it, and the plant it is shooting at simply never reacts. Death still travels
            // as NRD from the Destroyed hook, so nothing needs announcing here.
            if (!g_reconcile || !IsUnreplicatedLevelActor(act)) continue;
            last = NpcHp{hull, shield, armor, wasDead};
            coop::SendToAllClients(Format("NHP|%s|%.3f|%.3f|%.3f",
                                          GetPathName((UObject*)act).c_str(), hull, shield, armor));
            ++g_unrepHpSent;
            ++sent;
            continue;
        }
        last = NpcHp{hull, shield, armor, wasDead};
        if (hull <= 0.001f && !last.announcedDead) {
            last.announcedDead = true;
            uint64_t dg = NetGuidOf((const UObject*)act);
            if (dg) { coop::SendToAllClients(Format("ND|%llu", (unsigned long long)dg)); ++g_npcDeathsSent; }
        }
        coop::SendToAllClients(Format("NH|%llu|%.3f|%.3f|%.3f", (unsigned long long)guid, hull, shield, armor));
        ++g_npcHpSent;
        ++sent;
    }
    if (g_npcHpLast.size() > 512) g_npcHpLast.clear();
}


// ---------------------------------------------------------------- damage feedback (numbers + hitmarker)
// ES2 produces the floating damage number AND the hitmarker from one place:
// UGameplayLib::DamageDealtByPlayerOrPlayerFriend classifies the victim's hitpoint component
// (shield / armor / hull), calls AESHUD::ShowHitpointNumbers, then AESHUD::OnPlayerDealtDamage.
// Whether it does any of that is decided by comparing the causer with UGameplayStatics::GetPlayerPawn
// (world, 0) -- the LOCAL player. That is why a client sees neither: on the host the test fails for a
// client's routed fire, so the host correctly stays silent, and the client never runs a damage path of
// its own (it is blocked, see SetClientDamageBlock). Nobody is left to draw it.
//
// The host forwards the event instead, and the client draws it with ES2's own API.
//
// COALESCED, not per-event. A beam weapon calls this every tick; sending one reliable RPC per call is
// exactly the flood that took clients down before (docs/NOTES.md). Damage is summed per victim and
// flushed at 10 Hz, which is also how ES2 itself presents sustained fire.
static uint64_t g_dmgSent = 0, g_dmgShown = 0;
static bool g_dmgNumbers = true;
static bool g_dmgDebug = false;
// One entry per HIT, not per victim. Summing hits made the client draw 3.28 where ES2 would have drawn
// 1.73 and 1.55 -- the number stopped matching the weapon. Individual events are kept and the rate is
// bounded instead; only when a shooter genuinely outruns the budget (a constant beam, which calls this
// every tick) do hits merge, which is what ES2 does for beams anyway.
struct DmgEvent { uint64_t guid; float shield, armor, hull; bool crit, kill; };
static std::map<int, std::vector<DmgEvent>> g_dmgQueue;            // player id -> pending hits
static double g_dmgFlushIn = 0;
static uint64_t g_dmgMerged = 0;
static constexpr size_t kDmgQueueMax = 8;      // per player; beyond this, merge into the same victim
static constexpr int    kDmgPerFlush = 2;      // 20 Hz flush -> at most 40 messages/s per player

using Fn_DamageDealt = void (*)(UObject* hpComp, float amount, void* instigator, AActor* causer,
                                AActor* victim, const void* hit, bool bCritical, bool bRadial);
static Fn_DamageDealt o_DamageDealt = nullptr;

static void H_DamageDealt(UObject* hpComp, float amount, void* instigator, AActor* causer,
                          AActor* victim, const void* hit, bool bCritical, bool bIsKill) {
    o_DamageDealt(hpComp, amount, instigator, causer, victim, hit, bCritical, bIsKill);
    if (!g_dmgNumbers || amount <= 0.f || !hpComp || !victim) return;
    // Attribute through the INSTIGATOR, which is what ES2 itself does (it reads AController::Pawn at
    // +0x2E8 and compares that with the local player). The causer is only the pawn for instant-hit
    // weapons; for anything that launches something it is the projectile actor, so attributing by
    // causer silently dropped every projectile hit -- exactly the weapons whose numbers went missing.
    players::Player* pl = nullptr;
    if (instigator && IsValidObject((UObject*)instigator)) {
        pl = players::ByController((APlayerController*)instigator);
        if (!pl) {
            AActor* ipawn = UE_FIELD(AActor*, instigator, es2off::AController::Pawn);
            if (ipawn) pl = players::ByPawn(ipawn);
        }
    }
    if (!pl && causer) pl = players::ByPawn(causer);        // instant-hit weapons pass the pawn
    // Logged BEFORE the local filter so a host hit and a client hit can be compared side by side --
    // that is what tells you whether a client's routed fire deals the damage its own weapon claims.
    if (g_dmgDebug) {
        LOGF("[dmg] %-6s amount=%.2f comp=%s crit=%d kill=%d causer=%s victim=%s",
             pl ? (pl->local ? "HOST" : "CLIENT") : "other", amount,
             GetObjectClassName(hpComp).c_str(), (int)bCritical, (int)bIsKill,
             GetName((UObject*)causer).c_str(), GetName((UObject*)victim).c_str());
    }
    if (!pl || pl->local || !pl->pc) return;            // the host's own damage: ES2 already drew it
    uint64_t guid = NetGuidOf((const UObject*)victim);
    if (!guid) return;
    auto& q = g_dmgQueue[pl->id];
    DmgEvent* e = nullptr;
    if (q.size() < kDmgQueueMax) {
        q.push_back(DmgEvent{guid, 0, 0, 0, false, false});
        e = &q.back();
    } else {
        // Over budget: fold into the newest hit on the same victim rather than dropping it.
        for (auto it = q.rbegin(); it != q.rend(); ++it) if (it->guid == guid) { e = &*it; break; }
        if (!e) return;
        ++g_dmgMerged;
    }
    // Mirror ES2's own split: the single amount belongs to whichever layer took it.
    if (IsA(hpComp, FindClass("ShieldComponent")))      e->shield += amount;
    else if (IsA(hpComp, FindClass("ArmorComponent")))  e->armor  += amount;
    else                                                e->hull   += amount;
    e->crit = e->crit || bCritical;
    e->kill = e->kill || bIsKill;
}

// Host: drain the accumulator. Runs on the game thread, never from a message handler.
static void HostDamageNumbersTick(float dt) {
    if (g_dmgQueue.empty()) return;
    g_dmgFlushIn -= dt;
    if (g_dmgFlushIn > 0) return;
    g_dmgFlushIn = 0.05;                                 // 20 Hz
    for (auto& [id, q] : g_dmgQueue) {
        players::Player* pl = players::ById(id);
        if (!pl || !pl->pc) { q.clear(); continue; }
        int sent = 0;
        auto it = q.begin();
        for (; it != q.end() && sent < kDmgPerFlush; ++it, ++sent) {
            if (g_dmgDebug)
                LOGF("[dmg] flush shield=%.2f armor=%.2f hull=%.2f crit=%d kill=%d",
                     it->shield, it->armor, it->hull, (int)it->crit, (int)it->kill);
            coop::SendToClient(pl->pc, Format("DM|%llu|%.1f|%.1f|%.1f|%d|%d", (unsigned long long)it->guid,
                                              it->shield, it->armor, it->hull, (int)it->crit, (int)it->kill));
            ++g_dmgSent;
        }
        q.erase(q.begin(), it);                          // anything left rides the next flush
    }
}

// Client: PC -> MyHUD is the AESHUD; its IngameHudWidget owns the pooled text panel that draws numbers.
static bool FindDamageWidgets(UObject** hudOut, UObject** panelOut) {
    UWorld* w = GetWorld();
    APlayerController* pc = w ? GetFirstLocalPlayerController(w) : nullptr;
    UObject* hud = pc ? UE_FIELD(UObject*, pc, es2off::APlayerController::MyHUD) : nullptr;
    if (!hud || !IsValidObject(hud)) return false;
    UObject* widget = nullptr, *panel = nullptr;
    for (auto& p : GetProperties((UStruct*)GetClass(hud), true))
        if (p.Name == "IngameHudWidget" && p.TypeName == "ObjectProperty") { widget = UE_FIELD(UObject*, hud, p.Offset); break; }
    if (!widget || !IsValidObject(widget)) return false;
    for (auto& p : GetProperties((UStruct*)GetClass(widget), true))
        if (p.Name == "PooledHudText" && p.TypeName == "ObjectProperty") { panel = UE_FIELD(UObject*, widget, p.Offset); break; }
    if (!panel || !IsValidObject(panel)) return false;
    *hudOut = hud; *panelOut = panel;
    return true;
}

struct PendingDmg { uint64_t guid; float shield, armor, hull; bool crit, kill; };
static std::vector<PendingDmg> g_pendingDmg;

static bool ApplyDamageNumbers(const std::string& body) {
    if (!g_dmgNumbers) return true;
    unsigned long long guid = 0; float sh = 0, ar = 0, hu = 0; int crit = 0, kill = 0;
    if (sscanf(body.c_str(), "%llu|%f|%f|%f|%d|%d", &guid, &sh, &ar, &hu, &crit, &kill) != 6) return true;
    if (g_pendingDmg.size() < 64) g_pendingDmg.push_back({guid, sh, ar, hu, crit != 0, kill != 0});
    return true;
}

// Deferred for the usual reason: message handlers run inside the net code consuming a bunch, and these
// calls run Blueprint graphs. See ClientDeathFxTick.
void ClientDamageNumbersTick(float) {
    if (g_pendingDmg.empty()) return;
    std::vector<PendingDmg> due;
    due.swap(g_pendingDmg);
    UObject* hud = nullptr, *panel = nullptr;
    if (!FindDamageWidgets(&hud, &panel)) return;
    UFunction* addText = FindFunction(panel, "AddDamageTextWithActor");
    UFunction* marker  = FindFunction(hud, "OnPlayerDealtDamage");
    if (!addText) return;
    for (const PendingDmg& d : due) {
        AActor* victim = ActorFromNetGuid(d.guid);
        if (!victim || !IsValidObject((UObject*)victim)) continue;
        struct { AActor* Target; bool bIsCritical; float Shield, Armor, Hull; } parms{};
        parms.Target = victim; parms.bIsCritical = d.crit;
        parms.Shield = d.shield; parms.Armor = d.armor; parms.Hull = d.hull;
        ProcessEvent(panel, addText, &parms);
        // OnPlayerDealtDamage's parameter is the kill-confirm bit -- ES2 gets it from
        // UHealthComponent::IsDead() at the call site, so pass the real thing rather than a flat false.
        if (marker) { bool bIsKill = d.kill; ProcessEvent(hud, marker, &bIsKill); }
        ++g_dmgShown;
    }
}

// Play the destruction effect on our own copy so the player sees the enemy blow up rather than vanish.
//
// The host spawns a BP_Explosion_Base_C per kill, but that actor's CDO has RemoteRole ROLE_None, so it
// never replicates -- a client sees the ship silently disappear when the host destroys it. The client
// has to spawn its own.
//
// BP_ShipBase_C::SpawnExplosion is the right entry point, and the choice is not arbitrary:
//   Die              is ESPawn's, takes THREE parameters (EventInstigator, InstigatorPawn,
//                    DamageCauser) and was previously called here with a null parameter block. It did
//                    nothing observable -- measured on the host, with hitpoints already at zero, the
//                    actor survived and no explosion actor appeared.
//   Explode          runs the whole sequence including DestroyAfterExploding, which destroys a
//                    replicated actor on the client. The explosion appeared and then the client died
//                    seconds later, once that timer fired.
//   SpawnExplosion   takes no parameters and only spawns the effect. Verified live: explosion actor
//                    count went up and the client stayed healthy.
//
// DEFERRED deliberately. Message handlers run inside UActorChannel::ReceivedBunch -> ReceivedRPC ->
// execClientMessage, i.e. in the middle of the net code consuming a bunch. Running a Blueprint that
// spawns effects re-entrantly from inside RPC dispatch took the client down with an access violation in
// ProcessEvent's own parameter memcpy. The same call is safe one tick later, off the receive path.
static std::vector<uint64_t> g_pendingDeaths;

static uint64_t g_deathsRx = 0, g_deathsNoActorAtRx = 0, g_deathsNoActorAtDrain = 0;

// --- when to play it, and what to play -------------------------------------------------------
//
// TIMING. The host announces on hull<=0, deliberately early so the guid still resolves — but hull-zero
// is where ES2's death sequence STARTS. The ship then tumbles out of control and only explodes and is
// destroyed at the end of it. Firing our explosion on the announcement therefore played it seconds too
// early: the player saw the blast, then the ship tumbling on past it, then vanishing. (The kinematic
// proxies made this obvious, because the tumble is now smooth and legible rather than lost in jitter.)
//
// So the announcement no longer plays anything: it just marks that actor as "the host says this one is
// dead". The FX is played from AESPawn::Destroyed, i.e. the moment our copy actually goes away, which is
// exactly when the host destroyed it. That also removes the resolution race entirely — inside Destroyed
// the actor is still fully valid, so there is no "could not resolve the guid in time" case left.
//
// The marking matters: a proxy is also destroyed when it merely leaves relevancy, and that must NOT
// produce an explosion. Only actors the host explicitly announced are in the set.
//
// WHAT. `SpawnExplosion` is a BP_ShipBase_C function, so only the ship branch has it — turrets
// (BP_TurretBase_C) and anemones (BP_Cave_Anemone_Base_C) hang off BP_PawnBase_C directly and have none.
// The old code silently `continue`d for those, so they vanished with no explosion and no counter said so.
// Now the class-independent fallback spawns the same explosion actor the host does.
static std::map<AActor*, uint64_t> g_dying;        // actor -> guid, announced dead by the host
static std::map<AActor*, double> g_dyingSince;
static double g_fxNow = 0;
static double g_dyingDeadline = 8.0;               // safety net: never lose an explosion outright
static uint64_t g_fxAtDestroy = 0, g_fxTimeout = 0, g_fxNoFunction = 0, g_fxFallback = 0, g_fxFallbackFailed = 0;
static bool g_fxFallbackOn = true;

// The host spawns a BP_Explosion_Base_C per kill; it has RemoteRole ROLE_None, which is exactly why it
// never reaches a client. Resolved by scanning for the class object (a Blueprint-generated class, so
// FindClass's native-only short-name path cannot see it).
static UClass* g_explosionClass = nullptr;
static bool g_explosionSearched = false;
static UClass* ExplosionClass() {
    if (g_explosionSearched) return g_explosionClass;
    g_explosionSearched = true;
    UClass* cc = ClassClass();
    if (!cc) { g_explosionSearched = false; return nullptr; }   // too early; try again later
    ForEachObject([&](UObject* o) {
        if (!IsA(o, cc)) return true;
        if (GetName(o) != "BP_Explosion_Base_C") return true;
        g_explosionClass = (UClass*)o;
        return false;
    });
    LOGF("[combat] explosion fallback class %s", g_explosionClass ? GetPathName((UObject*)g_explosionClass).c_str() : "NOT FOUND");
    return g_explosionClass;
}

using Fn_BeginDeferredSpawn = AActor* (*)(const UObject* wco, UClass* const* cls, const FTransform* xf,
                                          int collisionHandling, AActor* owner, int scaleMethod);
using Fn_FinishSpawningActor = AActor* (*)(AActor* actor, const FTransform* xf, int scaleMethod);

static bool SpawnExplosionAt(const FTransform& at) {
    UClass* cls = ExplosionClass();
    UWorld* w = GetWorld();
    if (!cls || !w) return false;
    UClass* arg = cls;                       // TSubclassOf is passed by hidden pointer
    AActor* a = Rva<std::remove_pointer_t<Fn_BeginDeferredSpawn>>(es2rva::UGameplayStatics_BeginDeferredActorSpawnFromClass)(
        (const UObject*)w, &arg, &at, 1 /*AdjustIfPossibleButAlwaysSpawn*/, nullptr, 0);
    if (!a) return false;
    Rva<std::remove_pointer_t<Fn_FinishSpawningActor>>(es2rva::UGameplayStatics_FinishSpawningActor)(a, &at, 0);
    return true;
}

// Play whatever this class can manage, at `act`'s current transform. Called with the actor still alive.
static void PlayDeathFx(AActor* act) {
    if (!act || !IsValidObject((UObject*)act)) return;
    UFunction* fn = FindFunction((UObject*)act, "SpawnExplosion");
    if (fn && UE_FIELD(uint8_t, fn, es2off::UFunction::NumParms) == 0) {
        ProcessEvent((UObject*)act, fn, nullptr);   // only ever a null parm block for NumParms==0
        ++g_npcDeathsPlayed;
        return;
    }
    ++g_fxNoFunction;                                // e.g. turrets, anemones: no BP_ShipBase_C in the chain
    if (!g_fxFallbackOn) return;
    if (SpawnExplosionAt(GetActorTransform(act))) { ++g_fxFallback; ++g_npcDeathsPlayed; }
    else ++g_fxFallbackFailed;
}

using Fn_ActorVoid = void (*)(AActor*);
static Fn_ActorVoid o_ESPawnDestroyed = nullptr;
// Every actor destruction funnels through UWorld::DestroyActor -- AActor::Destroy and K2_DestroyActor
// both land here -- so one hook covers plants, mines, containers and anything else unreplicated. The
// AESPawn::Destroyed hook this replaces only ever saw pawns, which is why mines and loot containers were
// still ghosts after the first attempt at this.
using Fn_DestroyActor = bool (*)(UWorld* w, AActor* a, bool netForce, bool modifyLevel);
static Fn_DestroyActor o_DestroyActor = nullptr;
static bool H_DestroyActor(UWorld* w, AActor* a, bool netForce, bool modifyLevel) {
    if (a && g_reconcile && coop::CurrentRole() == coop::Role::Host && players::Count() > 1
        && IsLevelPlaced(a) && !GetReplicates(a) && !players::ByPawn(a)
        && IsUnreplicatedLevelActor(a)) {
        coop::SendToAllClients("NRD|" + GetPathName((UObject*)a));
        ++g_ghostReported;
    }
    return o_DestroyActor(w, a, netForce, modifyLevel);
}

static void H_ESPawnDestroyed(AActor* a) {
    // Host: an unreplicated level actor just died here. Nothing about it is networked -- no channel, no
    // guid, no destruction message -- so the client would keep its own copy standing forever. Tell it by
    // path, which is the one identity the two machines share for level actors. This is what makes a
    // client's fire visibly kill a plant or a mine: the client's shot is applied on the host (the client
    // never damages anything itself), and this is the host reporting the outcome.
    // Our copy is going away. If the host told us this one died, this is the moment to show it.
    if (a && coop::CurrentRole() == coop::Role::Client && !g_dying.empty()) {
        auto it = g_dying.find(a);
        if (it != g_dying.end()) {
            PlayDeathFx(a);
            ++g_fxAtDestroy;
            g_dying.erase(it);
            g_dyingSince.erase(a);
        }
    }
    o_ESPawnDestroyed(a);
}

static bool ApplyNpcDeath(const std::string& body) {
    if (!g_npcDeathFx) return true;
    unsigned long long guid = 0;
    if (sscanf(body.c_str(), "%llu", &guid) != 1) return true;
    ++g_deathsRx;
    // Resolving here is a pure lookup -- no ProcessEvent -- so it is safe on the receive path, and this
    // is the moment the proxy is most certainly still around (the host announces before destroying it).
    // Nothing is played yet: we only remember that this actor is doomed, and the FX goes off when it
    // actually dies (H_ESPawnDestroyed). See the note above.
    AActor* act = ActorFromNetGuid(guid);
    if (!act) { ++g_deathsNoActorAtRx; return true; }
    g_dying[act] = guid;
    g_dyingSince[act] = g_fxNow;
    return true;
}

// Runs on the game thread, outside the net receive path. Only the safety net lives here now.
void ClientDeathFxTick(float dt) {
    g_fxNow += dt;
    if (g_dying.empty()) return;
    for (auto it = g_dying.begin(); it != g_dying.end(); ) {
        AActor* a = it->first;
        if (!IsValidObject((UObject*)a)) {
            // Destroyed without our hook seeing it (or already gone): nothing left to play it on.
            ++g_deathsNoActorAtDrain;
            g_dyingSince.erase(a);
            it = g_dying.erase(it);
            continue;
        }
        if (g_fxNow - g_dyingSince[a] >= g_dyingDeadline) {
            // The host said it died but our copy is still here well past any death sequence. Play it
            // anyway rather than silently losing the explosion.
            PlayDeathFx(a);
            ++g_fxTimeout;
            g_dyingSince.erase(a);
            it = g_dying.erase(it);
            continue;
        }
        ++it;
    }
}

// NHP|<path>|hull|shield|armor -- the unreplicated-level-actor twin of NH.
static bool ApplyUnrepHp(const std::string& body) {
    size_t bar = body.find('|');
    if (bar == std::string::npos) return true;
    float hull = -1, shield = -1, armor = -1;
    if (sscanf(body.c_str() + bar + 1, "%f|%f|%f", &hull, &shield, &armor) < 1) return true;
    UObject* o = FindObject(body.substr(0, bar));
    if (!o) return true;
    AActor* act = (AActor*)o;
    ApplyRatio(act, "HealthComponent", hull);
    ApplyRatio(act, "ShieldComponent", shield);
    ApplyRatio(act, "ArmorComponent", armor);
    ++g_unrepHpApplied;
    return true;
}

static bool ApplyNpcHp(const std::string& body) {
    unsigned long long guid = 0; float hull = -1, shield = -1, armor = -1;
    if (sscanf(body.c_str(), "%llu|%f|%f|%f", &guid, &hull, &shield, &armor) < 2) return true;
    AActor* act = ActorFromNetGuid(guid);
    if (!act) return true;
    ApplyRatio(act, "HealthComponent", hull);
    ApplyRatio(act, "ShieldComponent", shield);
    ApplyRatio(act, "ArmorComponent", armor);
    ++g_npcHpApplied;
    return true;
}

// ---------------------------------------------------------------- messages
bool OnServerOp(APlayerController* from, const std::string& op, const std::string& body) {
    if (op == "NRQ") return AnswerGhostQuery(from, body);
    if (op == "AIM") {
        double x = 0, y = 0, z = 0, tx = 0, ty = 0, tz = 0;
        unsigned long long l0 = 0, l1 = 0, a0 = 0, a1 = 0, tg = 0;
        int n = sscanf(body.c_str(), "%lf|%lf|%lf|%llu|%llu|%llu|%llu|%llu|%lf|%lf|%lf",
                       &x, &y, &z, &l0, &l1, &a0, &a1, &tg, &tx, &ty, &tz);
        if (n < 3) return true;
        players::Player* p = players::ByController(from);
        if (!p) return true;
        g_aim[p->id] = FVector{x, y, z};
        if (n >= 7) g_playerAutoAim[p->id] = {a0, a1};
        if (n >= 5) g_playerLock[p->id] = {l0, l1};   // stamped per tick, see ApplyLock
        if (n >= 11) { g_playerTargetGuid[p->id] = tg; g_playerTargetSeen[p->id] = FVector{tx, ty, tz}; }
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
    if (op == "NRD") return ApplyGhostList(body);
    if (op == "NHP") return ApplyUnrepHp(body);
    if (op == "WF") return ApplyNpcFire(body);
    if (op == "WA") return ApplyNpcAim(body);
    if (op == "NH") return ApplyNpcHp(body);
    if (op == "ND") return ApplyNpcDeath(body);
    if (op == "DM") return ApplyDamageNumbers(body);
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
// Every map change (travel, connect, the loadout pawn swap is NOT one) invalidates the UObject*/AActor*
// keys below. UObject memory is recycled, so a stale entry is not merely dead weight: a new NPC at an
// old address would inherit `announcedDead` and never get its explosion, or an old aim sample.
void OnWorldChanged() {
    g_reconcileAnswered = false; g_reconcileTries = 0; g_reconcileWait = 0;   // new map, new level actors
    g_npcFireState.clear();
    g_npcAim.clear(); g_npcLock.clear(); g_npcAutoAim.clear();
    g_npcTargets.clear(); g_npcCache.clear(); g_npcCacheAge = 0;
    g_npcHpLast.clear(); g_hostNpcCache.clear(); g_hostNpcCacheAge = 0;
    g_aim.clear(); g_playerAutoAim.clear(); g_playerLock.clear();
    g_playerTargetGuid.clear(); g_playerTargetSeen.clear();
    g_dmgQueue.clear(); g_pendingDmg.clear(); g_pendingDeaths.clear();
    g_dying.clear(); g_dyingSince.clear();
    g_explosionClass = nullptr; g_explosionSearched = false;   // classes are per-world-load in practice
    // The bodies themselves are gone with the old world; just forget them (restoring would dereference
    // freed actors, and the fresh world's proxies get converted again on sight).
    g_madeKinematic.clear();
    // (the jitter probe validates its target every tick and stops by itself)
}

void Tick(float dt, bool isHost) {
    if (!isHost) return;
    HostNpcAimTick(dt);
    HostNpcHpTick(dt);
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

// delegate <objAddr> <hexOffset> — decode a dynamic multicast delegate's invocation list.
// TMulticastScriptDelegate is just TArray<TScriptDelegate>, and TScriptDelegate is
// { FWeakObjectPtr Object; FName FunctionName } — so who is listening is directly readable. This is how
// to tell "the HUD never subscribed" apart from "the HUD subscribed to a component that is now dead".
static void CmdDelegate(const console::Args& a, std::string& out) {
    if (a.size() < 3) { out = "usage: delegate <objAddr> <hexOffset>\n"; return; }
    UObject* obj = (UObject*)strtoull(a[1].c_str(), nullptr, 16);
    if (!IsValidObject(obj)) { out = "not a live UObject\n"; return; }
    uint32_t off = (uint32_t)strtoul(a[2].c_str(), nullptr, 16);
    struct Entry { int32_t idx; int32_t serial; FName fn; };
    struct RawArray { Entry* Data; int32_t Num; int32_t Max; };
    RawArray& list = UE_FIELD(RawArray, obj, off);
    if (list.Num < 0 || list.Num > 256) { out = Format("implausible list (num=%d)\n", list.Num); return; }
    out += Format("%s +0x%X: %d listener(s)\n", GetName(obj).c_str(), off, list.Num);
    for (int i = 0; i < list.Num; ++i) {
        UObject* target = (list.Data[i].idx >= 0) ? ObjectAt(list.Data[i].idx) : nullptr;
        out += Format("  [%d] %-42s :: %s\n", i,
                      target ? GetFullName(target).c_str() : "<dead/none>",
                      list.Data[i].fn.ToString().c_str());
    }
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
    if (a.size() > 2 && a[1] == "deathfx") { g_npcDeathFx = a[2] == "1"; out += Format("npc death fx %d\n", (int)g_npcDeathFx); return; }
    if (a.size() > 2 && a[1] == "fxfallback") { g_fxFallbackOn = a[2] == "1"; out += Format("explosion fallback %d\n", (int)g_fxFallbackOn); return; }
    // fxtest <guid>: play the death FX on that actor right now, so both paths (a ship's own
    // SpawnExplosion and the spawned-actor fallback) can be exercised without waiting for a kill.
    if (a.size() > 2 && a[1] == "fxtest") {
        AActor* t = ActorFromNetGuid(strtoull(a[2].c_str(), nullptr, 10));
        if (!t) { out += "guid not resolved here\n"; return; }
        uint64_t p0 = g_npcDeathsPlayed, f0 = g_fxFallback, n0 = g_fxNoFunction, e0 = g_fxFallbackFailed;
        PlayDeathFx(t);
        out += Format("%s: played=%llu fallbackSpawned=%llu noFn=%llu failed=%llu (explosionClass=%s)\n",
                      GetName((UObject*)t).c_str(),
                      (unsigned long long)(g_npcDeathsPlayed - p0), (unsigned long long)(g_fxFallback - f0),
                      (unsigned long long)(g_fxNoFunction - n0), (unsigned long long)(g_fxFallbackFailed - e0),
                      ExplosionClass() ? GetName((UObject*)ExplosionClass()).c_str() : "none");
        return;
    }
    // Diagnostic only: the block is normally driven purely by role. Lifting it lets a client run ES2's
    // real damage path, which is how the impact/hitmarker/destruction visuals are produced -- and also
    // how it reaches the null AESGameModeBase. Useful for measuring exactly where that path dies.
    if (a.size() > 2 && a[1] == "dmgdebug") { g_dmgDebug = a[2] == "1"; out += Format("dmg debug %d\n", (int)g_dmgDebug); return; }
    if (a.size() > 2 && a[1] == "dmgnumbers") { g_dmgNumbers = a[2] == "1"; out += Format("damage numbers %d\n", (int)g_dmgNumbers); return; }
    if (a.size() > 2 && a[1] == "damageblock") { SetClientDamageBlock(a[2] == "1"); out += Format("client damage block %s\n", a[2].c_str()); return; }
    if (a.size() > 2 && a[1] == "npchp") { g_npcHpSync = a[2] == "1"; out += Format("npc hp sync %d\n", (int)g_npcHpSync); return; }
    if (a.size() > 2 && a[1] == "npcfollow") { g_npcFollow = a[2] == "1"; out += Format("npc follow %d\n", (int)g_npcFollow); return; }
    if (a.size() > 2 && a[1] == "followrate") { g_npcFollowRate = (float)atof(a[2].c_str()); if (!(g_npcFollowRate >= 0.f)) g_npcFollowRate = 0.f; out += Format("rate %.1f\n", g_npcFollowRate); return; }
    if (a.size() > 2 && a[1] == "kinematic") {
        bool on = a[2] == "1";
        if (!on && g_npcKinematic) RestoreSimulation();     // put the bodies back before we stop driving them
        g_npcKinematic = on;
        out += Format("npc proxies kinematic %d (restored %llu)\n", (int)g_npcKinematic, (unsigned long long)g_kinematicRestored);
        return;
    }
    if (a.size() > 2 && a[1] == "lead") { g_npcLeadFrame = a[2] == "1"; out += Format("npc one-frame lead %d\n", (int)g_npcLeadFrame); return; }
    if (a.size() > 2 && a[1] == "npcvel") { g_npcFollowVel = a[2] == "1"; out += Format("npc velocity follow %d\n", (int)g_npcFollowVel); return; }
    if (a.size() > 2 && a[1] == "npcrot") { g_npcFollowRot = a[2] == "1"; out += Format("npc rotation follow %d\n", (int)g_npcFollowRot); return; }
    if (a.size() > 2 && a[1] == "rotrate") { g_npcRotRate = (float)atof(a[2].c_str()); if (!(g_npcRotRate >= 0.f)) g_npcRotRate = 0.f; out += Format("rot rate %.1f\n", g_npcRotRate); return; }
    if (a.size() > 2 && a[1] == "clientregen") { g_blockClientRegen = a[2] == "0"; out += Format("client regen blocked %d\n", (int)g_blockClientRegen); return; }
    if (a.size() > 2 && a[1] == "aimattarget") { g_aimAtTarget = a[2] == "1"; out += Format("aim-at-target %d\n", (int)g_aimAtTarget); return; }
    if (a.size() > 2 && a[1] == "autoaim") { g_autoAimSync = a[2] == "1"; out += Format("autoaim sync %d\n", (int)g_autoAimSync); return; }
    if (a.size() > 2 && a[1] == "locksync") { g_lockSync = a[2] == "1"; out += Format("lock sync %d\n", (int)g_lockSync); return; }
    if (a.size() > 2 && a[1] == "npcaim") { g_npcAimSync = a[2] == "1"; out += Format("npc aim sync %d\n", (int)g_npcAimSync); return; }
    if (a.size() > 2 && a[1] == "aimsync") { g_aimSync = a[2] == "1"; out += Format("aim sync %d\n", (int)g_aimSync); return; }
    if (a.size() > 2 && a[1] == "reconcile") { g_reconcile = a[2] == "1"; g_reconcileAnswered = false; g_reconcileTries = 0; g_reconcileWait = 0; }
    if (a.size() > 2 && a[1] == "hphz") { g_healthHz = (float)atof(a[2].c_str()); if (!(g_healthHz >= 1.f)) g_healthHz = 1.f; }   // 0 meant "never" (1/0 = inf)
    if (a.size() > 2 && a[1] == "npcfire") g_mirrorNpcFire = a[2] == "1";
    out += Format("unreplicatedHp sent=%llu applied=%llu\n",
                  (unsigned long long)g_unrepHpSent, (unsigned long long)g_unrepHpApplied);
    out += Format("reconcile=%d answered=%d tries=%d | asked=%llu reportedDead=%llu ghostsRetired=%llu\n",
                  (int)g_reconcile, (int)g_reconcileAnswered, g_reconcileTries, (unsigned long long)g_ghostAsked,
                  (unsigned long long)g_ghostReported, (unsigned long long)g_ghostKilled);
    out += Format("npcFireMirror=%d sent=%llu applied=%llu unresolved=%llu deduped=%llu\n", (int)g_mirrorNpcFire,
                  (unsigned long long)g_npcFireSent, (unsigned long long)g_npcFireApplied,
                  (unsigned long long)g_npcFireUnresolved, (unsigned long long)g_npcFireDeduped);
    out += Format("autoAimSync=%d autoAimApplied=%llu\n", (int)g_autoAimSync, (unsigned long long)g_autoAimApplied);
    out += Format("lockSync=%d lockApplied=%llu guidScans=%llu\n", (int)g_lockSync,
                  (unsigned long long)g_lockApplied, (unsigned long long)g_guidScans);
    out += Format("npcAimSync=%d npcAimSent=%llu npcAimApplied=%llu npcAimTracked=%d\n",
                  (int)g_npcAimSync, (unsigned long long)g_npcAimSent, (unsigned long long)g_npcAimApplied, (int)g_npcAim.size());
    out += Format("dmgNumbers=%d dmgSent=%llu dmgShown=%llu dmgMerged=%llu\n", (int)g_dmgNumbers,
                  (unsigned long long)g_dmgSent, (unsigned long long)g_dmgShown, (unsigned long long)g_dmgMerged);
    // deathsRx < deathsSent is not a fault: the host also kills NPCs the client never had replicated
    // to it. What must hold is deathsRx == deathsPlayed + the two loss counters.
    out += Format("deathsRx=%llu deathsNoActorAtRx=%llu deathsNoActorAtDrain=%llu\n",
                  (unsigned long long)g_deathsRx,
                  (unsigned long long)g_deathsNoActorAtRx, (unsigned long long)g_deathsNoActorAtDrain);
    out += Format("fxAtDestroy=%llu fxTimeout=%llu pendingDying=%d | noSpawnExplosionFn=%llu fallbackSpawned=%llu fallbackFailed=%llu\n",
                  (unsigned long long)g_fxAtDestroy, (unsigned long long)g_fxTimeout, (int)g_dying.size(),
                  (unsigned long long)g_fxNoFunction, (unsigned long long)g_fxFallback,
                  (unsigned long long)g_fxFallbackFailed);
    out += Format("npcDeathFx=%d deathsSent=%llu deathsPlayed=%llu\n", (int)g_npcDeathFx,
                  (unsigned long long)g_npcDeathsSent, (unsigned long long)g_npcDeathsPlayed);
    out += Format("npcHpSync=%d npcHpSent=%llu npcHpApplied=%llu\n", (int)g_npcHpSync,
                  (unsigned long long)g_npcHpSent, (unsigned long long)g_npcHpApplied);
    out += Format("npcFollow=%d rate=%.1f followed=%llu snaps=%llu tracked=%d\n", (int)g_npcFollow,
                  g_npcFollowRate, (unsigned long long)g_npcFollowed, (unsigned long long)g_npcFollowSnaps,
                  (int)g_npcCache.size());
    out += Format("npcFollowVel=%d velWrites=%llu | npcFollowRot=%d rotRate=%.1f rotWrites=%llu\n",
                  (int)g_npcFollowVel, (unsigned long long)g_npcVelWrites,
                  (int)g_npcFollowRot, g_npcRotRate, (unsigned long long)g_npcRotWrites);
    out += Format("npcLeadFrame=%d\n", (int)g_npcLeadFrame);
    out += Format("npcKinematic=%d madeKinematic=%llu restored=%llu tracked=%d\n", (int)g_npcKinematic,
                  (unsigned long long)g_kinematicSet, (unsigned long long)g_kinematicRestored,
                  (int)g_madeKinematic.size());
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
    console::Register("delegate", "delegate <objAddr> <hexOff> - who is bound to a multicast delegate", CmdDelegate);
    console::Register("npcpos", "npcpos [max] - NPC pawn positions keyed by NetGUID", CmdNpcPos);
    console::Register("guid", "guid <n> - resolve a NetGUID to an actor on this machine", CmdGuid);
    console::RegisterTick("jitter", JitterTick);
    console::RegisterTick("npcfollow", NpcFollowTick);
    console::RegisterTick("deathfx", ClientDeathFxTick);
    console::RegisterTick("dmgnum", ClientDamageNumbersTick);
    console::RegisterTick("dmgnumhost", HostDamageNumbersTick);
    console::Register("aiminfo", "aiminfo [npc] - weapon FocusLocation per player, or per NPC keyed by NetGUID", CmdAimInfo);
    console::Register("hp", "hp [Class] - health/shield ratios of pawns in the world", CmdHp);
    console::Register("lock", "lock [playerId] - acquire closest target (host: on that player's server-side pawn)", CmdLock);
    console::Register("aim", "aim [playerId] - lock closest target and point the hull at it (test aid)", CmdAim);
    console::Register("god", "god [0|1] - make the local ship undepletable (test aid)", CmdGod);
}

void OnInit() {
    // installed but left OFF; coop enables it only while this process is a network client
    { void* orig = nullptr;
      hooks::Install("UGameplayLib::DamageDealtByPlayerOrPlayerFriend", es2rva::UGameplayLib_DamageDealtByPlayerOrPlayerFriend,
                     (void*)&H_DamageDealt, (void**)&o_DamageDealt);
      hooks::Install("FOnHealthDepletedDelegate::Broadcast", es2rva::FOnHealthDepletedDelegate_Broadcast,
                     (void*)&H_HealthDepleted, (void**)&o_HealthDepleted);
      hooks::Install("UGameplayLib::ApplyESPointDamage", es2rva::UGameplayLib_ApplyESPointDamage, (void*)&H_ApplyESPointDamage, &orig);
      hooks::Enable("UGameplayLib::ApplyESPointDamage", false); }
    { void* orig = nullptr;
      hooks::Install("UGameplayLib::ApplyESRadialDamage", es2rva::UGameplayLib_ApplyESRadialDamage, (void*)&H_ApplyESRadialDamage, &orig);
      hooks::Enable("UGameplayLib::ApplyESRadialDamage", false); }
    hooks::Install("AESPawn::Destroyed", es2rva::AESPawn_Destroyed, (void*)&H_ESPawnDestroyed, (void**)&o_ESPawnDestroyed);
    hooks::Install("UWorld::DestroyActor", es2rva::UWorld_DestroyActor, (void*)&H_DestroyActor, (void**)&o_DestroyActor);
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
