// Shared world state for co-op: mission/objective progress, dialog lines, and kill XP.
//
// Architecture (derived from the PDB, see docs/research/05-missions-loot.md):
//
//  * Mission actors and all mission Blueprint logic only ever exist on the HOST, because AESGameModeBase
//    is null on a network client. There is nothing to suppress client-side.
//  * Mission state does not live in those actors: it lives in UPlayerData::MissionSaveState, an array of
//    FTaskSaveGameData. Each machine has its own UPlayerData (it is a per-process singleton).
//  * Every mutation of that state funnels through UMissionLib::UpdateTaskInPlayerData, so one host-side
//    hook observes all mission progress.
//  * The client's mission log and world indicators are 100% data-driven from its own UPlayerData, so
//    mirroring the records and then calling UMapLib::RefreshMissionAndWaypointIndicators is enough — no
//    mission actor has to exist on the client.
//  * Dialog funnels through UDialogManager::EnqueueDialog; XP-on-kill through UXPComponent::OwnerHealthDepleted.
#include "world_state.h"
#include "coop.h"
#include "players.h"
#include "console.h"
#include "log.h"
#include "hooks.h"
#include <windows.h>
#include <map>
#include <string>
#include <cstdlib>
#include <cstdio>

using namespace ue;
using es2coop::Format;

