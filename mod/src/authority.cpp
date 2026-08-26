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
#include <cstdlib>

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
    // NOT PickupBase: ES2's pickups never replicate, so loot.cpp spawns each client's own copy of every
    // drop the host announces -- a local, non-remote-owned spawn this guard would have blocked.
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

// ---------------------------------------------------------------- remote-player UI census
//
// A listen server builds a full ES2 UI for EVERY player controller it owns, including the remote ones it
// will never draw: one client join adds ~500k widget objects to the host (measured: Image +75k,
// OverlaySlot +70k, HorizontalBoxSlot +56k, and thousands of WG_Inventory_Slot_C / WG_ItemAttribute_C /
// WG_Placement_Hint_C), and a forced GC frees none of it -- they stay referenced. Two joins put the host
// over UE's 2,162,688 UObject ceiling and it dies with "Maximum number of UObjects exceeded".
//
// UWidgetBlueprintLibrary::Create is the Blueprint "Create Widget" node and takes the owning controller,
// so it can tell a remote player's UI from the host's own. ABI (verified at 0x16AF708): rcx = world
// context, rdx = POINTER to the TSubclassOf (the callee does `mov rbp, [rdx]`), r8 = APlayerController*.
using Fn_WBLCreate = UObject* (*)(UObject* worldCtx, const void* widgetClass, void* pc);
static Fn_WBLCreate o_WBLCreate = nullptr;
using Fn_SpawnPCCommon = void* (*)(void* gm, int role, const void* loc, const void* rot, void* pcClass);
static Fn_SpawnPCCommon o_SpawnPCCommon = nullptr;
static bool g_uiWatch = false;
static std::map<std::string, int> g_uiCounts;
static int g_uiLocal = 0, g_uiRemote = 0, g_uiNoPC = 0;
static std::map<uint64_t, int> g_uiByFrame;   // GFrameCounter -> creations, to see if a burst is one frame
static int g_uiTrace = 0;                    // capture a backtrace for the next N creations
static bool g_uiBlockRemote = true;          // the fix: no UI for a remote player's controller
static bool g_uiSuppress = false;            // set only while a remote PC's BeginPlay runs
static uint64_t g_uiBlocked = 0;

static UObject* H_WBLCreate(UObject* worldCtx, const void* widgetClass, void* pc) {
    if (g_uiWatch) {
        UClass* cls = widgetClass ? *(UClass**)widgetClass : nullptr;
        ++g_uiCounts[cls ? GetName((UObject*)cls) : "(null class)"];
        if (!pc) ++g_uiNoPC;
        else if (UE_FIELD(void*, pc, es2off::APlayerController::NetConnection)) ++g_uiRemote;
        else ++g_uiLocal;
        ++g_uiByFrame[*Rva<uint64_t>(es2rva::GFrameCounter)];
        if (g_uiTrace > 0) {
            --g_uiTrace;
            void* fr[16] = {};
            USHORT n = RtlCaptureStackBackTrace(1, 16, fr, nullptr);
            std::string line = Format("[uiwatch] create %s stack:", cls ? GetName((UObject*)cls).c_str() : "?");
            for (USHORT i = 0; i < n; ++i) {
                uintptr_t a = reinterpret_cast<uintptr_t>(fr[i]);
                if (a > g_base && a - g_base < 0xB000000) line += Format(" +%llX", (unsigned long long)(a - g_base));
            }
            LOGF("%s", line.c_str());
        }
    }
    if (g_uiSuppress) {
        ++g_uiBlocked;
        return nullptr;                       // the joining player's menu is never drawn on this machine
    }
    return o_WBLCreate(worldCtx, widgetClass, pc);
}

