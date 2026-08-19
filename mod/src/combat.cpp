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
    out += Format("routeFire=%d localFire=%d healthHz=%.0f fireSent=%llu fireApplied=%llu\n",
                  (int)g_routeFire, (int)g_localFire, g_healthHz,
                  (unsigned long long)g_fireSent, (unsigned long long)g_fireApplied);
    for (auto* p : players::All()) {
        if (!p->pawn) continue;
        out += Format("  p%d %-22s hp=%.3f shield=%.3f\n", p->id, GetName((UObject*)p->pawn).c_str(),
                      GetHealthRatio(p->pawn), GetShieldRatio(p->pawn));
    }
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
}

void OnInit() {
    hooks::Install("AESPlayerController::InputStartFirePrimary", es2rva::AESPlayerController_InputStartFirePrimary, (void*)&H_StartFirePrimary, (void**)&o_StartFirePrimary);
    hooks::Install("AESPlayerController::InputStopFirePrimary", es2rva::AESPlayerController_InputStopFirePrimary, (void*)&H_StopFirePrimary, (void**)&o_StopFirePrimary);
    hooks::Install("AESPlayerController::InputStartFireSecondary", es2rva::AESPlayerController_InputStartFireSecondary, (void*)&H_StartFireSecondary, (void**)&o_StartFireSecondary);
    hooks::Install("AESPlayerController::InputStopFireSecondary", es2rva::AESPlayerController_InputStopFireSecondary, (void*)&H_StopFireSecondary, (void**)&o_StopFireSecondary);
}
}
