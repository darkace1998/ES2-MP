// Co-op core: role detection, MP-mode adjustments, per-player registry, the mod's message channel,
// and smoothed replication of each client's ship onto its host-side pawn.
//
// Message channel (strings, because the reliable engine RPCs we borrow carry an FString):
//   client -> host : APlayerController::ServerChangeName   (reliable, server-executed)
//   host  -> client: APlayerController::ClientMessage      (reliable, client-executed)
// Every mod message starts with '$' so a genuine name change is never swallowed.
#include "coop.h"
#include "players.h"
#include "console.h"
#include "steamp2p.h"
#include <map>
#include "log.h"
#include "hooks.h"
#include "combat.h"
#include "loadout.h"
#include "travel.h"
#include "world_state.h"
#include "loot.h"
#include "respawn.h"
#include "attribution.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdlib>

using namespace ue;
using es2coop::Format;

namespace coop {

// ---------------------------------------------------------------- engine fn typedefs
using Fn_ServerChangeName = void (*)(APlayerController*, const FString*);              // client -> server RPC stub
using Fn_ClientMessage = void (*)(APlayerController*, const FString*, FName, float);   // server -> client RPC stub
using Fn_SetPhysicsLinearVelocity = void (*)(void* prim, const FVector* v, bool add, FName bone);
using Fn_GetVelocity = void (*)(const AActor*, FVector* out);
using Fn_GetGameUserSettings = UObject* (*)(UEngine*);
using Fn_Void = void (*)();

static Role g_role = Role::None;
static float g_sendHz = 20.f;
static double g_sendAccum = 0;
static uint64_t g_txCount = 0, g_rxCount = 0;
static bool g_verbose = false;
static double g_now = 0;                 // monotonic seconds accumulated from tick dt
static bool g_welcomed = false;          // client: the host has acknowledged us
// A client only has a registry slot for itself, so roster names for everyone else live here.
static std::map<int, std::string> g_remoteNames;
static void RememberRemoteName(int id, const std::string& n) { g_remoteNames[id] = n; }
std::string RosterName(int id) {
    if (players::Player* p = players::ById(id)) if (!p->name.empty()) return p->name;
    auto it = g_remoteNames.find(id);
    return it == g_remoteNames.end() ? std::string() : it->second;
}
static void ApplyRoster(const std::string& body);
int RosterCount() {
    int n = players::Count();
    for (auto& [id, nm] : g_remoteNames) if (!players::ById(id)) ++n;
    return n;
}
static Fn_Void o_PushPause = nullptr, o_PopPause = nullptr;

// smoothing tunables
static double g_smoothRate = 12.0;       // 1/s, position convergence
static double g_rotRate = 18.0;          // 1/s, rotation convergence
static double g_snapDist = 20000.0;      // uu; beyond this we teleport instead of easing
static double g_maxExtrap = 0.4;         // s of dead-reckoning allowed

Role CurrentRole() {
    int nm = GetNetMode(GetWorld());
    if (nm == 2 || nm == 1) return Role::Host;
    if (nm == 3) return Role::Client;
    return Role::None;
}
static const char* RoleName(Role r) { return r == Role::Host ? "Host" : r == Role::Client ? "Client" : "None"; }

// ---------------------------------------------------------------- small math
static inline double Dot(const FQuat& a, const FQuat& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W; }
static FQuat Slerp(FQuat a, const FQuat& b, double t) {
    double d = Dot(a, b);
    if (d < 0) { a.X = -a.X; a.Y = -a.Y; a.Z = -a.Z; a.W = -a.W; d = -d; }
    FQuat r;
    if (d > 0.9995) {           // nearly parallel: lerp + normalize
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
static inline double Dist(const FVector& a, const FVector& b) {
    double dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return sqrt(dx * dx + dy * dy + dz * dz);
}

// ---------------------------------------------------------------- helpers
static AActor* LocalPawn() {
    APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
    return pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
}
static void SetPhysVelocity(AActor* pawn, const FVector& v) {
    void* root = UE_FIELD(void*, pawn, es2off::AActor::RootComponent);
    if (root) Rva<std::remove_pointer_t<Fn_SetPhysicsLinearVelocity>>(es2rva::UPrimitiveComponent_SetPhysicsLinearVelocity)(root, &v, false, FName{});
}

static void ApplyNoPause() {
    UEngine* e = GetEngine();
    if (!e) return;
    UObject* gus = Rva<std::remove_pointer_t<Fn_GetGameUserSettings>>(es2rva::UEngine_GetGameUserSettings)(e);
    if (!gus) return;
    for (auto& p : GetProperties((UStruct*)GetClass(gus), true))
        if (p.Name == "bPauseGameWhenFocusLost") { SetPropValueFromString(gus, p, "false"); LOGF("[coop] bPauseGameWhenFocusLost=false"); }
}

// Uncap net relevancy distance so partners stay replicated across a whole location.
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

// ---------------------------------------------------------------- channel
void SendToServer(const std::string& msg) {
    APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
    if (!pc) return;
    FString s("$" + msg);
    Rva<std::remove_pointer_t<Fn_ServerChangeName>>(es2rva::APlayerController_ServerChangeName)(pc, &s);
    ++g_txCount;
}
void SendToClient(APlayerController* pc, const std::string& msg) {
    if (!pc) return;
    FString s("$" + msg);
    Rva<std::remove_pointer_t<Fn_ClientMessage>>(es2rva::APlayerController_ClientMessage)(pc, &s, FName{}, 0.f);
    ++g_txCount;
}
void SendToAllClients(const std::string& msg) {
    for (auto* p : players::All()) if (!p->local && p->pc) SendToClient(p->pc, msg);
}

// ---------------------------------------------------------------- transform sync
static void SendLocalTransform() {
    AActor* pawn = LocalPawn();
    if (!pawn) return;
    FTransform t = GetActorTransform(pawn);
    FVector vel{};
    Rva<std::remove_pointer_t<Fn_GetVelocity>>(es2rva::AActor_GetVelocity)(pawn, &vel);
    SendToServer(Format("T|%.1f|%.1f|%.1f|%.5f|%.5f|%.5f|%.5f|%.1f|%.1f|%.1f",
        t.Translation.X, t.Translation.Y, t.Translation.Z,
        t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W, vel.X, vel.Y, vel.Z));
}

static bool RecvTransform(APlayerController* pc, const std::string& body) {
    double v[10];
    if (sscanf(body.c_str(), "%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) != 10) return false;
    players::Player* pl = players::ByController(pc);
    if (!pl) pl = players::RegisterController(pc);
    if (!pl) return true;
    pl->tgtLoc = FVector{v[0], v[1], v[2]};
    pl->tgtRot = FQuat{v[3], v[4], v[5], v[6]};
    pl->tgtVel = FVector{v[7], v[8], v[9]};
    pl->tgtTime = g_now;
    pl->hasTarget = true;
    ++pl->rxPackets;
    pl->lastRxTime = g_now;
    ++g_rxCount;
    if (g_verbose && (pl->rxPackets % 40 == 1))
        LOGF("[coop] rx T p%d -> (%.0f %.0f %.0f)", pl->id, v[0], v[1], v[2]);
    // relay to the other clients so they can see this ship move (2-player needs no relay)
    if (players::Count() > 2) {
        std::string relay = Format("PT|%d|%s", pl->id, body.c_str());
        for (auto* o : players::All())
            if (!o->local && o->pc && o->pc != pc) SendToClient(o->pc, relay);
    }
    return true;
}

// A client-driven ship must NOT have the server's copy of its movement replicated back to the owning
// client: the server transform (which we are writing from the client's own reports, one round-trip late)
// fights the client's local physics and the ship rubberbands. The client is authoritative over its own
// ship; the host mirrors it locally for AI/collision/damage and forwards it to the OTHER clients.
static bool g_ownerAuthoritativeMovement = true;
static void EnforceMovementAuthority() {
    if (!g_ownerAuthoritativeMovement) return;
    for (auto* p : players::All()) {
        if (p->local || !p->pawn) continue;
        if (GetReplicateMovement(p->pawn)) {
            SetReplicateMovement(p->pawn, false);
            LOGF("[coop] p%d: movement replication off (client owns its ship)", p->id);
        }
    }
}

// Host: ease each client-driven pawn toward its last reported state instead of teleporting at packet rate.
static void SmoothRemotePawns(float dt) {
    for (auto* p : players::All()) {
        if (p->local || !p->hasTarget || !p->pawn) continue;
        double age = g_now - p->tgtTime;
        if (age > 5.0) continue;                       // stale: leave the pawn alone
        double ex = age < g_maxExtrap ? age : g_maxExtrap;
        FVector predicted{ p->tgtLoc.X + p->tgtVel.X * ex,
                           p->tgtLoc.Y + p->tgtVel.Y * ex,
                           p->tgtLoc.Z + p->tgtVel.Z * ex };
        FTransform cur = GetActorTransform(p->pawn);
        double err = Dist(cur.Translation, predicted);
        FTransform next = cur;
        if (err > g_snapDist) {
            next.Translation = predicted;
            next.Rotation = p->tgtRot;
        } else {
            double a = dt * g_smoothRate; if (a > 1) a = 1;
            next.Translation = FVector{ cur.Translation.X + (predicted.X - cur.Translation.X) * a,
                                        cur.Translation.Y + (predicted.Y - cur.Translation.Y) * a,
                                        cur.Translation.Z + (predicted.Z - cur.Translation.Z) * a };
            double ra = dt * g_rotRate; if (ra > 1) ra = 1;
            next.Rotation = Slerp(cur.Rotation, p->tgtRot, ra);
        }
        SetActorTransform(p->pawn, next, false, 1 /*TeleportPhysics*/);
        SetPhysVelocity(p->pawn, p->tgtVel);
    }
}

// ---------------------------------------------------------------- message dispatch
bool OnServerMessage(APlayerController* fromPC, const std::string& raw) {
    if (raw.empty() || raw[0] != '$') return false;      // not ours
    std::string msg = raw.substr(1);
    size_t bar = msg.find('|');
    std::string op = bar == std::string::npos ? msg : msg.substr(0, bar);
    std::string body = bar == std::string::npos ? "" : msg.substr(bar + 1);
    if (g_role != Role::Host) return true;               // only the host acts on these
    if (op == "T") { RecvTransform(fromPC, body); return true; }
    if (op == "HELLO") {
        players::Player* pl = players::ByController(fromPC);
        if (!pl) pl = players::RegisterController(fromPC);
        if (pl) {
            if (!body.empty()) pl->name = body;
            LOGF("[coop] HELLO from %s -> id %d", pl->name.c_str(), pl->id);
            SendToClient(fromPC, Format("WELCOME|%d|%d", pl->id, players::Count()));
            BroadcastRoster();
        }
        return true;
    }
    if (op == "SAY") { LOGF("[coop] say(client): %s", body.c_str()); SendToAllClients("SAY|" + body); return true; }
    if (combat::OnServerOp(fromPC, op, body)) return true;
    if (loadout::OnServerOp(fromPC, op, body)) return true;
    if (travel::OnServerOp(fromPC, op, body)) return true;
    if (world_state::OnServerOp(fromPC, op, body)) return true;
    LOGF("[coop] unhandled client op '%s'", op.c_str());
    return true;
}

bool OnClientMessage(APlayerController* toPC, const std::string& raw) {
    if (raw.empty() || raw[0] != '$') return false;
    std::string msg = raw.substr(1);
    size_t bar = msg.find('|');
    std::string op = bar == std::string::npos ? msg : msg.substr(0, bar);
    std::string body = bar == std::string::npos ? "" : msg.substr(bar + 1);
    ++g_rxCount;
    if (op == "WELCOME") {
        int id = atoi(body.c_str());
        players::SetLocalId(id);
        g_welcomed = true;
        APlayerController* me = GetFirstLocalPlayerController(GetWorld());
        if (me) players::RegisterLocalAs(me, id);
        LOGF("[coop] WELCOME: I am player %d (%s)", id, body.c_str());
        return true;
    }
    if (op == "PT") {
        int id = atoi(body.c_str());
        size_t b2 = body.find('|');
        if (b2 != std::string::npos && id != players::LocalId()) {
            players::Player* p = players::ById(id);
            double v[10];
            if (p && p->pawn && sscanf(body.c_str() + b2 + 1, "%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf",
                                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) == 10) {
                p->tgtLoc = FVector{v[0], v[1], v[2]}; p->tgtRot = FQuat{v[3], v[4], v[5], v[6]};
                p->tgtVel = FVector{v[7], v[8], v[9]}; p->tgtTime = g_now; p->hasTarget = true;
            }
        }
        return true;
    }
    if (op == "SAY") { LOGF("[coop] say(host): %s", body.c_str()); return true; }
    if (op == "ROSTER") { ApplyRoster(body); return true; }
    if (combat::OnClientOp(op, body)) return true;
    if (loadout::OnClientOp(op, body)) return true;
    if (travel::OnClientOp(op, body)) return true;
    if (world_state::OnClientOp(op, body)) return true;
    if (loot::OnClientOp(op, body)) return true;
    if (respawn::OnClientOp(op, body)) return true;
    LOGF("[coop] unhandled host op '%s'", op.c_str());
    return true;
}

// ---------------------------------------------------------------- pause hooks
static void H_PushPause() { if (g_role != Role::None) { static int n = 0; if (n++ < 3) LOGF("[coop] PushPause suppressed (MP)"); return; } o_PushPause(); }
static void H_PopPause() { if (g_role != Role::None) return; o_PopPause(); }

// ---------------------------------------------------------------- tick
static bool g_helloSent = false;
static UWorld* g_lastWorld = nullptr;
static double g_helloAccum = 0;

static void OnRoleChanged(Role r) {
    LOGF("[coop] role -> %s (world %s)", RoleName(r), WorldName(GetWorld()).c_str());
    players::Reset();
    loadout::ResetSession();
    g_helloSent = false; g_welcomed = false; g_helloAccum = 0;
    if (r != Role::None) { ApplyNoPause(); travel::ApplyOriginShiftPolicy(true); }
    combat::SetClientDamageBlock(r == Role::Client);
    if (r == Role::Host) {
        players::SetLocalId(0);
        APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
        if (pc) players::RegisterController(pc);
        LOGF("[coop] relevancy sweep: %d pawns", RelevancySweep());
    }
}

static double g_sweepAccum = 0;
// The lobby overview wants real names, so the client introduces itself with its Steam persona name
// and the host echoes the whole roster back out. Falls back to a generic label without Steam.
std::string LocalPlayerName() {
    std::string n = steamp2p::PersonaName();
    if (!n.empty()) return n;
    return CurrentRole() == Role::Host ? "Host" : "Player";
}

void BroadcastRoster() {
    if (CurrentRole() != Role::Host) return;
    // The host's own entry is not in the registry under a name until now; label it from Steam.
    if (players::Player* me = players::ById(0)) if (me->name.empty()) me->name = LocalPlayerName();
    std::string msg = "ROSTER";
    for (auto* p : players::All()) msg += Format("|%d=%s", p->id, p->name.empty() ? "Player" : p->name.c_str());
    SendToAllClients(msg);
}

static void ApplyRoster(const std::string& body) {
    // body: "<id>=<name>|<id>=<name>|..."
    size_t pos = 0;
    while (pos < body.size()) {
        size_t bar = body.find('|', pos);
        std::string tok = body.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
        size_t eq = tok.find('=');
        if (eq != std::string::npos) {
            int id = atoi(tok.substr(0, eq).c_str());
            std::string name = tok.substr(eq + 1);
            if (players::Player* p = players::ById(id)) p->name = name;
            else RememberRemoteName(id, name);
        }
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
}

static void Tick(float dt) {
    g_now += dt;
    UWorld* w = GetWorld();
    Role r = CurrentRole();
    if (w != g_lastWorld) {
        // A map load invalidates every actor pointer we hold (and single-player loads fire PostLogin too).
        g_lastWorld = w;
        players::Reset();
        g_helloSent = false; g_welcomed = false; g_helloAccum = 0;
        if (r == Role::Host) {
            players::SetLocalId(0);
            APlayerController* pc = GetFirstLocalPlayerController(w);
            if (pc) players::RegisterController(pc);
        }
        if (r != Role::None) travel::ApplyOriginShiftPolicy(true);
        LOGF("[coop] world -> %s (role %s)", WorldName(w).c_str(), RoleName(r));
    }
    if (r != g_role) { g_role = r; OnRoleChanged(r); }
    travel::Tick(dt, r == Role::Host);
    if (r == Role::None) return;
    players::Refresh();
    loadout::HudRebindTick();   // the HUD caches the pawn; it must follow every pawn swap
    if (r == Role::Host) {
        g_sweepAccum += dt;
        if (g_sweepAccum > 0.5) { g_sweepAccum = 0; RelevancySweep(); }
        EnforceMovementAuthority();
        SmoothRemotePawns(dt);
        combat::Tick(dt, true);
        loadout::HostTick(dt);
        world_state::Tick(dt, true);
        respawn::Tick(dt, true);
    } else if (r == Role::Client) {
        // The first HELLO can be dropped if it beats the connection into steady state; retry until acknowledged.
        // HELLO only needs a local controller — the host registers the joining player by its
        // PlayerController and replies with an id. Requiring a pawn here held the whole handshake
        // (and therefore the loadout transfer behind it) until the host's placeholder ship had been
        // spawned and replicated back, which measured 18-25 s. If we do send during a transition map,
        // the world-change reset below clears g_welcomed and we simply say hello again.
        if (!g_welcomed && GetFirstLocalPlayerController(GetWorld())) {
            g_helloAccum += dt;
            if (!g_helloSent || g_helloAccum > 2.0) { SendToServer("HELLO|" + LocalPlayerName()); g_helloSent = true; g_helloAccum = 0; }
        }
        g_sendAccum += dt;
        if (g_sendAccum >= 1.0 / g_sendHz) { g_sendAccum = 0; SendLocalTransform(); }
        SmoothRemotePawns(dt);   // other players' ships, fed by the host's PT relay
        if (g_welcomed) { loadout::MaybeSendOnJoin(); loadout::ClientTick(dt); }
        combat::ClientAimTick(dt);
        loadout::ClientLocalShipTick();
        loadout::ClientBuildWeaponsTick();
        world_state::Tick(dt, false);
    }
}

// ---------------------------------------------------------------- commands
static void CmdCoop(const console::Args& a, std::string& out) {
    if (a.size() > 2 && a[1] == "hz") g_sendHz = (float)atof(a[2].c_str());
    else if (a.size() > 2 && a[1] == "verbose") g_verbose = a[2] == "1";
    else if (a.size() > 2 && a[1] == "smooth") g_smoothRate = atof(a[2].c_str());
    else if (a.size() > 2 && a[1] == "rotrate") g_rotRate = atof(a[2].c_str());
    else if (a.size() > 2 && a[1] == "snap") g_snapDist = atof(a[2].c_str());
    else if (a.size() > 2 && a[1] == "ownermove") g_ownerAuthoritativeMovement = a[2] == "1";
    else if (a.size() > 1 && a[1] == "sweep") out += Format("swept %d\n", RelevancySweep());
    else if (a.size() > 1 && a[1] == "nopause") ApplyNoPause();
    else if (a.size() > 2 && a[1] == "say") {
        std::string m; for (size_t i = 2; i < a.size(); ++i) m += (i > 2 ? " " : "") + a[i];
        if (g_role == Role::Client) SendToServer("SAY|" + m); else SendToAllClients("SAY|" + m);
        out += "sent\n";
    }
    out += Format("role=%s localId=%d players=%d tx=%llu rx=%llu hz=%.0f smooth=%.1f rot=%.1f snap=%.0f ownerMove=%d verbose=%d\n",
                  RoleName(g_role), players::LocalId(), players::Count(),
                  (unsigned long long)g_txCount, (unsigned long long)g_rxCount, g_sendHz, g_smoothRate, g_rotRate, g_snapDist,
                  (int)g_ownerAuthoritativeMovement, (int)g_verbose);
    for (auto* p : players::All())
        if (p->pawn) out += Format("  p%d %-8s repMove=%d hasTarget=%d rx=%llu\n", p->id, p->name.c_str(),
                                   (int)GetReplicateMovement(p->pawn), (int)p->hasTarget, (unsigned long long)p->rxPackets);
}

static void CmdPlayers(const console::Args&, std::string& out) { out += players::Describe(); }

// push <vx> <vy> <vz> : give the local ship a velocity. Used to verify the owning client keeps control
// of its own movement (no server correction / rubberbanding).
static void CmdPush(const console::Args& a, std::string& out) {
    AActor* pawn = LocalPawn();
    if (!pawn) { out = "no local pawn\n"; return; }
    FVector v{ a.size() > 1 ? atof(a[1].c_str()) : 0.0, a.size() > 2 ? atof(a[2].c_str()) : 0.0, a.size() > 3 ? atof(a[3].c_str()) : 0.0 };
    SetPhysVelocity(pawn, v);
    FTransform t = GetActorTransform(pawn);
    out += Format("pushed (%.0f %.0f %.0f); pos now (%.0f, %.0f, %.0f)\n", v.X, v.Y, v.Z, t.Translation.X, t.Translation.Y, t.Translation.Z);
}

static void CmdWhere(const console::Args&, std::string& out) {
    AActor* pawn = LocalPawn();
    if (!pawn) { out = "no local pawn\n"; return; }
    FTransform t = GetActorTransform(pawn);
    FVector vel{};
    Rva<std::remove_pointer_t<Fn_GetVelocity>>(es2rva::AActor_GetVelocity)(pawn, &vel);
    out += Format("%.0f %.0f %.0f  vel=(%.0f %.0f %.0f) |v|=%.0f\n", t.Translation.X, t.Translation.Y, t.Translation.Z, vel.X, vel.Y, vel.Z,
                  sqrt(vel.X*vel.X + vel.Y*vel.Y + vel.Z*vel.Z));
}

static void CmdTp(const console::Args& a, std::string& out) {
    AActor* pawn = LocalPawn();
    if (!pawn) { out = "no local pawn\n"; return; }
    FTransform t = GetActorTransform(pawn);
    if (a.size() >= 4) { t.Translation = FVector{atof(a[1].c_str()), atof(a[2].c_str()), atof(a[3].c_str())}; SetActorTransform(pawn, t, false, 1); }
    else if (a.size() == 2) {
        // tp <playerId> : jump next to that player's ship
        int id = atoi(a[1].c_str());
        players::Player* p = players::ById(id);
        if (p && p->pawn && p->pawn != pawn) { t = GetActorTransform(p->pawn); t.Translation.X += 3000; SetActorTransform(pawn, t, false, 1); }
        else {
            UClass* esPawn = FindClass("/Script/ES2.ESPawn");
            for (AActor* o : GetAllActorsOfClass(GetWorld(), esPawn))
                if (o != pawn && GetName((UObject*)o).find("Ship_Player") != std::string::npos) { t = GetActorTransform(o); t.Translation.X += 3000; SetActorTransform(pawn, t, false, 1); break; }
        }
    }
    t = GetActorTransform(pawn);
    out += Format("pawn at (%.0f, %.0f, %.0f)\n", t.Translation.X, t.Translation.Y, t.Translation.Z);
}

void Register() {
    console::Register("coop", "coop [hz N|verbose 0/1|smooth R|rotrate R|snap D|sweep|say <text>|nopause] - co-op core", CmdCoop);
    console::Register("players", "list the player registry (id, controller, pawn, position)", CmdPlayers);
    console::Register("push", "push <vx> <vy> <vz> - set the local ship's velocity (rubberband test)", CmdPush);
    console::Register("where", "local pawn position and velocity", CmdWhere);
    console::Register("tp", "tp <x> <y> <z> | tp <playerId> - teleport the local pawn", CmdTp);
}
void OnInit() {
    hooks::Install("UESGameInstance::PushPause", es2rva::UESGameInstance_PushPause, (void*)&H_PushPause, (void**)&o_PushPause);
    hooks::Install("UESGameInstance::PopPause", es2rva::UESGameInstance_PopPause, (void*)&H_PopPause, (void**)&o_PopPause);
    console::RegisterTick("coop", Tick);
}

// called from the net.cpp login hooks
void OnPostLogin(APlayerController* pc) {
    if (CurrentRole() != Role::Host) return;   // single-player map loads fire PostLogin too
    players::RegisterController(pc);
    world_state::OnPlayerJoined(pc);
}
void OnLogout(APlayerController* pc) { players::UnregisterController(pc); }
}
