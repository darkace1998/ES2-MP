// Co-op location travel.
//
// Two things in ES2's jump are single-player-only:
//
// 1. World origin shifting. AESGameModeBase (server-only) periodically rebases the world origin around
//    the HOST's pawn (UESGameInstance::WorldOriginShiftingStack is a refcount; CheckForWorldOriginShifting
//    calls UGameplayStatics::SetWorldOriginLocation). A client never runs that code, so every absolute
//    coordinate drifts apart between the two machines. In multiplayer we turn it off.
//
// 2. The jump itself. UGameplayLib::ESOpenLevel ends in UGameplayStatics::OpenLevel -> UEngine::SetClientTravel,
//    a *client* travel: it tears the world down without telling anyone, so connected clients are dropped,
//    and because it travels absolute the host's FURL loses the "Listen" option and the host stops listening.
//    On a listen server we redirect that to UWorld::ServerTravel(url, bAbsolute=false, ...), which runs
//    AGameModeBase::ProcessServerTravel -> ProcessClientTravel -> APlayerController::ClientTravel for every
//    remote player, so everyone travels together, and travelling relative keeps "Listen" + port from LastURL.
#include "travel.h"
#include "coop.h"
#include "players.h"
#include "console.h"
#include "log.h"
#include "hooks.h"
#include <windows.h>
#include <cstdlib>

using namespace ue;
using es2coop::Format;

