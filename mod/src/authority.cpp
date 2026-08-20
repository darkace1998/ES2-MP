// Host authority enforcement on the client side:
//   * instrument every actor spawn (which class, and whether it came from replication)
//   * optionally block the client's own gameplay code from spawning NPCs/projectiles,
//     so only the host's replicated actors exist.
//
// UWorld::SpawnActor(UClass*, const FTransform*, const FActorSpawnParameters&) is THE funnel:
// the (vector,rotator) overload and SpawnActorAbsolute both forward into it (verified by disassembly).
// Replication-driven spawns (UActorChannel) set FActorSpawnParameters::bRemoteOwned; gameplay spawns do not.
#include "authority.h"
#include "console.h"
#include "ue.h"
#include "log.h"
#include "hooks.h"
#include <windows.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

using namespace ue;
using es2coop::Format;

namespace authority {

using Fn_SpawnActor_T = AActor* (*)(UWorld*, UClass*, const FTransform*, const void* params);
static Fn_SpawnActor_T o_SpawnActor = nullptr;

// AESHUD::Tick dereferences the local player pawn without a null check. In multiplayer the pawn is
// briefly absent (respawn with a different ship, death, travel), which crashed the client. Skip the
// tick during those gaps instead of letting it fault.
using Fn_HudTick = void (*)(void* hud, float dt);
static Fn_HudTick o_HudTick = nullptr;
static uint64_t g_hudSkipped = 0;
static void H_HudTick(void* hud, float dt) {
    UWorld* w = GetWorld();
    if (w) {
        APlayerController* pc = GetFirstLocalPlayerController(w);
        if (!pc || UE_FIELD(void*, pc, es2off::AController::Pawn) == nullptr) {
            if (++g_hudSkipped % 120 == 1) LOGF("[authority] HUD tick skipped (no local pawn) x%llu", (unsigned long long)g_hudSkipped);
            return;
        }
    }
    o_HudTick(hud, dt);
}

static bool g_log = false;         // record a per-class spawn census
static bool g_guard = false;       // actually block client-side gameplay spawns
static bool g_guardVerbose = false;

struct Stat { uint64_t remote = 0, local = 0, blocked = 0; };
static std::map<std::string, Stat> g_stats;
static std::mutex g_statsMutex;

// class-name -> resolved once (FName::ToString allocates; the hook is hot)
static std::map<UClass*, std::string> g_nameCache;
static const std::string& ClassName(UClass* c) {
    auto it = g_nameCache.find(c);
    if (it != g_nameCache.end()) return it->second;
    return g_nameCache.emplace(c, GetName((UObject*)c)).first->second;
}

// Classes whose client-side gameplay spawns are suppressed when the guard is on.
// Matched against the class and all of its parents, so Blueprint children are covered.
static std::vector<std::string> g_blockBases = {
    "ESPawn",          // every ship: NPCs, drones, turrets  (the local player ship arrives via replication)
    "ProjectileBase",  // host-authoritative projectiles
    "PickupBase",      // loot is host-owned; the client gets it replicated
    "POISpawner",
    "SpawnComposition",
    "MissionBase",
    "MissionTaskBase",
    "MapEventManager",
    "WantedLevelManager",
    "BattleSimulator",
};
static std::vector<UClass*> g_blockClasses;    // resolved lazily from g_blockBases
static bool g_blockResolved = false;

static void ResolveBlockClasses() {
    g_blockClasses.clear();
    for (auto& n : g_blockBases) {
        UClass* c = FindClass(n);
        if (c) g_blockClasses.push_back(c);
        else LOGF("[authority] block base '%s' not found (yet)", n.c_str());
    }
    g_blockResolved = !g_blockClasses.empty();
}

static bool ShouldBlock(UClass* cls) {
    if (!g_blockResolved) ResolveBlockClasses();
    for (UClass* b : g_blockClasses) if (IsChildOf((const UStruct*)cls, (const UStruct*)b)) return true;
    return false;
}

static AActor* H_SpawnActor(UWorld* world, UClass* cls, const FTransform* t, const void* params) {
    // Fast path: only clients are policed, and only when something is enabled.
    if ((g_log || g_guard) && cls && world) {
        int nm = GetNetMode(world);
        if (nm == 3 /*NM_Client*/) {
            bool remoteOwned = params && (UE_FIELDC(uint8_t, params, es2off::FActorSpawnParameters::bRemoteOwned_off)
                                          & es2off::FActorSpawnParameters::bRemoteOwned_mask) != 0;
            bool block = false;
            if (g_guard && !remoteOwned) block = ShouldBlock(cls);
            if (g_log || block) {
                const std::string& n = ClassName(cls);
                std::lock_guard<std::mutex> lk(g_statsMutex);
                Stat& s = g_stats[n];
                if (block) ++s.blocked; else if (remoteOwned) ++s.remote; else ++s.local;
                if (block && g_guardVerbose && s.blocked < 5) LOGF("[authority] blocked client spawn of %s", n.c_str());
            }
            if (block) return nullptr;
        }
    }
    return o_SpawnActor(world, cls, t, params);
}

// ---------------------------------------------------------------- commands
static void CmdSpawnLog(const console::Args& a, std::string& out) {
    std::string sub = a.size() > 1 ? a[1] : "dump";
    if (sub == "on") { g_log = true; out += "spawn census ON\n"; }
    else if (sub == "off") { g_log = false; out += "spawn census OFF\n"; }
    else if (sub == "clear") { std::lock_guard<std::mutex> lk(g_statsMutex); g_stats.clear(); out += "cleared\n"; }
    if (sub == "dump" || sub == "on" || sub == "clear") {
        std::lock_guard<std::mutex> lk(g_statsMutex);
        out += Format("%-52s %8s %8s %8s\n", "class", "replic", "local", "blocked");
        std::vector<std::pair<std::string, Stat>> rows(g_stats.begin(), g_stats.end());
        std::sort(rows.begin(), rows.end(), [](auto& x, auto& y) { return (x.second.local + x.second.blocked) > (y.second.local + y.second.blocked); });
        for (auto& [n, s] : rows) out += Format("%-52s %8llu %8llu %8llu\n", n.c_str(), (unsigned long long)s.remote, (unsigned long long)s.local, (unsigned long long)s.blocked);
        out += Format("(%d distinct classes; log=%d guard=%d)\n", (int)rows.size(), (int)g_log, (int)g_guard);
    }
}

static void CmdGuard(const console::Args& a, std::string& out) {
    if (a.size() > 1) {
        if (a[1] == "on") { g_guard = true; ResolveBlockClasses(); }
        else if (a[1] == "off") g_guard = false;
        else if (a[1] == "verbose") g_guardVerbose = (a.size() > 2 && a[2] == "1");
        else if (a[1] == "add" && a.size() > 2) { g_blockBases.push_back(a[2]); ResolveBlockClasses(); }
        else if (a[1] == "del" && a.size() > 2) { g_blockBases.erase(std::remove(g_blockBases.begin(), g_blockBases.end(), a[2]), g_blockBases.end()); ResolveBlockClasses(); }
    }
    if (!g_blockResolved) ResolveBlockClasses();
    out += Format("guard=%s verbose=%d (client-side gameplay spawns of these bases are blocked)\n", g_guard ? "ON" : "off", (int)g_guardVerbose);
    for (size_t i = 0; i < g_blockBases.size(); ++i) {
        UClass* c = i < g_blockClasses.size() ? g_blockClasses[i] : nullptr;
        out += Format("  %-24s %s\n", g_blockBases[i].c_str(), c ? GetPathName((UObject*)c).c_str() : "(unresolved)");
    }
}

void Register() {
    console::Register("spawnlog", "spawnlog [on|off|dump|clear] - census of client-side actor spawns (replicated vs local)", CmdSpawnLog);
    console::Register("guard", "guard [on|off|verbose 0/1|add <Class>|del <Class>] - block client-side gameplay spawns", CmdGuard);
}
void OnInit() {
    hooks::Install("UWorld::SpawnActor(Transform)", es2rva::UWorld_SpawnActor_Transform, (void*)&H_SpawnActor, (void**)&o_SpawnActor);
    hooks::Install("AESHUD::Tick", es2rva::AESHUD_Tick, (void*)&H_HudTick, (void**)&o_HudTick);
}
bool GuardEnabled() { return g_guard; }
}
