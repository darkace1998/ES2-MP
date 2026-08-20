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

static void* LocalGuidCache() {
    UNetDriver* nd = GetNetDriver(GetWorld());
    return nd ? UE_FIELD(void*, nd, es2off::UNetDriver::GuidCache) : nullptr;   // TSharedPtr: object first

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
static float g_aimHz = 20.f;
static bool g_aimSync = true;
static uint64_t g_aimSent = 0, g_aimApplied = 0;

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

// Client: report where our own weapons are pointing.
void ClientAimTick(float dt) {
    if (!g_aimSync || coop::CurrentRole() != coop::Role::Client) return;
    g_aimAccum += dt;
    if (g_aimAccum < 1.0 / g_aimHz) return;
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
    coop::SendToServer(Format("AIM|%.1f|%.1f|%.1f", focus.X, focus.Y, focus.Z));
    ++g_aimSent;
}

// Host: stamp the reporting player's aim onto their server-side weapons before ES2 reads it.
static void H_WeaponTick(UObject* comp, float dt, int tickType, void* tickFn) {
    if (g_aimSync && !g_aim.empty() && comp && coop::CurrentRole() == coop::Role::Host) {
        AActor* owner = UE_FIELD(AActor*, comp, es2off::UActorComponent::OwnerPrivate);
        if (owner) {
            if (players::Player* p = players::ByPawn(owner)) {
                auto it = g_aim.find(p->id);
                if (it != g_aim.end() && !p->local) {
                    UE_FIELD(FVector, comp, es2off::UWeaponComponent::FocusLocation) = it->second;
                    UE_FIELD(FVector, comp, es2off::UWeaponComponent::ClampedNonAutoAimedFocusLocation) = it->second;
                    ++g_aimApplied;
                }
            }
        }
    }
    o_WeaponTick(comp, dt, tickType, tickFn);
}

// ---------------------------------------------------------------- messages
bool OnServerOp(APlayerController* from, const std::string& op, const std::string& body) {
    if (op == "AIM") {
        double x = 0, y = 0, z = 0;
        if (sscanf(body.c_str(), "%lf|%lf|%lf", &x, &y, &z) != 3) return true;
        if (players::Player* p = players::ByController(from)) g_aim[p->id] = FVector{x, y, z};
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
        out += Format("     focus=(%.0f, %.0f, %.0f) autoAimTarget=%s\n", focus.X, focus.Y, focus.Z,
                      (aim && IsValidObject(aim)) ? GetName(aim).c_str() : "none");
    }
    if (out.empty()) out = "no players\n";
}

static void CmdCombat(const console::Args& a, std::string& out) {
    if (a.size() > 2 && a[1] == "route") g_routeFire = a[2] == "1";
    if (a.size() > 2 && a[1] == "localfire") g_localFire = a[2] == "1";
    if (a.size() > 2 && a[1] == "aimsync") { g_aimSync = a[2] == "1"; out += Format("aim sync %d\n", (int)g_aimSync); return; }
    if (a.size() > 2 && a[1] == "hphz") g_healthHz = (float)atof(a[2].c_str());
    if (a.size() > 2 && a[1] == "npcfire") g_mirrorNpcFire = a[2] == "1";
    out += Format("npcFireMirror=%d sent=%llu applied=%llu unresolved=%llu deduped=%llu\n", (int)g_mirrorNpcFire,
                  (unsigned long long)g_npcFireSent, (unsigned long long)g_npcFireApplied,
                  (unsigned long long)g_npcFireUnresolved, (unsigned long long)g_npcFireDeduped);
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
    console::Register("aiminfo", "aiminfo - weapon FocusLocation per player (compare host vs client)", CmdAimInfo);
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
    hooks::Install("UWeaponComponent::TickComponent", es2rva::UWeaponComponent_TickComponent, (void*)&H_WeaponTick, (void**)&o_WeaponTick);
    hooks::Install("UWeaponComponent::StartFire", es2rva::UWeaponComponent_StartFire, (void*)&H_StartFire, (void**)&o_StartFire);
    hooks::Install("UWeaponComponent::StopFire", es2rva::UWeaponComponent_StopFire, (void*)&H_StopFire, (void**)&o_StopFire);
    hooks::Install("AESPlayerController::InputStartFirePrimary", es2rva::AESPlayerController_InputStartFirePrimary, (void*)&H_StartFirePrimary, (void**)&o_StartFirePrimary);
    hooks::Install("AESPlayerController::InputStopFirePrimary", es2rva::AESPlayerController_InputStopFirePrimary, (void*)&H_StopFirePrimary, (void**)&o_StopFirePrimary);
    hooks::Install("AESPlayerController::InputStartFireSecondary", es2rva::AESPlayerController_InputStartFireSecondary, (void*)&H_StartFireSecondary, (void**)&o_StartFireSecondary);
    hooks::Install("AESPlayerController::InputStopFireSecondary", es2rva::AESPlayerController_InputStopFireSecondary, (void*)&H_StopFireSecondary, (void**)&o_StopFireSecondary);
}
}