namespace travel {

// NOTE ON ABI: both OpenLevel and ESOpenLevel take `FString Options` BY VALUE. FString is
// non-trivially-copyable, so the MS x64 ABI passes it INDIRECTLY (a pointer) and the CALLER owns and
// destroys it. Declaring the parameter as a by-value FString in the detour made us construct/destroy a
// second owner of the same buffer and crashed the game on the first level load. Treat it as an opaque
// pointer we simply forward, and pass a pointer to an FString we own when we call these ourselves.
using Fn_OpenLevel = void (*)(const UObject* wco, FName levelName, bool bAbsolute, const void* options);
using Fn_ServerTravel = bool (*)(UWorld*, const FString*, bool bAbsolute, bool bSkipGameNotify);
using Fn_ESOpenLevel = void (*)(const UObject* wco, FName levelName, bool bAbsolute, const FString* options);
// The correct way to change location: ChangeLocation_Internal writes PlayerData->CurrentLocation, saves the
// old location's state, refreshes ship data and mission state, and only THEN calls ESOpenLevel. Calling
// ESOpenLevel on its own loads a map the player data still thinks it is not in, and the travel URL build
// faults. All three of these return/take FLocationData by const& == a plain pointer.
using Fn_GetCurrentLocationData = const void* (*)(const UObject* wco);
using Fn_GetLocationData = const void* (*)(const FName* id);
using Fn_ChangeLocation = void (*)(UObject* wco, const void* oldLoc, const void* newLoc, bool bDontClearDockingPoint, bool bDontAutoSave);

using Fn_EnableListenServer = bool (*)(UGameInstance*, bool, int);

static Fn_OpenLevel o_OpenLevel = nullptr;
static bool g_coopTravel = true;
static bool g_disableOriginShift = true;
static uint64_t g_redirects = 0;
static std::string g_lastUrl;

// Travel model. ES2's own jump is a client travel (OpenLevel -> SetClientTravel): it tears the world down
// without telling clients, and travelling absolute drops the "Listen" option so the host stops listening.
// UE's ServerTravel would carry clients along, but ES2 has never exercised non-seamless server travel and
// the host crashes mid-transition. So we use the paths the game does exercise: the host jumps normally and
// re-arms its listen server on the far side, and every client reconnects to it once it is back up.
//   "reconnect"    - default, robust
//   "servertravel" - experimental UWorld::ServerTravel redirect
static std::string g_mode = "reconnect";
static int g_listenPort = 7777;
static bool g_wasHosting = false;         // host: we jumped while hosting, re-listen when the map is up
static double g_now = 0;
static double g_relistenAt = 0;
static std::string g_hostAddr;            // client: where to reconnect after the host jumps
static double g_reconnectAt = 0;
static int g_reconnectTries = 0;
// Docking is a LEVEL CHANGE, not an animation: ES2 opens a station/cinematic map. On a client that is a
// local client-travel, so the player simply drops out of the session for as long as they are docked --
// the host carries on without them (verified: host stayed in its location, clientConnections went to 0).
// That is the behaviour we want for co-op ("solo docking"), and all it needs is a way back: remember
// that we left under our own steam and rejoin once we are out of the station again.
static bool g_leftSession = false;        // client -> standalone seen; still waiting to see where we land
static bool g_dockedAway = false;
static std::string g_dockedMap;           // the station/cinematic map we left into
static int g_lastNetMode = -1;
static constexpr double kDockReturnSettle = 10.0;   // let the location finish coming up before connecting
static double g_reconnectDelay = 18.0;   // the host needs ~15s to load the new location and re-listen

// ---------------------------------------------------------------- world origin shifting
void ApplyOriginShiftPolicy(bool multiplayer) {
    UGameInstance* gi = GetGameInstance();
    if (!gi || !g_disableOriginShift) return;
    int& stack = UE_FIELD(int32_t, gi, es2off::UESGameInstance::WorldOriginShiftingStack);
    bool& cfg = UE_FIELD(bool, gi, es2off::UESGameInstance::bUseWorldOriginShifting);
    if (multiplayer) {
        if (stack != 0 || cfg) {
            LOGF("[travel] world origin shifting OFF for multiplayer (stack %d -> 0, cfg %d -> 0)", stack, (int)cfg);
            stack = 0;
            cfg = false;
        }
    }
}

// ---------------------------------------------------------------- travel redirect
static void H_OpenLevel(const UObject* wco, FName levelName, bool bAbsolute, const void* options) {
    UWorld* w = GetWorld();
    int nm = GetNetMode(w);
    if (g_coopTravel && nm == 2 /*ListenServer*/ && players::Count() > 1) {
        std::string lvl = levelName.ToString();
        std::string opt = options ? reinterpret_cast<const FString*>(options)->ToUtf8() : std::string();
        ++g_redirects;
        if (g_mode == "servertravel") {
            std::string url = lvl;
            if (!opt.empty()) url += (opt[0] == '?' ? "" : "?") + opt;   // ES2 already prefixes '?' sometimes
            if (url.find("listen") == std::string::npos) url += "?listen";
            g_lastUrl = url;
            LOGF("[travel] co-op travel: OpenLevel(%s) -> ServerTravel('%s')", lvl.c_str(), url.c_str());
            coop::SendToAllClients("TRAVEL|" + lvl);
            FString f(url);
            bool ok = Rva<std::remove_pointer_t<Fn_ServerTravel>>(es2rva::UWorld_ServerTravel)(w, &f, false, false);
            LOGF("[travel] ServerTravel -> %d", (int)ok);
            if (ok) return;
            LOGF("[travel] ServerTravel refused, falling back to vanilla OpenLevel");
        } else {
            // Tell the clients where we are going and that they should come back to us afterwards, then
            // perform ES2's own jump untouched.
            g_lastUrl = lvl;
            g_wasHosting = true;
            LOGF("[travel] co-op travel (reconnect model): host jumping to %s, clients will reconnect", lvl.c_str());
            coop::SendToAllClients("TRAVEL|" + lvl);
        }
    }
    o_OpenLevel(wco, levelName, bAbsolute, options);
}

bool OnClientOp(const std::string& op, const std::string& body) {
    if (op != "TRAVEL") return false;
    g_reconnectAt = g_now + g_reconnectDelay;
    g_reconnectTries = 0;
    LOGF("[travel] host is jumping to %s; will reconnect to %s in %.0fs", body.c_str(),
         g_hostAddr.empty() ? "(unknown host)" : g_hostAddr.c_str(), g_reconnectDelay);
    return true;
}

void SetHostAddress(const std::string& addr) { g_hostAddr = addr; LOGF("[travel] host address remembered: %s", addr.c_str()); }
void SetListenPort(int port) { g_listenPort = port; }
int  ListenPort() { return g_listenPort; }

// Driven from the coop tick on both sides.
void Tick(float dt, bool isHost) {
    g_now += dt;
    UWorld* w = GetWorld();
    int nm = GetNetMode(w);
    std::string world = WorldName(w);
    bool inGameplayMap = !world.empty() && world != "EntryMap" && world != "EmptyTransitionMap" && world != "Map_MainMenu";

    if (g_wasHosting && nm == 0 /*Standalone*/ && inGameplayMap) {
        // the jump finished and the Listen option was lost with the absolute travel: host again
        if (g_relistenAt == 0) { g_relistenAt = g_now + 2.0; return; }
        if (g_now < g_relistenAt) return;
        UGameInstance* gi = GetGameInstance();
        if (gi) {
            bool ok = Rva<std::remove_pointer_t<Fn_EnableListenServer>>(es2rva::UGameInstance_EnableListenServer)(gi, true, g_listenPort);
            LOGF("[travel] re-armed listen server on port %d after jump -> %d (world %s)", g_listenPort, (int)ok, world.c_str());
        }
        g_wasHosting = false;
        g_relistenAt = 0;
        return;
    }

    // ---- solo docking: a client that travelled away on its own finds its way back --------------
    // Client -> Standalone with no host-announced travel pending means we left the session ourselves,
    // which in practice means we docked. Remember the map we landed in so we know when we are out of it.
    // Where we land cannot be read on the frame the net mode drops: a dock travels through
    // EmptyTransitionMap first, so deciding immediately either misses the station (the edge is already
    // spent by the time the real map loads) or latches onto the transition map. Latch the edge, then
    // decide once the world has settled.
    if (g_lastNetMode == 3 && nm == 0 && !g_hostAddr.empty() && g_reconnectAt == 0 && !g_dockedAway)
        g_leftSession = true;
    g_lastNetMode = nm;

    if (g_leftSession) {
        if (nm == 3) {
            g_leftSession = false;                      // back in a session on our own
        } else if (world == "EntryMap" || world == "Map_MainMenu") {
            // Quit to the front end, or ES2 bounced us there. This is NOT a dock, and it must never
            // arm a rejoin: travelling to a host from the main menu crashes the game (ES2's own bug,
            // see docs/INSTALL.md). Observed live — a client that fell back to EntryMap logged
            // "left the session for 'EntryMap' (docked?)" before this check existed.
            LOGF("[travel] left the session for the main menu — not rejoining");
            g_leftSession = false;
        } else if (inGameplayMap) {
            g_leftSession = false;
            g_dockedAway = true;
            g_dockedMap = world;
            LOGF("[travel] docked away into '%s' — will rejoin %s on the way out",
                 world.c_str(), g_hostAddr.c_str());
        }
        // otherwise still in transit (EmptyTransitionMap / no world yet): decide on a later tick
    }
    if (g_dockedAway) {
        if (nm == 3) { g_dockedAway = false; g_dockedMap.clear(); }          // already back in
        else if (nm == 0 && inGameplayMap && world != g_dockedMap) {
            // Arm the ordinary reconnect path rather than opening right here: that loop already knows how
            // to wait for the drop, retry and give up, and a few seconds of settle costs nothing on a
            // level that has just finished streaming in. (An earlier note here blamed an immediate
            // connect for an access violation; that crash was actually caused by testing the return leg
            // with a raw `open`, which bypasses UGameplayLib::ChangeLocation — see docs/NOTES.md.)
            g_dockedAway = false;
            g_dockedMap.clear();
            g_reconnectAt = g_now + kDockReturnSettle;
            g_reconnectTries = 0;
            LOGF("[travel] out of the station and back in %s — rejoining %s in %.0fs",
                 world.c_str(), g_hostAddr.c_str(), kDockReturnSettle);
        }
    }

    if (g_reconnectAt > 0) {
        // We are still attached to the host that is about to tear the world down; wait for the drop.
        // (Checking "am I a client?" before we have even tried once used to cancel the reconnect
        // instantly, because the old connection is still alive in the same tick the host announces.)
        if (nm == 3) {
            if (g_reconnectTries > 0) { LOGF("[travel] reconnected to %s", g_hostAddr.c_str()); g_reconnectAt = 0; g_reconnectTries = 0; }
            return;
        }
        if (g_now < g_reconnectAt) return;
        if (g_hostAddr.empty()) { g_reconnectAt = 0; return; }
        if (++g_reconnectTries > 8) { LOGF("[travel] giving up reconnecting to %s", g_hostAddr.c_str()); g_reconnectAt = 0; g_reconnectTries = 0; return; }
        LOGF("[travel] reconnecting to %s (try %d)", g_hostAddr.c_str(), g_reconnectTries);
        ExecConsoleCommand("open " + g_hostAddr);
        g_reconnectAt = g_now + 12.0;      // retry until it takes
    }
}

// A client cannot start a location change itself (the jump completion handler lives on the game mode,
// which only exists on the server), so it asks the host to do it.
bool OnServerOp(APlayerController* from, const std::string& op, const std::string& body) {
    if (op != "JUMPREQ") return false;
    LOGF("[travel] player requested a jump to %s", body.c_str());
    DoChangeLocation(body);
    return true;
}

// Run ES2's real location change (the same call its jump-completed handler makes).
bool DoChangeLocation(const std::string& locationId) {
    UWorld* w = GetWorld();
    if (!w) return false;
    FName id = FName::Make(locationId);
    const void* newLoc = Rva<std::remove_pointer_t<Fn_GetLocationData>>(es2rva::UMapLib_GetLocationData)(&id);
    const void* oldLoc = Rva<std::remove_pointer_t<Fn_GetCurrentLocationData>>(es2rva::UMapLib_GetCurrentLocationData)((const UObject*)w);
    if (!newLoc) { LOGF("[travel] unknown location %s", locationId.c_str()); return false; }
    std::string newId = UE_FIELDC(FName, newLoc, es2off::FLocationData::LocationID).ToString();
    if (newId != locationId) { LOGF("[travel] location %s not found (got '%s')", locationId.c_str(), newId.c_str()); return false; }
    LOGF("[travel] ChangeLocation %s -> %s",
         oldLoc ? UE_FIELDC(FName, oldLoc, es2off::FLocationData::LocationID).ToString().c_str() : "?", newId.c_str());
    Rva<std::remove_pointer_t<Fn_ChangeLocation>>(es2rva::UGameplayLib_ChangeLocation)((UObject*)w, oldLoc, newLoc, false, false);
    return true;
}

// ---------------------------------------------------------------- location table
using Fn_GetGameData = UObject* (*)();
static void CmdLocations(const console::Args& a, std::string& out) {
    UObject* gd = Rva<std::remove_pointer_t<Fn_GetGameData>>(es2rva::UGameData_GetSingleton)();
    if (!gd) { out = "no UGameData\n"; return; }
    struct RawArray { char* Data; int32_t Num; int32_t Max; };
    RawArray& locs = UE_FIELD(RawArray, gd, es2off::UGameData::Locations);
    std::string filter = a.size() > 1 ? a[1] : "";
    out += Format("locations: %d\n", locs.Num);
    int shown = 0;
    for (int i = 0; i < locs.Num && shown < 60; ++i) {
        char* ld = locs.Data + (size_t)i * es2off::FLocationData::__size;
        std::string id = UE_FIELD(FName, ld, es2off::FLocationData::LocationID).ToString();
        std::string sys = UE_FIELD(FName, ld, es2off::FLocationData::SystemID).ToString();
        if (!filter.empty() && id.find(filter) == std::string::npos && sys.find(filter) == std::string::npos) continue;
        out += Format("  %-14s system=%-10s%s\n", id.c_str(), sys.c_str(),
                      UE_FIELD(bool, ld, es2off::FLocationData::bNeverShowIngame) ? " (hidden)" : "");
        ++shown;
    }
}

// ---------------------------------------------------------------- commands
static void CmdTravelTo(const console::Args& a, std::string& out) {
    if (a.size() < 2) { out = "usage: goto <LocationID>   (host: co-op travel; client: ask the host)\n"; return; }
    if (coop::CurrentRole() == coop::Role::Client) {
        coop::SendToServer("JUMPREQ|" + a[1]);
        out += "asked the host to travel to " + a[1] + "\n";
        return;
    }
    out += DoChangeLocation(a[1]) ? ("ChangeLocation(" + a[1] + ") issued\n") : ("could not change location to " + a[1] + "\n");
}

static void CmdTravelInfo(const console::Args& a, std::string& out) {
    if (a.size() > 2 && a[1] == "coop") g_coopTravel = a[2] == "1";
    if (a.size() > 2 && a[1] == "mode") g_mode = a[2];
    if (a.size() > 2 && a[1] == "host") SetHostAddress(a[2]);
    if (a.size() > 2 && a[1] == "originshift") { g_disableOriginShift = a[2] == "1"; ApplyOriginShiftPolicy(coop::CurrentRole() != coop::Role::None); }
    UGameInstance* gi = GetGameInstance();
    out += Format("coopTravel=%d mode=%s disableOriginShift=%d redirects=%llu lastUrl=%s\n",
                  (int)g_coopTravel, g_mode.c_str(), (int)g_disableOriginShift, (unsigned long long)g_redirects, g_lastUrl.c_str());
    out += Format("hostAddr=%s wasHosting=%d reconnectIn=%.1fs\n", g_hostAddr.c_str(), (int)g_wasHosting,
                  g_reconnectAt > 0 ? g_reconnectAt - g_now : 0.0);
    out += Format("leftSession=%d dockedAway=%d dockedMap=%s\n", (int)g_leftSession, (int)g_dockedAway, g_dockedMap.empty() ? "-" : g_dockedMap.c_str());
    if (gi) out += Format("worldOriginShiftingStack=%d bUseWorldOriginShifting=%d\n",
                          UE_FIELD(int32_t, gi, es2off::UESGameInstance::WorldOriginShiftingStack),
                          (int)UE_FIELD(bool, gi, es2off::UESGameInstance::bUseWorldOriginShifting));
    out += Format("world=%s netmode=%s\n", WorldName(GetWorld()).c_str(), NetModeName(GetNetMode(GetWorld())));
}

void Register() {
    console::Register("goto", "goto <LocationID> - travel to a location (host travels everyone; client asks the host)", CmdTravelTo);
    console::Register("locations", "locations [filter] - list location IDs you can 'goto'", CmdLocations);
    console::Register("travelinfo", "travelinfo [coop 0/1|originshift 0/1] - co-op travel + world origin state", CmdTravelInfo);
}
void OnInit() {
    hooks::Install("UGameplayStatics::OpenLevel", es2rva::UGameplayStatics_OpenLevel, (void*)&H_OpenLevel, (void**)&o_OpenLevel);
}
}
