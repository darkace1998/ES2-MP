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
// Per pawn CLASS: the offset of the property holding its HealthComponent (-1 = none). IsFlying runs for
// every player every frame, and walking the whole reflection chain (two engine FName::ToString per
// property across five classes) each time was several hundred allocations per player per frame.
static std::map<UClass*, int32_t> g_healthPropOffset;

void OnPlayerLeft(int id) { g_state.erase(id); }
void Reset() { g_state.clear(); g_healthPropOffset.clear(); }

// ES2 hands the controller a BP_Pawn_GameOver_C when the run ends. That — not "anything that is not a
// ship" — is what death looks like.
static bool IsGameOverPawn(AActor* pawn) {
    return ue::GetObjectClassName((UObject*)pawn).find("GameOver") != std::string::npos;
}

// A player counts as "flying" when their pawn is an ESPawn with hull left.
//
// Anything else that is NOT the game-over pawn is some other legitimate state — above all DOCKING,
// which travels to a station/cinematic map and possesses a DefaultPawn. Treating that as death made
// this watchdog respawn every player every 5 s for as long as they stayed docked, leaking a fresh
// DefaultPawn per cycle (observed: player 0 and player 1 looping in Cinematics). Only a missing pawn or
// the game-over pawn should bring us in.
static bool IsFlying(AActor* pawn) {
    if (!pawn || !IsValidObject((UObject*)pawn)) return false;
    if (IsGameOverPawn(pawn)) return false;
    UClass* esPawn = FindClass("/Script/ES2.ESPawn");
    if (!esPawn || !IsA((UObject*)pawn, esPawn)) return true;       // docked / cinematic: not our business
    // hull ratio, if the pawn has a health component
    UClass* hc = FindClass("HealthComponent");
    if (!hc) return true;
    UClass* cls = GetClass((UObject*)pawn);
    auto it = g_healthPropOffset.find(cls);
    if (it == g_healthPropOffset.end() || !IsValidObject((UObject*)cls)) {
        int32_t off = -1;
        for (auto& p : GetProperties((UStruct*)cls, true)) {
            if (p.TypeName != "ObjectProperty") continue;
            UObject* v = UE_FIELD(UObject*, pawn, p.Offset);
            if (v && IsValidObject(v) && IsA(v, hc)) { off = p.Offset; break; }
        }
        it = g_healthPropOffset.emplace(cls, off).first;
    }
    if (it->second < 0) return true;
    UObject* v = UE_FIELD(UObject*, pawn, it->second);
    if (!v || !IsValidObject(v) || !IsA(v, hc)) { g_healthPropOffset.erase(it); return true; }   // layout changed: relearn next time
    return UE_FIELD(float, v, es2off::UHealthComponent::HitpointRatio) > 0.f;
}

// Hard veto: never let a co-op player's controller be put into the GAME-OVER pawn, which is what starts
// the single-player game-over flow.
//
// This used to veto anything that was not a ship, which also caught the DefaultPawn a station or
// cinematic map possesses — i.e. it fought docking, and combined with the respawn watchdog above it
// produced an endless possess/veto/respawn loop.
static void H_Possess(void* controller, AActor* pawn) {
    if (g_enabled && g_vetoGameOverPawn && coop::CurrentRole() == coop::Role::Host && controller && pawn) {
        players::Player* pl = players::ByController((APlayerController*)controller);
        if (pl && IsGameOverPawn(pawn)) {
            ++g_vetoed;
            LOGF("[respawn] vetoed the game-over pawn %s for player %d — respawning instead",
                 GetName((UObject*)pawn).c_str(), pl->id);
            g_state[pl->id].deadSince = g_now;
            g_state[pl->id].pending = true;
            return;                                   // do NOT call the original
        }
    }
    o_Possess(controller, pawn);
}

// While a co-op session is live, the HOST leaving ends it for everyone -- so the host's exits are
// blocked. A client returning to the menu only drops that client, which is its call to make (and the
// only way it has to leave at all; solo docking already client-travels out of the session the same way).
static void H_ReturnToMainMenu(void* pc) {
    if (g_blockSessionExits && coop::CurrentRole() == coop::Role::Host) {
        ++g_blockedExits;
        LOGF("[respawn] blocked ReturnToMainMenu during a co-op session (host)");
        return;
    }
    o_ReturnToMainMenu(pc);
}
// LoadGame stays blocked on both sides: on a client it would swap the UPlayerData the replicated pawn
// and every mirrored record are built from, mid-session.
static void H_LoadGame(const UObject* wco, const void* saveName, int userIndex) {
    if (g_blockSessionExits && coop::CurrentRole() != coop::Role::None) {
        ++g_blockedExits;
        LOGF("[respawn] blocked LoadGame during a co-op session (it would end it for everyone)");
        return;
    }
    coop::OnSaveLoad();      // a different save: per-save caches (mission snapshot) must not carry over
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

// The registry's cached pawn lags behind a possess and is null for a moment while a joining client's
// pawn is being substituted — which made this watchdog announce "player N stopped flying (pawn=null)"
// and fire a pointless RestartPlayer five seconds into every join. The controller's own Pawn field is
// always current; the cache is only a fallback for a player whose controller has gone away.
static AActor* LivePawn(players::Player* pl) {
    if (pl->pc && IsValidObject((UObject*)pl->pc))
        return UE_FIELD(AActor*, pl->pc, es2off::AController::Pawn);
    return pl->pawn;
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
        AActor* pawn = LivePawn(pl);
        if (IsFlying(pawn)) { st.deadSince = 0; st.pending = false; continue; }
        if (st.deadSince == 0) {
            st.deadSince = g_now;
            st.pending = true;
            LOGF("[respawn] player %d stopped flying (pawn=%s); respawning in %.0fs",
                 pl->id, GetName((UObject*)pawn).c_str(), g_delay);
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
        out += Format("  p%d %-22s flying=%d\n", pl->id, GetName((UObject*)LivePawn(pl)).c_str(), (int)IsFlying(LivePawn(pl)));
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