// A listen server spawns a PlayerController for every joining client, and ES2's BP_PlayerController
// BeginPlay graph builds the whole in-game menu (WG_Menu_Ingame_C plus its map / inventory / perk tabs)
// with no IsLocalController check -- there was never a second player to check for. That is ~1700 widgets,
// ~500k UObjects, per join, and they stay referenced: a forced GC frees none of them. Two joins put the
// host over UE's 2,162,688 object ceiling and it dies with "Maximum number of UObjects exceeded".
//
// SpawnPlayerControllerCommon is the funnel (both SpawnPlayerController overloads reach it) and it calls
// FinishSpawningActor itself, so BeginPlay -- and the whole widget burst -- happens inside this one call.
// Its ENetRole argument is the signal: ROLE_SimulatedProxy(1) is the listen server's own local player,
// ROLE_AutonomousProxy(2) is a remote joiner. The controller's NetConnection is NOT usable here; UE only
// assigns it after Login returns, so at BeginPlay time it is still null for everyone.
static void* H_SpawnPCCommon(void* gm, int role, const void* loc, const void* rot, void* pcClass) {
    const bool remote = g_uiBlockRemote && role == 2 /*ROLE_AutonomousProxy*/
                        && GetNetMode(GetWorld()) == 2 /*ListenServer*/;
    if (!remote) {
        LOGF("[uiwatch] spawning a player controller (role %d) -- keeping its UI", role);
        return o_SpawnPCCommon(gm, role, loc, rot, pcClass);
    }
    const uint64_t before = g_uiBlocked;
    g_uiSuppress = true;
    void* pc = o_SpawnPCCommon(gm, role, loc, rot, pcClass);
    g_uiSuppress = false;
    LOGF("[uiwatch] remote player controller %s (role %d): suppressed %llu menu widget(s) on the host",
         GetName((UObject*)pc).c_str(), role, (unsigned long long)(g_uiBlocked - before));
    return pc;
}

static void CmdUiWatch(const console::Args& a, std::string& out) {
    if (a.size() > 1 && (a[1] == "on" || a[1] == "off")) g_uiWatch = a[1] == "on";
    if (a.size() > 2 && a[1] == "block") g_uiBlockRemote = a[2] == "1";
    if (a.size() > 2 && a[1] == "trace") g_uiTrace = atoi(a[2].c_str());
    if (a.size() > 1 && a[1] == "clear") { g_uiCounts.clear(); g_uiByFrame.clear(); g_uiLocal = g_uiRemote = g_uiNoPC = 0; }
    out += Format("uiwatch=%d blockRemoteUI=%d (blocked %llu)  createdFor: local=%d remote=%d noController=%d  (%zu classes)\n",
                  (int)g_uiWatch, (int)g_uiBlockRemote, (unsigned long long)g_uiBlocked,
                  g_uiLocal, g_uiRemote, g_uiNoPC, g_uiCounts.size());
    std::vector<std::pair<std::string, int>> v(g_uiCounts.begin(), g_uiCounts.end());
    std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
    int n = a.size() > 2 ? atoi(a[2].c_str()) : 15;
    for (int i = 0; i < (int)v.size() && i < n; ++i) out += Format("  %6d  %s\n", v[i].second, v[i].first.c_str());
    std::vector<std::pair<uint64_t, int>> f(g_uiByFrame.begin(), g_uiByFrame.end());
    std::sort(f.begin(), f.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
    out += Format("busiest frames (%zu frames created widgets):\n", f.size());
    for (int i = 0; i < (int)f.size() && i < 6; ++i) out += Format("  frame %llu: %d\n", (unsigned long long)f[i].first, f[i].second);
}

void Register() {
    console::Register("spawnlog", "spawnlog [on|off|dump|clear] - census of client-side actor spawns (replicated vs local)", CmdSpawnLog);
    console::Register("guard", "guard [on|off|verbose 0/1|add <Class>|del <Class>] - block client-side gameplay spawns", CmdGuard);
    console::Register("uiwatch", "uiwatch [on|off|block 0/1|clear|trace N] [topN] - census of Blueprint-created widgets, by owning controller", CmdUiWatch);
}
void OnInit() {
    hooks::Install("UWorld::SpawnActor(Transform)", es2rva::UWorld_SpawnActor_Transform, (void*)&H_SpawnActor, (void**)&o_SpawnActor);
    hooks::Install("AESHUD::Tick", es2rva::AESHUD_Tick, (void*)&H_HudTick, (void**)&o_HudTick);
    hooks::Install("UWidgetBlueprintLibrary::Create", es2rva::UWidgetBlueprintLibrary_Create,
                   (void*)&H_WBLCreate, (void**)&o_WBLCreate);
    hooks::Install("AGameModeBase::SpawnPlayerControllerCommon", es2rva::AGameModeBase_SpawnPlayerControllerCommon,
                   (void*)&H_SpawnPCCommon, (void**)&o_SpawnPCCommon);
}
bool GuardEnabled() { return g_guard; }
}
