// Co-op core: role detection, MP-mode adjustments (no pause, relevancy), and the player-ship transform channel.
#include "coop.h"
#include "console.h"
#include "log.h"
#include "hooks.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <map>

using namespace ue;
using es2coop::Format;

namespace coop {

// ---------------------------------------------------------------- engine fn typedefs
using Fn_ServerChangeName = void (*)(APlayerController*, const FString*);                 // RPC-sending stub (client -> server)
using Fn_ClientMessage = void (*)(APlayerController*, const FString*, FName, float);     // RPC-sending stub (server -> client)
using Fn_SetPhysicsLinearVelocity = void (*)(void* prim, const FVector* v, bool add, FName bone);
using Fn_GetVelocity = void (*)(const AActor*, FVector* out);
using Fn_GetGameUserSettings = UObject* (*)(UEngine*);
using Fn_Void = void (*)();

static Role g_role = Role::None;
static float g_sendHz = 20.f;
static double g_sendAccum = 0;
static uint64_t g_txCount = 0, g_rxCount = 0;
static bool g_verbose = false;
static Fn_Void o_PushPause = nullptr, o_PopPause = nullptr;

Role CurrentRole() {
    int nm = GetNetMode(GetWorld());
    if (nm == 2 || nm == 1) return Role::Host;
    if (nm == 3) return Role::Client;
    return Role::None;
}
static const char* RoleName(Role r) { return r == Role::Host ? "Host" : r == Role::Client ? "Client" : "None"; }

// ---------------------------------------------------------------- helpers
static AActor* LocalPawn() {
    APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
    return pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
}
static std::vector<APlayerController*> RemotePlayerControllers() {
    std::vector<APlayerController*> out;
    UClass* pcc = FindClass("/Script/Engine.PlayerController");
    if (!pcc) return out;
    for (AActor* a : GetAllActorsOfClass(GetWorld(), pcc)) {
        if (UE_FIELD(void*, a, es2off::APlayerController::NetConnection) != nullptr) out.push_back((APlayerController*)a);
    }
    return out;
}
static void SetFloatProp(AActor* a, uint32_t off, float v) { UE_FIELD(float, a, off) = v; }

static void ApplyNoPause() {
    UEngine* e = GetEngine();
    if (!e) return;
    UObject* gus = Rva<std::remove_pointer_t<Fn_GetGameUserSettings>>(es2rva::UEngine_GetGameUserSettings)(e);
    if (!gus) return;
    for (auto& p : GetProperties((UStruct*)GetClass(gus), true)) {
        if (p.Name == "bPauseGameWhenFocusLost") { SetPropValueFromString(gus, p, "false"); LOGF("[coop] bPauseGameWhenFocusLost=false"); }
    }
}

// ---------------------------------------------------------------- relevancy (host)
static int RelevancySweep() {
    UClass* esPawn = FindClass("/Script/ES2.ESPawn");
    if (!esPawn) return 0;
    int n = 0;
    for (AActor* a : GetAllActorsOfClass(GetWorld(), esPawn)) {
        float& cull = UE_FIELD(float, a, es2off::AActor::NetCullDistanceSquared);
        if (cull < 1e20f) { cull = 1e30f; ++n; }
    }
    return n;
}

// ---------------------------------------------------------------- transform channel
static void SendLocalTransform() {
    AActor* pawn = LocalPawn();
    if (!pawn) return;
    FTransform t = GetActorTransform(pawn);
    FVector vel{};
    Rva<std::remove_pointer_t<Fn_GetVelocity>>(es2rva::AActor_GetVelocity)(pawn, &vel);
    std::string msg = Format("T|%.1f|%.1f|%.1f|%.5f|%.5f|%.5f|%.5f|%.1f|%.1f|%.1f",
        t.Translation.X, t.Translation.Y, t.Translation.Z, t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W, vel.X, vel.Y, vel.Z);
    SendToServer(msg);
}

static bool ApplyTransformMsg(APlayerController* pc, const std::string& msg) {
    double v[11];
    if (sscanf(msg.c_str(), "T|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) != 10) return false;
    AActor* pawn = UE_FIELD(AActor*, pc, es2off::AController::Pawn);
    if (!pawn) return true;
    FTransform t = GetActorTransform(pawn);
    t.Translation = FVector{v[0], v[1], v[2]};
    t.Rotation = FQuat{v[3], v[4], v[5], v[6]};
    SetActorTransform(pawn, t, false, 1 /*TeleportPhysics*/);
    void* root = UE_FIELD(void*, pawn, es2off::AActor::RootComponent);
    if (root) {
        FVector vel{v[7], v[8], v[9]};
        Rva<std::remove_pointer_t<Fn_SetPhysicsLinearVelocity>>(es2rva::UPrimitiveComponent_SetPhysicsLinearVelocity)(root, &vel, false, FName{});
    }
    ++g_rxCount;
    if (g_verbose && (g_rxCount % 40 == 1)) LOGF("[coop] rx T from %s -> (%.0f %.0f %.0f)", GetName((UObject*)pc).c_str(), v[0], v[1], v[2]);
    return true;
}

void SendToServer(const std::string& msg) {
    APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
    if (!pc) return;
    FString s(msg);
    Rva<std::remove_pointer_t<Fn_ServerChangeName>>(es2rva::APlayerController_ServerChangeName)(pc, &s);
    ++g_txCount;
}
void SendToClient(APlayerController* pc, const std::string& msg) {
    FString s("ES2C|" + msg);
    Rva<std::remove_pointer_t<Fn_ClientMessage>>(es2rva::APlayerController_ClientMessage)(pc, &s, FName{}, 0.f);
}
void SendToAllClients(const std::string& msg) { for (APlayerController* pc : RemotePlayerControllers()) SendToClient(pc, msg); }

bool OnServerMessage(APlayerController* fromPC, const std::string& msg) {
    if (msg.rfind("T|", 0) == 0) { if (g_role == Role::Host) ApplyTransformMsg(fromPC, msg); return true; }
    if (msg.rfind("ES2C|", 0) == 0) { LOGF("[coop] server got: %s", msg.c_str()); return true; }
    return false;
}
bool OnClientMessage(APlayerController* toPC, const std::string& msg) {
    if (msg.rfind("ES2C|", 0) != 0) return false;
    LOGF("[coop] client got: %s", msg.c_str());
    return true;
}

// ---------------------------------------------------------------- pause hooks
static void H_PushPause() { if (g_role != Role::None) { static int n = 0; if (n++ < 3) LOGF("[coop] PushPause suppressed (MP)"); return; } o_PushPause(); }
static void H_PopPause() { if (g_role != Role::None) return; o_PopPause(); }

// ---------------------------------------------------------------- tick
static void OnRoleChanged(Role r) {
    LOGF("[coop] role -> %s (world %s)", RoleName(r), WorldName(GetWorld()).c_str());
    if (r != Role::None) ApplyNoPause();
    if (r == Role::Host) LOGF("[coop] relevancy sweep: %d pawns", RelevancySweep());
}
static double g_sweepAccum = 0;
static void Tick(float dt) {
    Role r = CurrentRole();
    if (r != g_role) { g_role = r; OnRoleChanged(r); }
    if (r == Role::Host) {
        g_sweepAccum += dt;
        if (g_sweepAccum > 0.5) { g_sweepAccum = 0; RelevancySweep(); }
    } else if (r == Role::Client) {
        g_sendAccum += dt;
        if (g_sendAccum >= 1.0 / g_sendHz) { g_sendAccum = 0; SendLocalTransform(); }
    }
}

// ---------------------------------------------------------------- commands
static void CmdCoop(const console::Args& a, std::string& out) {
    out += Format("role=%s tx=%llu rx=%llu sendHz=%.0f verbose=%d\n", RoleName(g_role), (unsigned long long)g_txCount, (unsigned long long)g_rxCount, g_sendHz, (int)g_verbose);
    if (a.size() > 2 && a[1] == "hz") g_sendHz = (float)atof(a[2].c_str());
    if (a.size() > 2 && a[1] == "verbose") g_verbose = a[2] == "1";
    if (a.size() > 1 && a[1] == "sweep") out += Format("swept %d\n", RelevancySweep());
    if (a.size() > 2 && a[1] == "say") { std::string m; for (size_t i = 2; i < a.size(); ++i) m += (i > 2 ? " " : "") + a[i]; if (g_role == Role::Client) SendToServer("ES2C|say " + m); else SendToAllClients("say " + m); out += "sent\n"; }
    if (a.size() > 1 && a[1] == "nopause") ApplyNoPause();
}
static void CmdTp(const console::Args& a, std::string& out) {
    AActor* pawn = LocalPawn();
    if (!pawn) { out = "no local pawn\n"; return; }
    FTransform t = GetActorTransform(pawn);
    if (a.size() >= 4) { t.Translation = FVector{atof(a[1].c_str()), atof(a[2].c_str()), atof(a[3].c_str())}; SetActorTransform(pawn, t, false, 1); }
    else if (a.size() == 2 && a[1] == "host") { // client: jump next to the first simulated-proxy player ship
        UClass* esPawn = FindClass("/Script/ES2.ESPawn");
        for (AActor* o : GetAllActorsOfClass(GetWorld(), esPawn)) if (o != pawn && GetName((UObject*)o).find("Ship_Player") != std::string::npos) { t = GetActorTransform(o); t.Translation.X += 3000; SetActorTransform(pawn, t, false, 1); break; }
    }
    t = GetActorTransform(pawn);
    out += Format("pawn at (%.0f, %.0f, %.0f)\n", t.Translation.X, t.Translation.Y, t.Translation.Z);
}

void Register() {
    console::Register("coop", "coop [hz N|verbose 0/1|sweep|say <text>|nopause] - co-op core status/controls", CmdCoop);
    console::Register("tp", "tp <x> <y> <z> | tp host - teleport local pawn", CmdTp);
}
void OnInit() {
    hooks::Install("UESGameInstance::PushPause", es2rva::UESGameInstance_PushPause, (void*)&H_PushPause, (void**)&o_PushPause);
    hooks::Install("UESGameInstance::PopPause", es2rva::UESGameInstance_PopPause, (void*)&H_PopPause, (void**)&o_PopPause);
    console::RegisterTick("coop", Tick);
}
}