namespace world_state {

// ---------------------------------------------------------------- engine bindings
using Fn_UpdateTaskInPlayerData = void (*)(void* task, bool a, bool b);
using Fn_FindTaskInPlayerData   = void* (*)(FName taskId);                       // -> FTaskSaveGameData*
using Fn_RefreshIndicators      = void (*)();
using Fn_EnqueueDialogMember    = bool (*)(void* self, FName dialogId, const void* finishedDelegate, float delay, int type, int behavior, bool playOnce);
using Fn_EnqueueDialogStatic    = void (*)(bool* outOk, FName dialogId, float delay, int type, int behavior, bool playOnce);
using Fn_AddXP                  = bool (*)(float xp, bool a, bool b, float c);
using Fn_OwnerHealthDepleted    = void (*)(void* self, AActor* a, AActor* b, void* controller);

static Fn_UpdateTaskInPlayerData o_UpdateTask = nullptr;
static Fn_EnqueueDialogMember    o_EnqueueDialog = nullptr;
static Fn_OwnerHealthDepleted    o_OwnerHealthDepleted = nullptr;

// ---------------------------------------------------------------- state
struct TaskSnapshot { int state = -1, stage = -1, progress = -1; std::string loc, station; };
static std::map<std::string, TaskSnapshot> g_taskCache;     // host: last broadcast value per task id

static bool g_syncMissions = true;
static bool g_syncDialog = true;
static bool g_syncXP = true;
static bool g_applying = false;          // re-entrancy guard for our own writes
static uint64_t g_taskSent = 0, g_taskApplied = 0, g_dlgSent = 0, g_dlgApplied = 0, g_xpSent = 0;
static float g_xpApplied = 0;
static bool g_indicatorsDirty = false;
static double g_refreshAccum = 0;

// ---------------------------------------------------------------- mission mirroring
static void BroadcastTask(void* task) {
    if (!task) return;
    std::string id = UE_FIELD(FName, task, es2off::AMissionTaskBase::MissionTaskID).ToString();
    if (id.empty() || id == "None") return;
    TaskSnapshot now;
    now.state    = (int)UE_FIELD(uint8_t, task, es2off::AMissionTaskBase::TaskState);
    now.stage    = UE_FIELD(int32_t, task, es2off::AMissionTaskBase::StageValue);
    now.progress = UE_FIELD(int32_t, task, es2off::AMissionTaskBase::ProgressValue);
    now.loc      = UE_FIELD(FName, task, es2off::AMissionTaskBase::LocationID).ToString();
    now.station  = UE_FIELD(FName, task, es2off::AMissionTaskBase::StationID).ToString();

    auto it = g_taskCache.find(id);
    if (it != g_taskCache.end()) {
        const TaskSnapshot& p = it->second;
        if (p.state == now.state && p.stage == now.stage && p.progress == now.progress &&
            p.loc == now.loc && p.station == now.station) return;      // nothing changed: stay quiet
    }
    g_taskCache[id] = now;
    ++g_taskSent;
    coop::SendToAllClients(Format("MT|%s|%d|%d|%d|%s|%s", id.c_str(), now.state, now.stage, now.progress,
                                  now.loc.c_str(), now.station.c_str()));
}

static void H_UpdateTaskInPlayerData(void* task, bool a, bool b) {
    o_UpdateTask(task, a, b);
    if (!g_syncMissions || g_applying) return;
    if (coop::CurrentRole() != coop::Role::Host) return;
    if (!IsValidObject((UObject*)task)) return;
    BroadcastTask(task);
}

// Client: write the mirrored values into our own UPlayerData record for that task.
static bool ApplyTask(const std::string& body) {
    char id[128] = {0}, loc[128] = {0}, station[128] = {0};
    int state = 0, stage = 0, progress = 0;
    // MT|<taskId>|<state>|<stage>|<progress>|<locationId>|<stationId>
    if (sscanf(body.c_str(), "%127[^|]|%d|%d|%d|%127[^|]|%127[^|]", id, &state, &stage, &progress, loc, station) < 4)
        return false;
    FName taskId = FName::Make(std::string(id));
    void* rec = Rva<std::remove_pointer_t<Fn_FindTaskInPlayerData>>(es2rva::UMissionLib_FindTaskInPlayerData)(taskId);
    if (!rec) {
        // The client has no record for this task. That happens when the two players' saves differ; a full
        // snapshot on join covers the common case, and creating records from scratch would mean
        // constructing FTaskSaveGameData (320 bytes with TArray/TMap members) — deliberately not done here.
        static int warned = 0;
        if (warned++ < 10) LOGF("[world] no local record for task '%s' (state %d) — skipped", id, state);
        return true;
    }
    g_applying = true;
    UE_FIELD(uint8_t, rec, es2off::FTaskSaveGameData::TaskState) = (uint8_t)state;
    UE_FIELD(int32_t, rec, es2off::FTaskSaveGameData::Stage) = stage;
    UE_FIELD(int32_t, rec, es2off::FTaskSaveGameData::Progress) = progress;
    if (loc[0])     UE_FIELD(FName, rec, es2off::FTaskSaveGameData::LocationID) = FName::Make(std::string(loc));
    if (station[0]) UE_FIELD(FName, rec, es2off::FTaskSaveGameData::StationID) = FName::Make(std::string(station));
    g_applying = false;
    ++g_taskApplied;
    g_indicatorsDirty = true;
    return true;
}

// ---------------------------------------------------------------- dialog
static bool H_EnqueueDialog(void* self, FName dialogId, const void* finishedDelegate, float delay,
                            int type, int behavior, bool playOnce) {
    bool r = o_EnqueueDialog(self, dialogId, finishedDelegate, delay, type, behavior, playOnce);
    if (g_syncDialog && !g_applying && coop::CurrentRole() == coop::Role::Host && r) {
        std::string id = dialogId.ToString();
        if (!id.empty() && id != "None") {
            ++g_dlgSent;
            coop::SendToAllClients(Format("DLG|%s|%.3f|%d|%d|%d", id.c_str(), delay, type, behavior, (int)playOnce));
        }
    }
    return r;
}

static bool ApplyDialog(const std::string& body) {
    char id[128] = {0};
    float delay = 0; int type = 0, behavior = 0, playOnce = 0;
    if (sscanf(body.c_str(), "%127[^|]|%f|%d|%d|%d", id, &delay, &type, &behavior, &playOnce) < 3) return false;
    bool ok = false;
    g_applying = true;
    Rva<std::remove_pointer_t<Fn_EnqueueDialogStatic>>(es2rva::UDialogManager_EnqueueDialog_Static)(
        &ok, FName::Make(std::string(id)), delay, type, behavior, playOnce != 0);
    g_applying = false;
    ++g_dlgApplied;
    return true;
}

// ---------------------------------------------------------------- kill XP
static void H_OwnerHealthDepleted(void* self, AActor* a, AActor* b, void* controller) {
    o_OwnerHealthDepleted(self, a, b, controller);
    if (!g_syncXP || coop::CurrentRole() != coop::Role::Host) return;
    float xp = self ? UE_FIELD(float, self, es2off::UXPComponent::XP) : 0.f;
    if (xp > 0.f) {
        ++g_xpSent;
        coop::SendToAllClients(Format("XP|%.2f", xp));
    }
}

static bool ApplyXP(const std::string& body) {
    float xp = (float)atof(body.c_str());
    if (xp <= 0.f) return true;
    Rva<std::remove_pointer_t<Fn_AddXP>>(es2rva::UGameplayLib_AddXP)(xp, false, false, 0.f);
    g_xpApplied += xp;
    return true;
}

// ---------------------------------------------------------------- snapshot on join
void OnPlayerJoined(APlayerController* pc) {
    if (!g_syncMissions || coop::CurrentRole() != coop::Role::Host || !pc) return;
    // Re-send every task we know about so a joiner is consistent, not just up to date from now on.
    int n = 0;
    for (auto& [id, s] : g_taskCache) {
        coop::SendToClient(pc, Format("MT|%s|%d|%d|%d|%s|%s", id.c_str(), s.state, s.stage, s.progress,
                                      s.loc.c_str(), s.station.c_str()));
        ++n;
    }
    LOGF("[world] sent %d cached mission task(s) to joiner %s", n, GetName((UObject*)pc).c_str());
}

// ---------------------------------------------------------------- dispatch
bool OnServerOp(APlayerController*, const std::string&, const std::string&) { return false; }

bool OnClientOp(const std::string& op, const std::string& body) {
    if (op == "MT")  return ApplyTask(body);
    if (op == "DLG") return ApplyDialog(body);
    if (op == "XP")  return ApplyXP(body);
    return false;
}

void Tick(float dt, bool isHost) {
    if (isHost || !g_indicatorsDirty) return;
    // Refreshing walks every registered marker and does path-finding, so coalesce it.
    g_refreshAccum += dt;
    if (g_refreshAccum < 1.0) return;
    g_refreshAccum = 0;
    g_indicatorsDirty = false;
    Rva<std::remove_pointer_t<Fn_RefreshIndicators>>(es2rva::UMapLib_RefreshMissionAndWaypointIndicators)();
}

// ---------------------------------------------------------------- commands
static void CmdWorld(const console::Args& a, std::string& out) {
    if (a.size() > 2 && a[1] == "missions") g_syncMissions = a[2] == "1";
    if (a.size() > 2 && a[1] == "dialog")   g_syncDialog = a[2] == "1";
    if (a.size() > 2 && a[1] == "xp")       g_syncXP = a[2] == "1";
    if (a.size() > 1 && a[1] == "refresh")  { Rva<std::remove_pointer_t<Fn_RefreshIndicators>>(es2rva::UMapLib_RefreshMissionAndWaypointIndicators)(); out += "indicators refreshed\n"; }
    out += Format("missions=%d dialog=%d xp=%d | sent: tasks=%llu dialog=%llu xp=%llu | applied: tasks=%llu dialog=%llu xp=%.1f\n",
                  (int)g_syncMissions, (int)g_syncDialog, (int)g_syncXP,
                  (unsigned long long)g_taskSent, (unsigned long long)g_dlgSent, (unsigned long long)g_xpSent,
                  (unsigned long long)g_taskApplied, (unsigned long long)g_dlgApplied, g_xpApplied);
    out += Format("cached tasks: %d\n", (int)g_taskCache.size());
    int n = 0;
    for (auto& [id, s] : g_taskCache) {
        if (n++ >= 20) { out += "  ...\n"; break; }
        out += Format("  %-28s state=%d stage=%d progress=%d loc=%s\n", id.c_str(), s.state, s.stage, s.progress, s.loc.c_str());
    }
}

// Inspect this machine's own mission records (works on host and client).
static void CmdMissions(const console::Args& a, std::string& out) {
    std::string filter = a.size() > 1 ? a[1] : "";
    UClass* c = FindClass("/Script/ES2.MissionTaskBase");
    if (c) {
        auto actors = GetAllActorsOfClass(GetWorld(), c);
        out += Format("mission task actors in world: %d\n", (int)actors.size());
        for (AActor* act : actors) {
            std::string id = UE_FIELD(FName, act, es2off::AMissionTaskBase::MissionTaskID).ToString();
            if (!filter.empty() && id.find(filter) == std::string::npos) continue;
            out += Format("  %-28s state=%d stage=%d progress=%d\n", id.c_str(),
                          (int)UE_FIELD(uint8_t, act, es2off::AMissionTaskBase::TaskState),
                          UE_FIELD(int32_t, act, es2off::AMissionTaskBase::StageValue),
                          UE_FIELD(int32_t, act, es2off::AMissionTaskBase::ProgressValue));
        }
    }
    // and the records in our own UPlayerData, which is what the mission log actually reads
    if (!filter.empty()) {
        void* rec = Rva<std::remove_pointer_t<Fn_FindTaskInPlayerData>>(es2rva::UMissionLib_FindTaskInPlayerData)(FName::Make(filter));
        out += rec ? Format("PlayerData record '%s': state=%d stage=%d progress=%d loc=%s\n", filter.c_str(),
                            (int)UE_FIELD(uint8_t, rec, es2off::FTaskSaveGameData::TaskState),
                            UE_FIELD(int32_t, rec, es2off::FTaskSaveGameData::Stage),
                            UE_FIELD(int32_t, rec, es2off::FTaskSaveGameData::Progress),
                            UE_FIELD(FName, rec, es2off::FTaskSaveGameData::LocationID).ToString().c_str())
                  : Format("PlayerData record '%s': not found\n", filter.c_str());
    }
}

void Register() {
    console::Register("world", "world [missions 0/1|dialog 0/1|xp 0/1|refresh] - shared world-state sync status", CmdWorld);
    console::Register("missions", "missions [taskIdFilter] - mission task actors and this machine's PlayerData records", CmdMissions);
}

void OnInit() {
    hooks::Install("UMissionLib::UpdateTaskInPlayerData", es2rva::UMissionLib_UpdateTaskInPlayerData,
                   (void*)&H_UpdateTaskInPlayerData, (void**)&o_UpdateTask);
    hooks::Install("UDialogManager::EnqueueDialog", es2rva::UDialogManager_EnqueueDialog_Member,
                   (void*)&H_EnqueueDialog, (void**)&o_EnqueueDialog);
    hooks::Install("UXPComponent::OwnerHealthDepleted", es2rva::UXPComponent_OwnerHealthDepleted,
                   (void*)&H_OwnerHealthDepleted, (void**)&o_OwnerHealthDepleted);
}
}
