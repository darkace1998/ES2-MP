// Co-op death handling.
//
// In single player, losing your ship is the end of the run: the controller is handed a
// BP_Pawn_GameOver_C and the game-over flow takes over. In co-op that must not stop everyone else, so
// the host watches every player's pawn and, a few seconds after one of them stops flying a ship,
// puts them back in one — with their own loadout, because the loadout substitution is keyed on the
// player id and runs again automatically on the respawn.
//
// The respawn itself is the sequence already proven for the loadout swap:
//   AController::UnPossess -> AGameModeBase::RestartPlayer -> destroy the old pawn
// (RestartPlayerAtTransform only spawns when the controller has no pawn, hence the UnPossess.)
#include "respawn.h"
#include "coop.h"
#include "players.h"
#include "console.h"
#include "log.h"
#include "hooks.h"
#include <windows.h>
#include <map>
#include <cstdlib>

using namespace ue;
using es2coop::Format;

namespace respawn {

using Fn_UnPossess = void (*)(void* controller);
using Fn_RestartPlayer = void (*)(void* gameMode, void* controller);
using Fn_Possess = void (*)(void* controller, AActor* pawn);
using Fn_PCVoid = void (*)(void* pc);
using Fn_LoadGame = void (*)(const UObject* wco, const void* saveName, int userIndex);

static Fn_Possess o_Possess = nullptr;
static Fn_PCVoid o_ReturnToMainMenu = nullptr;
static Fn_LoadGame o_LoadGame = nullptr;
static uint64_t g_vetoed = 0, g_blockedExits = 0;
static bool g_vetoGameOverPawn = true;
static bool g_blockSessionExits = true;

static bool g_enabled = true;
static double g_delay = 5.0;
static double g_now = 0;
static uint64_t g_respawns = 0;

struct DeathState { double deadSince = 0; bool pending = false; };
static std::map<int, DeathState> g_state;

// A player counts as "flying" when their pawn is an ESPawn with hull left.
static bool IsFlying(AActor* pawn) {
    if (!pawn || !IsValidObject((UObject*)pawn)) return false;
    UClass* esPawn = FindClass("/Script/ES2.ESPawn");
    if (!esPawn || !IsA((UObject*)pawn, esPawn)) return false;      // BP_Pawn_GameOver_C etc.
    // hull ratio, if the pawn has a health component
    UClass* hc = FindClass("HealthComponent");
    if (hc) {
        for (auto& p : GetProperties((UStruct*)GetClass((UObject*)pawn), true)) {
            if (p.TypeName != "ObjectProperty") continue;
            UObject* v = UE_FIELD(UObject*, pawn, p.Offset);
            if (v && IsValidObject(v) && IsA(v, hc))
                return UE_FIELD(float, v, es2off::UHealthComponent::HitpointRatio) > 0.f;
        }
    }
    return true;
}

// Hard veto: never let a co-op player's controller possess anything that is not a ship. That is what
// puts the player into BP_Pawn_GameOver_C and starts the single-player game-over flow.
static void H_Possess(void* controller, AActor* pawn) {
    if (g_enabled && g_vetoGameOverPawn && coop::CurrentRole() == coop::Role::Host && controller && pawn) {
        players::Player* pl = players::ByController((APlayerController*)controller);
        UClass* esPawn = FindClass("/Script/ES2.ESPawn");
        if (pl && esPawn && !IsA((UObject*)pawn, esPawn)) {
            ++g_vetoed;
            LOGF("[respawn] vetoed possession of %s by player %d (not a ship) — respawning instead",
                 GetName((UObject*)pawn).c_str(), pl->id);
            g_state[pl->id].deadSince = g_now;
            g_state[pl->id].pending = true;
            return;                                   // do NOT call the original
        }
    }
    o_Possess(controller, pawn);
}

// While a co-op session is live these two end it for everyone.
static void H_ReturnToMainMenu(void* pc) {
    if (g_blockSessionExits && coop::CurrentRole() != coop::Role::None) {
        ++g_blockedExits;
        LOGF("[respawn] blocked ReturnToMainMenu during a co-op session");
        return;
    }
    o_ReturnToMainMenu(pc);
}
static void H_LoadGame(const UObject* wco, const void* saveName, int userIndex) {
    if (g_blockSessionExits && coop::CurrentRole() != coop::Role::None) {
        ++g_blockedExits;
        LOGF("[respawn] blocked LoadGame during a co-op session (it would end it for everyone)");
        return;
    }
    o_LoadGame(wco, saveName, userIndex);
}

// Restore hull/armour/shield through the engine's own setter so the HUD follows (it broadcasts
// OnHealthChanged); writing HitpointRatio directly leaves the UI stale.
static void RestoreHitpoints(AActor* pawn) {
    if (!pawn) return;
    for (const char* cls : {"HealthComponent", "ArmorComponent", "ShieldComponent"}) {
        UClass* want = FindClass(cls);
        if (!want) continue;
        for (auto& p : GetProperties((UStruct*)GetClass((UObject*)pawn), true)) {
            if (p.TypeName != "ObjectProperty") continue;
            UObject* v = UE_FIELD(UObject*, pawn, p.Offset);
            if (!v || !IsValidObject(v) || !IsA(v, want)) continue;
            UFunction* fn = FindFunction(v, "SetCurrentHitpointsWithRatio");
            if (fn) { float ratio = 1.f; ProcessEvent(v, fn, &ratio); }
            else UE_FIELD(float, v, es2off::UHealthComponent::HitpointRatio) = 1.f;
            break;
        }
    }
}

static void DoRespawn(players::Player* pl) {
    UWorld* w = GetWorld();
    AGameModeBase* gm = GetGameMode(w);
    if (!gm || !pl || !pl->pc) return;
    AActor* oldPawn = UE_FIELD(AActor*, pl->pc, es2off::AController::Pawn);
    LOGF("[respawn] player %d: putting them back in a ship (old pawn %s)", pl->id, GetName((UObject*)oldPawn).c_str());
    if (oldPawn) Rva<std::remove_pointer_t<Fn_UnPossess>>(es2rva::AController_UnPossess)(pl->pc);
    Rva<std::remove_pointer_t<Fn_RestartPlayer>>(es2rva::AGameModeBase_RestartPlayer)(gm, pl->pc);
    AActor* newPawn = UE_FIELD(AActor*, pl->pc, es2off::AController::Pawn);
    if (newPawn && newPawn != oldPawn) {
        if (oldPawn && IsValidObject((UObject*)oldPawn)) {
            UFunction* destroy = FindFunction((UObject*)oldPawn, "K2_DestroyActor");
            if (destroy) ProcessEvent((UObject*)oldPawn, destroy, nullptr);
        }
        RestoreHitpoints(newPawn);
        ++g_respawns;
        LOGF("[respawn] player %d is flying %s again", pl->id, GetName((UObject*)newPawn).c_str());
        if (!pl->local && pl->pc) coop::SendToClient(pl->pc, "RESPAWNED|1");
    } else {
        LOGF("[respawn] player %d: RestartPlayer produced no new pawn (pawn=%s)", pl->id, GetName((UObject*)newPawn).c_str());
    }
}

void Tick(float dt, bool isHost) {
    g_now += dt;
    if (!g_enabled || !isHost) return;
    for (auto* pl : players::All()) {
        DeathState& st = g_state[pl->id];
        if (IsFlying(pl->pawn)) { st.deadSince = 0; st.pending = false; continue; }
        if (st.deadSince == 0) {
            st.deadSince = g_now;
            st.pending = true;
            LOGF("[respawn] player %d stopped flying (pawn=%s); respawning in %.0fs",
                 pl->id, GetName((UObject*)pl->pawn).c_str(), g_delay);
            continue;
        }
        if (st.pending && g_now - st.deadSince >= g_delay) {
            st.pending = false;
            DoRespawn(pl);
        }
    }
}

bool OnClientOp(const std::string& op, const std::string&) {
    if (op != "RESPAWNED") return false;
    LOGF("[respawn] the host put us back in a ship");
    return true;
}

static void CmdRespawn(const console::Args& a, std::string& out) {
    if (a.size() > 2 && a[1] == "on")    g_enabled = a[2] == "1";
    if (a.size() > 1 && a[1] == "off")   g_enabled = false;
    if (a.size() > 2 && a[1] == "delay") g_delay = atof(a[2].c_str());
    if (a.size() > 2 && a[1] == "now") {
        players::Player* pl = players::ById(atoi(a[2].c_str()));
        if (pl) { DoRespawn(pl); out += "respawned\n"; } else out += "no such player\n";
    }
    if (a.size() > 2 && a[1] == "veto") g_vetoGameOverPawn = a[2] == "1";
    if (a.size() > 2 && a[1] == "blockexits") g_blockSessionExits = a[2] == "1";
    out += Format("respawn=%d delay=%.0fs respawns=%llu vetoGameOverPawn=%d(%llu vetoed) blockSessionExits=%d(%llu blocked)\n",
                  (int)g_enabled, g_delay, (unsigned long long)g_respawns,
                  (int)g_vetoGameOverPawn, (unsigned long long)g_vetoed,
                  (int)g_blockSessionExits, (unsigned long long)g_blockedExits);
    for (auto* pl : players::All())
        out += Format("  p%d %-22s flying=%d\n", pl->id, GetName((UObject*)pl->pawn).c_str(), (int)IsFlying(pl->pawn));
}

void Register() {
    console::Register("respawn", "respawn [on 0/1|off|delay N|now <playerId>] - co-op death handling", CmdRespawn);
}
void OnInit() {
    hooks::Install("AController::Possess", es2rva::AController_Possess, (void*)&H_Possess, (void**)&o_Possess);
    hooks::Install("AESPlayerController::ReturnToMainMenu", es2rva::AESPlayerController_ReturnToMainMenu,
                   (void*)&H_ReturnToMainMenu, (void**)&o_ReturnToMainMenu);
    hooks::Install("UUserFunctionsLib::LoadGame", es2rva::UUserFunctionsLib_LoadGame, (void*)&H_LoadGame, (void**)&o_LoadGame);
}
}
