// Credit the player who actually did it.
//
// ES2 is written for one player, so every "who is the player?" accessor collapses into
// UGameplayStatics::GetPlayerController(world, 0) — GetESPlayerPawn, GetESPlayerController, GetESHUD,
// GetPlayerPawn and GetPlayerControllerWithoutContext all tail into it. On a listen server a remote
// client is a UNetConnection, not a ULocalPlayer, so index 0 is *always* the host: a client's kill
// awards the host's XP, and the client gets nothing.
//
// Fix, in two parts:
//   1. A scope: around a kill, remember which co-op player caused it. The killer's AController is an
//      explicit argument all the way down the damage path — nothing is lost, only the comparisons are
//      wrong — and UXPComponent::OwnerHealthDepleted receives it directly.
//   2. An identity shim: while that scope is active, GetPlayerController(world, 0) returns the acting
//      player's controller, so every downstream check refers to the right player. Hooking that one
//      function covers all the accessors above.
//
// The award itself cannot be impersonated, because UPlayerData is a per-process singleton — there is
// exactly one per machine. So UGameplayLib::AddXP is intercepted instead: if the acting player is
// remote, the XP is sent to that client, which applies it to its own UPlayerData.
#include "attribution.h"
#include "coop.h"
#include "players.h"
#include "console.h"
#include "log.h"
#include "hooks.h"
#include <windows.h>
#include <cstdlib>

using namespace ue;
using es2coop::Format;

namespace attribution {

using Fn_GetPlayerController = APlayerController* (*)(const UObject* wco, int index);
using Fn_AddXP = bool (*)(float xp, bool hideAnim, bool ignoreCap, float delay);
using Fn_OwnerHealthDepleted = void (*)(void* self, AActor* owner, AActor* causer, void* controller);

static Fn_GetPlayerController o_GetPlayerController = nullptr;
static Fn_AddXP o_AddXP = nullptr;
static Fn_OwnerHealthDepleted o_OwnerHealthDepleted = nullptr;

static bool g_enabled = true;
static players::Player* g_acting = nullptr;      // game thread only
static uint64_t g_shimmed = 0, g_redirected = 0, g_localAwards = 0;
static float g_redirectedXP = 0;

struct ActingScope {
    players::Player* prev;
    explicit ActingScope(players::Player* p) : prev(g_acting) { g_acting = p; }
    ~ActingScope() { g_acting = prev; }
};

// ---------------------------------------------------------------- identity shim
static APlayerController* H_GetPlayerController(const UObject* wco, int index) {
    if (g_enabled && g_acting && index == 0 && g_acting->pc) {
        ++g_shimmed;
        return g_acting->pc;
    }
    return o_GetPlayerController(wco, index);
}

// ---------------------------------------------------------------- award redirect
static bool H_AddXP(float xp, bool hideAnim, bool ignoreCap, float delay) {
    // Not every AddXP inside a kill scope is the kill's: a mission payout completed by that kill is the
    // HOST's own grant (world_state marks it and broadcasts the clients' share separately).
    if (g_enabled && g_acting && !g_acting->local && g_acting->pc && xp > 0.f && !coop::InLocalAward()) {
        // A remote player earned this: it must land in THEIR UPlayerData, not ours.
        ++g_redirected;
        g_redirectedXP += xp;
        coop::SendToClient(g_acting->pc, Format("XP|%.2f", xp));
        LOGF("[attribution] %.1f XP redirected to player %d", xp, g_acting->id);
        return true;                       // do not award locally
    }
    ++g_localAwards;
    return o_AddXP(xp, hideAnim, ignoreCap, delay);
}

// ---------------------------------------------------------------- kill scope
static void H_OwnerHealthDepleted(void* self, AActor* owner, AActor* causer, void* controller) {
    players::Player* actor = nullptr;
    if (g_enabled && coop::CurrentRole() == coop::Role::Host && controller)
        actor = players::ByController((APlayerController*)controller);
    if (actor) {
        ActingScope scope(actor);
        o_OwnerHealthDepleted(self, owner, causer, controller);
    } else {
        o_OwnerHealthDepleted(self, owner, causer, controller);
    }
}

// ---------------------------------------------------------------- commands
static void CmdAttribution(const console::Args& a, std::string& out) {
    if (a.size() > 2 && a[1] == "on") g_enabled = a[2] == "1";
    if (a.size() > 1 && a[1] == "off") g_enabled = false;
    out += Format("attribution=%d | identity shims=%llu | XP redirected=%llu (%.1f) | XP kept local=%llu\n",
                  (int)g_enabled, (unsigned long long)g_shimmed,
                  (unsigned long long)g_redirected, g_redirectedXP, (unsigned long long)g_localAwards);
    out += Format("acting player right now: %s\n", g_acting ? Format("p%d", g_acting->id).c_str() : "(none)");
}

void Register() {
    console::Register("attribution", "attribution [on 0/1|off] - credit kills/XP to the player who earned them", CmdAttribution);
}
void OnInit() {
    hooks::Install("UGameplayStatics::GetPlayerController", es2rva::UGameplayStatics_GetPlayerController,
                   (void*)&H_GetPlayerController, (void**)&o_GetPlayerController);
    hooks::Install("UGameplayLib::AddXP", es2rva::UGameplayLib_AddXP, (void*)&H_AddXP, (void**)&o_AddXP);
    hooks::Install("UXPComponent::OwnerHealthDepleted(attribution)", es2rva::UXPComponent_OwnerHealthDepleted,
                   (void*)&H_OwnerHealthDepleted, (void**)&o_OwnerHealthDepleted);
}
}
