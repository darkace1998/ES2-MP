// Steam P2P transport: diagnostics and the one gate that blocks it.
//
// The mod can already select the Steam net driver ('netdriver steam') and a client can 'connect
// steam.<SteamID64>:7777' — the URL resolution path is fully mapped. But an incoming P2P connection is
// dropped before it ever reaches the accept logic: FOnlineAsyncTaskManagerSteam::OnP2PSessionRequest
// asks the session interface for GetNumSessions() and silently discards the request when it is <= 0.
// ES2 never creates a Steam session, so a Steam-hosting mod would listen forever and never accept a
// peer, with nothing in any log to say why. Reporting 1 session while we are hosting opens that gate.
//
// Everything else here is read-only self-diagnosis, so the Steam path can be checked on one machine
// with one Steam account, right up to the point where a second player is genuinely required.
#include "steamp2p.h"
#include "coop.h"
#include "console.h"
#include "log.h"
#include "hooks.h"
#include <windows.h>
#include <cstdlib>
#include <string>

using namespace ue;
using es2coop::Format;

namespace steamp2p {

using Fn_GetNumSessions = int (*)(void* self);
using Fn_GetUniquePlayerId = void* (*)(const void* self, void* sretSharedPtr, int localUserNum);

static Fn_GetNumSessions o_GetNumSessions = nullptr;
static bool g_hosting = false;          // are we advertising ourselves as a Steam host?
static uint64_t g_gateOpened = 0;

// TSharedPtr<FOnlineSubsystemSteam> -> the object pointer is the first 8 bytes.
static void* SteamSubsystem() {
    void** sp = Rva<void*>(es2rva::FOnlineFactorySteam_SteamSingleton);
    return sp ? *sp : nullptr;
}
static void* SteamSockets() {
    void** sp = Rva<void*>(es2rva::FSocketSubsystemSteam_SocketSingleton);
    return sp ? *sp : nullptr;
}
static void* SessionInterface() {
    void* s = SteamSubsystem();
    return s ? *reinterpret_cast<void**>(reinterpret_cast<char*>(s) + es2off::FOnlineSubsystemSteam::SessionInterface) : nullptr;
}
static void* IdentityInterface() {
    void* s = SteamSubsystem();
    return s ? *reinterpret_cast<void**>(reinterpret_cast<char*>(s) + es2off::FOnlineSubsystemSteam::IdentityInterface) : nullptr;
}

// The accept gate: report at least one session while the mod is hosting over Steam.
static int H_GetNumSessions(void* self) {
    int r = o_GetNumSessions(self);
    if (g_hosting && r <= 0) {
        ++g_gateOpened;
        return 1;
    }
    return r;
}

// This machine's own SteamID64 — the address a partner would connect to.
static uint64_t LocalSteamId() {
    void* ident = IdentityInterface();
    if (!ident) return 0;
    // TSharedPtr<const FUniqueNetId> out; member fn returning a large struct: (this=RCX, sret=RDX, args...)
    void* out[2] = {nullptr, nullptr};
    Rva<std::remove_pointer_t<Fn_GetUniquePlayerId>>(es2rva::FOnlineIdentitySteam_GetUniquePlayerId)(ident, out, 0);
    if (!out[0]) return 0;
    return *reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(out[0]) + 0x18);   // FUniqueNetIdSteam::UniqueNetId
}

void SetHosting(bool on) {
    if (g_hosting == on) return;
    g_hosting = on;
    LOGF("[steam] hosting mode %s (P2P accept gate %s)", on ? "ON" : "off", on ? "open" : "closed");
}

static void CmdSteam(const console::Args& a, std::string& out) {
    if (a.size() > 2 && a[1] == "host") SetHosting(a[2] == "1");

    void* sub = SteamSubsystem();
    void* sock = SteamSockets();
    out += "--- Steam transport self-check ---\n";
    out += Format("1. FOnlineSubsystemSteam           : %s\n", sub ? "alive" : "NULL (Steam transport unavailable; stay on IpNetDriver)");
    if (sub) {
        bool init = UE_FIELD(bool, sub, es2off::FOnlineSubsystemSteam::bSteamworksClientInitialized);
        out += Format("   bSteamworksClientInitialized   : %s\n", init ? "true (SteamAPI_Init succeeded)" : "FALSE (no Steam client / not launched via Steam)");
        out += Format("   session interface              : %p\n", SessionInterface());
        out += Format("   identity interface             : %p\n", IdentityInterface());
    }
    out += Format("2. FSocketSubsystemSteam singleton : %s\n", sock ? "registered" : "NULL (CreateSteamSocketSubsystem did not run)");
    uint64_t id = LocalSteamId();
    if (id) out += Format("3. this machine's SteamID64        : %llu\n   join address                   : steam.%llu:7777\n",
                          (unsigned long long)id, (unsigned long long)id);
    else    out += "3. this machine's SteamID64        : (unavailable)\n";

    UWorld* w = GetWorld();
    UNetDriver* nd = GetNetDriver(w);
    std::string ndClass = nd ? ue::GetObjectClassName((UObject*)nd) : std::string("(none)");
    out += Format("4. active net driver               : %s\n", ndClass.c_str());
    if (nd && ndClass == "SteamNetDriver") {
        // USteamNetDriver::bIsPassthrough at +0x980: 1 means it silently degraded to plain UDP
        bool passthrough = UE_FIELD(uint8_t, nd, 0x980) != 0;
        out += Format("   transport                      : %s\n", passthrough ? "PASSTHROUGH (plain UDP, not Steam P2P)" : "real Steam P2P");
    }
    out += Format("5. P2P accept gate                 : hosting=%d, forced-open %llu time(s)\n",
                  (int)g_hosting, (unsigned long long)g_gateOpened);
    out += "\nTo host over Steam:  netdriver steam ; steam host 1 ; listen 7777\n";
    out += "A partner then runs: connect steam.<your SteamID64>:7777   (you must be Steam friends)\n";
}

void Register() {
    console::Register("steam", "steam [host 0/1] - Steam P2P transport self-check and accept gate", CmdSteam);
}
void OnInit() {
    hooks::Install("FOnlineSessionSteam::GetNumSessions", es2rva::FOnlineSessionSteam_GetNumSessions,
                   (void*)&H_GetNumSessions, (void**)&o_GetNumSessions);
}
}
