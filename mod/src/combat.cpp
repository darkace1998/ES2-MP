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
static float g_healthHz = 4.f;

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

// Health ratio of a pawn's Health component (0..1), or -1 if unavailable.
static float GetHealthRatio(AActor* pawn) {
    UObject* h = FindComponentOfClass(pawn, "HealthComponent");
    if (!h) return -1.f;
    return UE_FIELD(float, h, es2off::UHealthComponent::HitpointRatio);
}
static void SetHealthRatio(AActor* pawn, float v) {
    UObject* h = FindComponentOfClass(pawn, "HealthComponent");
    if (h) UE_FIELD(float, h, es2off::UHealthComponent::HitpointRatio) = v;
}
static float GetShieldRatio(AActor* pawn) {
    UObject* s = FindComponentOfClass(pawn, "ShieldComponent");
    if (!s) return -1.f;
    return UE_FIELD(float, s, es2off::UHealthComponent::HitpointRatio);
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
void SetClientDamageBlock(bool on) {
    if (g_damageBlockOn == on) return;
    g_damageBlockOn = on;
    hooks::Enable("UGameplayLib::ApplyESPointDamage", on);
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

// ---------------------------------------------------------------- messages
bool OnServerOp(APlayerController* from, const std::string& op, const std::string& body) {
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
        // HP|<playerId>|<healthRatio>|<shieldRatio>  — informational for the partner's HUD/logging
        int id = 0; float hp = 0, sh = 0;
        if (sscanf(body.c_str(), "%d|%f|%f", &id, &hp, &sh) != 3) return true;
        players::Player* p = players::ById(id);
        if (p && !p->local && p->pawn) SetHealthRatio(p->pawn, hp);
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
        float hp = GetHealthRatio(p->pawn), sh = GetShieldRatio(p->pawn);
        if (hp < 0) continue;
        coop::SendToAllClients(Format("HP|%d|%.3f|%.3f", p->id, hp, sh < 0 ? 0.f : sh));
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

static void CmdCombat(const console::Args& a, std::string& out) {
    if (a.size() > 2 && a[1] == "route") g_routeFire = a[2] == "1";
    if (a.size() > 2 && a[1] == "localfire") g_localFire = a[2] == "1";
    if (a.size() > 2 && a[1] == "hphz") g_healthHz = (float)atof(a[2].c_str());
    if (a.size() > 2 && a[1] == "npcfire") g_mirrorNpcFire = a[2] == "1";
    out += Format("npcFireMirror=%d sent=%llu applied=%llu unresolved=%llu deduped=%llu\n", (int)g_mirrorNpcFire,
                  (unsigned long long)g_npcFireSent, (unsigned long long)g_npcFireApplied,
                  (unsigned long long)g_npcFireUnresolved, (unsigned long long)g_npcFireDeduped);
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
    console::Register("combat", "combat [route 0/1|localfire 0/1|hphz N] - fire-routing status and player health", CmdCombat);
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
    hooks::Install("UWeaponComponent::StartFire", es2rva::UWeaponComponent_StartFire, (void*)&H_StartFire, (void**)&o_StartFire);
    hooks::Install("UWeaponComponent::StopFire", es2rva::UWeaponComponent_StopFire, (void*)&H_StopFire, (void**)&o_StopFire);
    hooks::Install("AESPlayerController::InputStartFirePrimary", es2rva::AESPlayerController_InputStartFirePrimary, (void*)&H_StartFirePrimary, (void**)&o_StartFirePrimary);
    hooks::Install("AESPlayerController::InputStopFirePrimary", es2rva::AESPlayerController_InputStopFirePrimary, (void*)&H_StopFirePrimary, (void**)&o_StopFirePrimary);
    hooks::Install("AESPlayerController::InputStartFireSecondary", es2rva::AESPlayerController_InputStartFireSecondary, (void*)&H_StartFireSecondary, (void**)&o_StartFireSecondary);
    hooks::Install("AESPlayerController::InputStopFireSecondary", es2rva::AESPlayerController_InputStopFireSecondary, (void*)&H_StopFireSecondary, (void**)&o_StopFireSecondary);
}
}
