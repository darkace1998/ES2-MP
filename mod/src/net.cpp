// Networking bring-up: listen / connect / net status + diagnostic hooks on the UE login flow.
#include "console.h"
#include "ue.h"
#include "log.h"
#include "hooks.h"
#include "coop.h"
#include "loadout.h"
#include "travel.h"
#include "steamp2p.h"
#include "players.h"
#include <windows.h>
#include <cstdlib>

using namespace ue;
using es2coop::Format;

namespace net {

// ---------------------------------------------------------------- engine fn typedefs
using Fn_EnableListenServer = bool (*)(UGameInstance*, bool, int);
using Fn_ServerTravel = bool (*)(UWorld*, const FString*, bool, bool);
using Fn_LowLevelGetRemoteAddress = void (*)(UNetConnection*, FString* out, bool appendPort);
using Fn_LowLevelDescribe = void (*)(UNetConnection*, FString* out);

// ---------------------------------------------------------------- hooks (diagnostics for now)
using Fn_PostLogin = void (*)(AGameModeBase*, APlayerController*);
using Fn_RestartPlayer = void (*)(AGameModeBase*, void* controller);
using Fn_SpawnDefaultPawnAtTransform = void* (*)(AGameModeBase*, void* controller, const FTransform*);
using Fn_ServerChangeName_Impl = void (*)(APlayerController*, const FString*);
using Fn_ClientMessage_Impl = void (*)(APlayerController*, const FString*, FName, float);
using Fn_SetGameMode = bool (*)(UWorld*, const void* url);
using Fn_Browse = int (*)(UEngine*, void* worldContext, void* url /*FURL by value -> passed by pointer (size>8)*/, FString* error);
using Fn_Listen = bool (*)(UWorld*, void* url);

static Fn_PostLogin o_PostLogin;
static Fn_RestartPlayer o_RestartPlayer;
static Fn_SpawnDefaultPawnAtTransform o_SpawnDefaultPawnAtTransform;
static Fn_ServerChangeName_Impl o_ServerChangeName;
static Fn_ClientMessage_Impl o_ClientMessage;
static Fn_SetGameMode o_SetGameMode;
static Fn_Browse o_Browse;
static Fn_Listen o_Listen;
static std::string UrlToString(const void* url) {
    if (!url) return "null";
    const FString& proto = UE_FIELDC(FString, url, es2off::FURL::Protocol);
    const FString& host = UE_FIELDC(FString, url, es2off::FURL::Host);
    int port = UE_FIELDC(int32_t, url, es2off::FURL::Port);
    const FString& map = UE_FIELDC(FString, url, es2off::FURL::Map);
    return Format("%s://%s:%d/%s", proto.ToUtf8().c_str(), host.ToUtf8().c_str(), port, map.ToUtf8().c_str());
}

using Fn_InitListen = bool (*)(UNetDriver*, void* notify, void* url, bool reuse, FString* err);
using Fn_InitBase = bool (*)(UNetDriver*, bool initAsClient, void* notify, const void* url, bool reuse, FString* err);
using Fn_InitConnect = bool (*)(UNetDriver*, void* notify, const void* url, FString* err);
using Fn_CreateNetDriver_Local = UNetDriver* (*)(UEngine*, void* ctx, FName defName, FName driverName);
static Fn_InitListen o_IpInitListen; static Fn_InitBase o_IpInitBase; static Fn_InitConnect o_IpInitConnect; static Fn_CreateNetDriver_Local o_CreateNetDriver_Local;
static bool H_IpInitListen(UNetDriver* d, void* n, void* url, bool reuse, FString* err) {
    bool r = o_IpInitListen(d, n, url, reuse, err);
    LOGF("[net] UIpNetDriver::InitListen url=%s -> %d err='%s'", UrlToString(url).c_str(), (int)r, err->ToUtf8().c_str());
    return r;
}
static bool H_IpInitBase(UNetDriver* d, bool asClient, void* n, const void* url, bool reuse, FString* err) {
    bool r = o_IpInitBase(d, asClient, n, url, reuse, err);
    LOGF("[net] UIpNetDriver::InitBase client=%d url=%s -> %d err='%s'", (int)asClient, UrlToString(url).c_str(), (int)r, err->ToUtf8().c_str());
    return r;
}
static bool H_IpInitConnect(UNetDriver* d, void* n, const void* url, FString* err) {
    bool r = o_IpInitConnect(d, n, url, err);
    LOGF("[net] UIpNetDriver::InitConnect url=%s -> %d err='%s'", UrlToString(url).c_str(), (int)r, err->ToUtf8().c_str());
    return r;
}
static UNetDriver* H_CreateNetDriver_Local(UEngine* e, void* ctx, FName defName, FName driverName) {
    UNetDriver* r = o_CreateNetDriver_Local(e, ctx, defName, driverName);
    LOGF("[net] CreateNetDriver_Local def=%s name=%s -> %s", defName.ToString().c_str(), driverName.ToString().c_str(), GetFullName((UObject*)r).c_str());
    return r;
}

static Fn_InitBase o_NdInitBase; static bool (*o_InitConnectionClass)(UNetDriver*); static void* (*o_IpGetSocketSubsystem)(UNetDriver*); static void* (*o_ISocketSubsystem_Get)(const FName*);
static bool H_NdInitBase(UNetDriver* d, bool asClient, void* n, const void* url, bool reuse, FString* err) {
    bool r = o_NdInitBase(d, asClient, n, url, reuse, err);
    LOGF("[net] UNetDriver::InitBase -> %d err='%s'", (int)r, err->ToUtf8().c_str());
    return r;
}
static bool H_InitConnectionClass(UNetDriver* d) {
    bool r = o_InitConnectionClass(d);
    LOGF("[net] UNetDriver::InitConnectionClass -> %d (class %s)", (int)r, GetFullName(UE_FIELD(UObject*, d, es2off::UNetDriver::NetConnectionClass)).c_str());
    return r;
}
static void* H_IpGetSocketSubsystem(UNetDriver* d) {
    // OnlineSubsystemSteam registers the STEAM socket subsystem as the *default*; plain UIpNetDriver::GetSocketSubsystem()
    // returns the default and then fails to bind IP sockets.  Force the platform (Windows) socket subsystem for IP play.
    static FName windows = FName::Make(L"Windows");
    void* platform = o_ISocketSubsystem_Get(&windows);
    void* def = o_IpGetSocketSubsystem(d);
    static bool logged = false;
    if (!logged) { LOGF("[net] UIpNetDriver::GetSocketSubsystem default=%p platform(Windows)=%p -> using platform", def, platform); logged = true; }
    return platform ? platform : def;
}
static void* H_ISocketSubsystem_Get(const FName* n) { return o_ISocketSubsystem_Get(n); }
static void H_PostLogin(AGameModeBase* gm, APlayerController* pc) {
    LOGF("[net] AGameModeBase::PostLogin gm=%s pc=%s", GetFullName((UObject*)gm).c_str(), GetFullName((UObject*)pc).c_str());
    o_PostLogin(gm, pc);
    coop::OnPostLogin(pc);
    LOGF("[net] PostLogin done (frame %llu)", (unsigned long long)*Rva<uint64_t>(es2rva::GFrameCounter));
}
static void H_RestartPlayer(AGameModeBase* gm, void* c) {
    LOGF("[net] RestartPlayer controller=%s", GetFullName((UObject*)c).c_str());
    o_RestartPlayer(gm, c);
    UObject* pawn = c ? UE_FIELD(UObject*, c, es2off::AController::Pawn) : nullptr;
    LOGF("[net] RestartPlayer done -> pawn=%s", GetFullName(pawn).c_str());
}
// AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation is written for exactly one player: it
// caches the spawned pawn/controller/playerdata on the game mode (+0x508/+0x510/+0x518) and fills the
// pawn's ShipData from the process-global UPlayerData. When it runs for a joining CLIENT that clobbers
// the host's own game-mode state (out-of-bounds checks, jump handling and more then act on the wrong
// pawn). So for a remote controller we snapshot those fields, and restore them afterwards.
static void* H_SpawnDefaultPawnAtTransform(AGameModeBase* gm, void* c, const FTransform* t) {
    bool remote = c && UE_FIELD(void*, c, es2off::APlayerController::NetConnection) != nullptr;
    LOGF("[net] ES SpawnDefaultPawnAtTransform controller=%s remote=%d at (%.0f,%.0f,%.0f)",
         GetFullName((UObject*)c).c_str(), (int)remote, t->Translation.X, t->Translation.Y, t->Translation.Z);

    void* savedPawn = nullptr; void* savedPC = nullptr; void* savedPD = nullptr;
    bool substituting = false;
    if (remote && gm) {
        savedPawn = UE_FIELD(void*, gm, es2off::AESGameModeBase::ESPlayerPawn);
        savedPC   = UE_FIELD(void*, gm, es2off::AESGameModeBase::ESPlayerController);
        savedPD   = UE_FIELD(void*, gm, es2off::AESGameModeBase::PlayerData);
        players::Player* pl = players::ByController((APlayerController*)c);
        if (pl) substituting = loadout::BeginSubstitution(pl->id);
    }

    void* r = o_SpawnDefaultPawnAtTransform(gm, c, t);

    if (substituting) loadout::EndSubstitution();
    if (remote && gm) {
        UE_FIELD(void*, gm, es2off::AESGameModeBase::ESPlayerPawn) = savedPawn;
        UE_FIELD(void*, gm, es2off::AESGameModeBase::ESPlayerController) = savedPC;
        UE_FIELD(void*, gm, es2off::AESGameModeBase::PlayerData) = savedPD;
        LOGF("[net] restored host game-mode state after remote spawn (pawn=%s)", GetName((UObject*)savedPawn).c_str());
    }
    LOGF("[net] ES SpawnDefaultPawnAtTransform -> %s", GetFullName((UObject*)r).c_str());
    return r;
}
static void H_ServerChangeName(APlayerController* pc, const FString* msg) {
    std::string m = msg->ToUtf8();
    if (coop::OnServerMessage(pc, m)) return;
    LOGF("[net] ServerChangeName from %s: %s", GetFullName((UObject*)pc).c_str(), m.c_str());
    o_ServerChangeName(pc, msg);
}
static void H_ClientMessage(APlayerController* pc, const FString* msg, FName type, float life) {
    std::string m = msg->ToUtf8();
    if (coop::OnClientMessage(pc, m)) return;
    LOGF("[net] ClientMessage to %s: %s (type %s)", GetFullName((UObject*)pc).c_str(), m.c_str(), type.ToString().c_str());
    o_ClientMessage(pc, msg, type, life);
}
static bool H_SetGameMode(UWorld* w, const void* url) {
    LOGF("[net] UWorld::SetGameMode world=%s url=%s netmode=%s", WorldName(w).c_str(), UrlToString(url).c_str(), NetModeName(GetNetMode(w)));
    bool r = o_SetGameMode(w, url);
    LOGF("[net] SetGameMode -> %d gamemode=%s", (int)r, GetFullName((UObject*)GetGameMode(w)).c_str());
    return r;
}
static int H_Browse(UEngine* e, void* ctx, void* url, FString* err) {
    LOGF("[net] UEngine::Browse url=%s", UrlToString(url).c_str());
    int r = o_Browse(e, ctx, url, err);
    LOGF("[net] Browse -> %d err=%s", r, err ? err->ToUtf8().c_str() : "");
    return r;
}
static bool H_Listen(UWorld* w, void* url) {
    LOGF("[net] UWorld::Listen world=%s url=%s", WorldName(w).c_str(), UrlToString(url).c_str());
    bool r = o_Listen(w, url);
    LOGF("[net] Listen -> %d netdriver=%s", (int)r, GetFullName((UObject*)GetNetDriver(w)).c_str());
    return r;
}

// ---------------------------------------------------------------- net driver selection
// GEngine->NetDriverDefinitions: GameNetDriver -> SteamNetDriver by default (Steam P2P).  For IP play we switch it to IpNetDriver.
static std::string g_netDriverMode = "ip";
// persist=false applies the definition for the next net driver without making it the new default or
// touching the Steam accept gate — what `connect steam.<id>` needs (a joiner is not a host).
static bool ApplyNetDriverMode(const std::string& mode, std::string& out, bool persist = true) {
    UEngine* e = GetEngine();
    if (!e) { out += "no engine\n"; return false; }
    struct Def { FName DefName, DriverClassName, DriverClassNameFallback; int32_t MaxChannelsOverride; };
    static_assert(sizeof(Def) == 28, "FNetDriverDefinition size");
    TArray<Def>& defs = UE_FIELD(TArray<Def>, e, es2off::UEngine::NetDriverDefinitions);
    FName game = FName::Make(L"GameNetDriver");
    FName ip = FName::Make(L"/Script/OnlineSubsystemUtils.IpNetDriver");
    FName steam = FName::Make(L"/Script/OnlineSubsystemSteam.SteamNetDriver");
    bool found = false;
    for (int i = 0; i < defs.Num; ++i) {
        Def& d = defs.Data[i];
        if (d.DefName == game) {
            d.DriverClassName = (mode == "steam") ? steam : ip;
            // Leaving the fallback on IpNetDriver makes a Steam failure silent: UE quietly creates an
            // IpNetDriver and nothing says why. Point the fallback at the same class so a failure is visible.
            d.DriverClassNameFallback = (mode == "steam") ? steam : ip;
            found = true;
        }
        out += Format("  netdriverdef %s -> %s (fallback %s)\n", d.DefName.ToString().c_str(), d.DriverClassName.ToString().c_str(), d.DriverClassNameFallback.ToString().c_str());
    }
    if (found && persist) { g_netDriverMode = mode; steamp2p::SetHosting(g_netDriverMode == "steam"); }
    return found;
}
static void CmdNetDriver(const console::Args& a, std::string& out) {
    std::string mode = a.size() > 1 ? a[1] : g_netDriverMode;
    if (mode != "ip" && mode != "steam") { out = "usage: netdriver ip|steam\n"; return; }
    ApplyNetDriverMode(mode, out);
    out += "mode=" + g_netDriverMode + "\n";
}

// ---------------------------------------------------------------- commands
static void CmdListen(const console::Args& a, std::string& out) {
    int port = a.size() > 1 ? atoi(a[1].c_str()) : 7777;
    UGameInstance* gi = GetGameInstance();
    if (!gi) { out = "no game instance\n"; return; }
    ApplyNetDriverMode(g_netDriverMode, out);
    travel::SetListenPort(port);
    bool ok = Rva<std::remove_pointer_t<Fn_EnableListenServer>>(es2rva::UGameInstance_EnableListenServer)(gi, true, port);
    UWorld* w = GetWorld();
    out += Format("EnableListenServer(port %d) -> %d; netmode now %s, netdriver=%s\n", port, (int)ok, NetModeName(GetNetMode(w)), GetFullName((UObject*)GetNetDriver(w)).c_str());
}
static void CmdConnect(const console::Args& a, std::string& out) {
    if (a.size() < 2) { out = "usage: connect <ip[:port]>\n"; return; }
    std::string addr = a[1];
    if (addr.find(':') == std::string::npos && addr.rfind("steam.", 0) != 0) addr += ":7777";
    // The address decides the driver for THIS connection only. Persisting it left a former Steam joiner
    // on SteamNetDriver for a later LAN `listen`, and opened its P2P accept gate as if it were hosting.
    ApplyNetDriverMode(addr.rfind("steam.", 0) == 0 ? "steam" : "ip", out, /*persist=*/false);
    travel::SetHostAddress(addr);
    ExecConsoleCommand("open " + addr);
    out += "issued: open " + addr + "\n";
}
static void CmdTravel(const console::Args& a, std::string& out) {
    if (a.size() < 2) { out = "usage: travel <mapname[?options]>  (UWorld::ServerTravel)\n"; return; }
    UWorld* w = GetWorld();
    FString s(a[1]);
    bool ok = Rva<std::remove_pointer_t<Fn_ServerTravel>>(es2rva::UWorld_ServerTravel)(w, &s, false, false);
    out += Format("ServerTravel(%s) -> %d\n", a[1].c_str(), (int)ok);
}
static void CmdNet(const console::Args&, std::string& out) {
    UWorld* w = GetWorld();
    out += Format("world=%s netmode=%s\n", WorldName(w).c_str(), NetModeName(GetNetMode(w)));
    UNetDriver* nd = GetNetDriver(w);
    if (!nd) { out += "no netdriver\n"; return; }
    out += Format("netdriver=%s name=%s\n", GetFullName((UObject*)nd).c_str(), UE_FIELD(FName, nd, es2off::UNetDriver::NetDriverName).ToString().c_str());
    UNetConnection* sc = UE_FIELD(UNetConnection*, nd, es2off::UNetDriver::ServerConnection);
    auto describe = [&](UNetConnection* c) {
        FString addr; VCall<void, FString*, bool>(c, vt::UNetConnection_LowLevelGetRemoteAddress, &addr, true);   // virtual! base impl is PURE_VIRTUAL (fatal)
        UObject* pc = UE_FIELD(UObject*, c, es2off::UNetConnection::PlayerController);
        UObject* owner = UE_FIELD(UObject*, c, es2off::UNetConnection::OwningActor);
        int state = UE_FIELD(int32_t, c, es2off::UNetConnection::ClientLoginState);
        return Format("%s addr=%s loginstate=%d pc=%s owner=%s", GetName((UObject*)c).c_str(), addr.ToUtf8().c_str(), state, GetFullName(pc).c_str(), GetFullName(owner).c_str());
    };
    if (sc) out += "serverConnection: " + describe(sc) + "\n";
    TArray<UNetConnection*>& cc = UE_FIELD(TArray<UNetConnection*>, nd, es2off::UNetDriver::ClientConnections);
    out += Format("clientConnections=%d\n", cc.Num);
    for (int i = 0; i < cc.Num; ++i) out += "  " + describe(cc[i]) + "\n";
    // player controllers in world
    UClass* pcc = FindClass("PlayerController");
    auto pcs = GetAllActorsOfClass(w, pcc);
    out += Format("playercontrollers=%d\n", (int)pcs.size());
    for (AActor* p : pcs) {
        UObject* pawn = UE_FIELD(UObject*, p, es2off::AController::Pawn);
        out += Format("  %s role=%d pawn=%s\n", GetFullName((UObject*)p).c_str(), GetRole(p), GetFullName(pawn).c_str());
    }
}

// Player capacity. AGameSession::MaxPlayers comes from the packaged ini (UE default 16), and
// AGameSession::AtCapacity prefers the net.MaxPlayersOverride cvar when it is > 0. ES2 itself has no
// player cap of its own, so co-op only needs this raised/confirmed, not patched.
static void CmdMaxPlayers(const console::Args& a, std::string& out) {
    AGameModeBase* gm = GetGameMode(GetWorld());
    if (!gm) { out = "no game mode (host only)\n"; return; }
    UObject* gs = UE_FIELD(UObject*, gm, es2off::AGameModeBase::GameSession);
    if (!gs) { out = "no game session\n"; return; }
    if (a.size() > 1) {
        int n = atoi(a[1].c_str());
        UE_FIELD(int32_t, gs, es2off::AGameSession::MaxPlayers) = n;
        ExecConsoleCommand(Format("net.MaxPlayersOverride %d", n));
        LOGF("[net] MaxPlayers -> %d", n);
    }
    out += Format("GameSession=%s MaxPlayers=%d MaxSpectators=%d (connected=%d)\n",
                  GetName(gs).c_str(),
                  UE_FIELD(int32_t, gs, es2off::AGameSession::MaxPlayers),
                  UE_FIELD(int32_t, gs, es2off::AGameSession::MaxSpectators),
                  players::Count());
}

void Register() {
    console::Register("maxplayers", "maxplayers [N] - show/raise the host's player capacity", CmdMaxPlayers);
    console::Register("listen", "listen [port=7777] - turn current world into a listen server", CmdListen);
    console::Register("connect", "connect <ip[:port]> - open <addr> (join a host)", CmdConnect);
    console::Register("travel", "travel <map> - UWorld::ServerTravel", CmdTravel);
    console::Register("net", "net driver / connections / player controllers", CmdNet);
    console::Register("netdriver", "netdriver [ip|steam] - select GameNetDriver class (default ip)", CmdNetDriver);
}

void OnInit() {
    { std::string o; ApplyNetDriverMode("ip", o); LOGF("[net] %s", o.c_str()); }
    hooks::Install("AGameModeBase::PostLogin", es2rva::AGameModeBase_PostLogin, (void*)&H_PostLogin, (void**)&o_PostLogin);
    hooks::Install("AGameModeBase::RestartPlayer", es2rva::AGameModeBase_RestartPlayer, (void*)&H_RestartPlayer, (void**)&o_RestartPlayer);
    hooks::Install("AESGameModeBase::SpawnDefaultPawnAtTransform", es2rva::AESGameModeBase_SpawnDefaultPawnAtTransform_Implementation, (void*)&H_SpawnDefaultPawnAtTransform, (void**)&o_SpawnDefaultPawnAtTransform);
    hooks::Install("APlayerController::ServerChangeName_Implementation", es2rva::APlayerController_ServerChangeName_Implementation, (void*)&H_ServerChangeName, (void**)&o_ServerChangeName);
    hooks::Install("APlayerController::ClientMessage_Implementation", es2rva::APlayerController_ClientMessage_Implementation, (void*)&H_ClientMessage, (void**)&o_ClientMessage);
    hooks::Install("UWorld::SetGameMode", es2rva::UWorld_SetGameMode, (void*)&H_SetGameMode, (void**)&o_SetGameMode);
    hooks::Install("UEngine::Browse", es2rva::UEngine_Browse, (void*)&H_Browse, (void**)&o_Browse);
    hooks::Install("UWorld::Listen", es2rva::UWorld_Listen, (void*)&H_Listen, (void**)&o_Listen);
    hooks::Install("UIpNetDriver::InitListen", es2rva::UIpNetDriver_InitListen, (void*)&H_IpInitListen, (void**)&o_IpInitListen);
    hooks::Install("UIpNetDriver::InitBase", es2rva::UIpNetDriver_InitBase, (void*)&H_IpInitBase, (void**)&o_IpInitBase);
    hooks::Install("UIpNetDriver::InitConnect", es2rva::UIpNetDriver_InitConnect, (void*)&H_IpInitConnect, (void**)&o_IpInitConnect);
    hooks::Install("UNetDriver::InitBase", es2rva::UNetDriver_InitBase, (void*)&H_NdInitBase, (void**)&o_NdInitBase);
    hooks::Install("UNetDriver::InitConnectionClass", es2rva::UNetDriver_InitConnectionClass, (void*)&H_InitConnectionClass, (void**)&o_InitConnectionClass);
    hooks::Install("UIpNetDriver::GetSocketSubsystem", es2rva::UIpNetDriver_GetSocketSubsystem, (void*)&H_IpGetSocketSubsystem, (void**)&o_IpGetSocketSubsystem);
    o_ISocketSubsystem_Get = Rva<std::remove_pointer_t<decltype(o_ISocketSubsystem_Get)>>(es2rva::ISocketSubsystem_Get);
    hooks::Install("CreateNetDriver_Local", es2rva::CreateNetDriver_Local, (void*)&H_CreateNetDriver_Local, (void**)&o_CreateNetDriver_Local);
}
}
