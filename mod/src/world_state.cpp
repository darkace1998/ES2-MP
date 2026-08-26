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
#include <vector>
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
using Fn_OnMissionCompleted     = void (*)(void* playerData, void* mission);
using Fn_ChangeTrackedMission   = void (*)(FName missionId, bool track);
using Fn_GetPlayerData          = UObject* (*)();
using Fn_DialogGetSingleton     = UObject* (*)(bool bMenu);
using Fn_AddNonItemRewards      = void (*)(const void* rewards, const void* missionType, const void* factionGroup, const void* name);
using Fn_ChangeCredits          = void (*)(int delta, int transferType, bool a, bool b);
using Fn_IncJobScore            = void (*)(int amount);
using Fn_AddItemToInventory     = bool (*)(UObject* item, bool b);

static Fn_UpdateTaskInPlayerData o_UpdateTask = nullptr;
static Fn_EnqueueDialogMember    o_EnqueueDialog = nullptr;
static Fn_OnMissionCompleted     o_OnMissionCompleted = nullptr;
static Fn_ChangeTrackedMission   o_ChangeTrackedMission = nullptr;
static Fn_AddNonItemRewards      o_AddNonItemRewards = nullptr;
static Fn_AddItemToInventory     o_AddItemToInventory = nullptr;

// ---------------------------------------------------------------- state
struct TaskSnapshot { int state = -1, stage = -1, progress = -1; uint64_t loc = 0, station = 0; std::string id, locStr, stationStr; };
// Keyed by the FName's raw 64-bit value: this hook runs on the per-frame mission paths, so the hot path
// must not build a std::string on every call.
static std::map<uint64_t, TaskSnapshot> g_taskCache;
static inline uint64_t NameKey(const FName& n) { return ((uint64_t)n.ComparisonIndex << 32) | n.Number; }

static bool g_syncMissions = true;
static bool g_syncDialog = true;
static bool g_syncXP = true;
static bool g_syncRewards = true;
static bool g_logItems = false;      // observability only: who grants items, and on which machine
static uint64_t g_rewardSent = 0, g_rewardApplied = 0;
static bool g_applying = false;          // re-entrancy guard for our own writes
static uint64_t g_taskSent = 0, g_taskApplied = 0, g_dlgSent = 0, g_dlgApplied = 0;
static float g_xpApplied = 0;
static bool g_indicatorsDirty = false;
static double g_refreshAccum = 0;
// Join snapshots, paced: every entry is one reliable ClientMessage bunch on the joiner's controller
// channel, and UE drops a connection whose channel has more than 256 reliable bunches in flight. A long
// host session can hold more cached tasks than that, and the join already puts WELCOME/ROSTER/SD on
// the same channel in the same frames -- so the snapshot goes out a few per tick, like loadout's chunks.
struct JoinSnapshot { APlayerController* pc; std::vector<std::string> msgs; size_t next = 0; };
static std::vector<JoinSnapshot> g_joinQueue;
static constexpr size_t kSnapshotPerTick = 8;
static uint64_t g_snapshotSent = 0;

void ResetSession() { g_taskCache.clear(); g_joinQueue.clear(); g_indicatorsDirty = false; }

// ---------------------------------------------------------------- mission mirroring
static void BroadcastTask(void* task) {
    if (!task) return;
    FName idName = UE_FIELD(FName, task, es2off::AMissionTaskBase::MissionTaskID);
    if (idName.IsNone()) return;
    uint64_t key = NameKey(idName);
    int state    = (int)UE_FIELD(uint8_t, task, es2off::AMissionTaskBase::TaskState);
    int stage    = UE_FIELD(int32_t, task, es2off::AMissionTaskBase::StageValue);
    int progress = UE_FIELD(int32_t, task, es2off::AMissionTaskBase::ProgressValue);
    uint64_t loc = NameKey(UE_FIELD(FName, task, es2off::AMissionTaskBase::LocationID));
    uint64_t sta = NameKey(UE_FIELD(FName, task, es2off::AMissionTaskBase::StationID));

    auto it = g_taskCache.find(key);
    if (it != g_taskCache.end()) {
        const TaskSnapshot& p = it->second;
        if (p.state == state && p.stage == stage && p.progress == progress && p.loc == loc && p.station == sta)
            return;                                    // nothing changed: no allocation, no message
    }
    // Something changed: only now is it worth resolving the names to strings.
    TaskSnapshot now;
    now.state = state; now.stage = stage; now.progress = progress; now.loc = loc; now.station = sta;
    now.id = idName.ToString();
    now.locStr = UE_FIELD(FName, task, es2off::AMissionTaskBase::LocationID).ToString();
    now.stationStr = UE_FIELD(FName, task, es2off::AMissionTaskBase::StationID).ToString();
    g_taskCache[key] = now;
    ++g_taskSent;
    coop::SendToAllClients(Format("MT|%s|%d|%d|%d|%s|%s", now.id.c_str(), state, stage, progress,
                                  now.locStr.c_str(), now.stationStr.c_str()));
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
    // MT|<taskId>|<state>|<stage>|<progress>|<locationId>|<stationId>
    // Split by hand: a `%[^|]` scanset fails on an EMPTY field, and an empty location (common) then
    // stopped sscanf before the station, which was silently never applied.
    std::vector<std::string> f;
    for (size_t pos = 0;;) {
        size_t bar = body.find('|', pos);
        f.push_back(body.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos));
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    if (f.size() < 4 || f[0].empty()) return false;
    const std::string& id = f[0];
    int state = atoi(f[1].c_str()), stage = atoi(f[2].c_str()), progress = atoi(f[3].c_str());
    const std::string loc = f.size() > 4 ? f[4] : "", station = f.size() > 5 ? f[5] : "";
    FName taskId = FName::Make(id);
    void* rec = Rva<std::remove_pointer_t<Fn_FindTaskInPlayerData>>(es2rva::UMissionLib_FindTaskInPlayerData)(taskId);
    if (!rec) {
        // The client has no record for this task. That happens when the two players' saves differ; a full
        // snapshot on join covers the common case, and creating records from scratch would mean
        // constructing FTaskSaveGameData (320 bytes with TArray/TMap members) — deliberately not done here.
        static int warned = 0;
        if (warned++ < 10) LOGF("[world] no local record for task '%s' (state %d) — skipped", id.c_str(), state);
        return true;
    }
    g_applying = true;
    UE_FIELD(uint8_t, rec, es2off::FTaskSaveGameData::TaskState) = (uint8_t)state;
    UE_FIELD(int32_t, rec, es2off::FTaskSaveGameData::Stage) = stage;
    UE_FIELD(int32_t, rec, es2off::FTaskSaveGameData::Progress) = progress;
    if (!loc.empty())     UE_FIELD(FName, rec, es2off::FTaskSaveGameData::LocationID) = FName::Make(loc);
    if (!station.empty()) UE_FIELD(FName, rec, es2off::FTaskSaveGameData::StationID) = FName::Make(station);
    g_applying = false;
    ++g_taskApplied;
    g_indicatorsDirty = true;
    return true;
}

// UPlayerData::MissionSaveState deltas do not cover CompletedMissions, which is written only here.
static void H_OnMissionCompleted(void* playerData, void* mission) {
    o_OnMissionCompleted(playerData, mission);
    if (!g_syncMissions || g_applying || coop::CurrentRole() != coop::Role::Host) return;
    if (!mission || !IsValidObject((UObject*)mission)) return;
    FName id = UE_FIELD(FName, mission, es2off::AMissionBase::MissionTaskID);
    if (id.IsNone()) return;
    coop::SendToAllClients("MC|" + id.ToString());
}

// ...nor the tracked-mission FNames, written only here. Replaying the same call on the client lets the
// game decide which of TrackedMainMission / TrackedSideMission / TrackedJob to write.
static void H_ChangeTrackedMission(FName missionId, bool track) {
    o_ChangeTrackedMission(missionId, track);
    if (!g_syncMissions || g_applying || coop::CurrentRole() != coop::Role::Host) return;
    if (missionId.IsNone()) return;
    coop::SendToAllClients(Format("TRK|%s|%d", missionId.ToString().c_str(), (int)track));
}

static bool ApplyMissionCompleted(const std::string& body) {
    UObject* pd = Rva<std::remove_pointer_t<Fn_GetPlayerData>>(es2rva::UGameplayLib_GetPlayerData)();
    if (!pd || body.empty()) return true;
    FName id = FName::Make(body);
    TArray<FName>& done = UE_FIELD(TArray<FName>, pd, es2off::UPlayerData::CompletedMissions);
    for (int i = 0; i < done.Num; ++i) if (done[i] == id) return true;      // already recorded
    g_applying = true;
    done.Add(id);
    g_applying = false;
    g_indicatorsDirty = true;
    LOGF("[world] mission '%s' recorded as completed locally (%d total)", body.c_str(), done.Num);
    return true;
}

static bool ApplyTrackedMission(const std::string& body) {
    char id[128] = {0}; int track = 1;
    if (sscanf(body.c_str(), "%127[^|]|%d", id, &track) < 1) return true;
    g_applying = true;
    Rva<std::remove_pointer_t<Fn_ChangeTrackedMission>>(es2rva::ChangeTrackedMission_Internal)(
        FName::Make(std::string(id)), track != 0);
    g_applying = false;
    g_indicatorsDirty = true;
    return true;
}

// ---------------------------------------------------------------- dialog
static bool H_EnqueueDialog(void* self, FName dialogId, const void* finishedDelegate, float delay,
                            int type, int behavior, bool playOnce) {
    bool r = o_EnqueueDialog(self, dialogId, finishedDelegate, delay, type, behavior, playOnce);
    // Menu chatter goes through a SECOND UDialogManager singleton; only mirror the in-game one.
    UObject* gameMgr = Rva<std::remove_pointer_t<Fn_DialogGetSingleton>>(es2rva::UDialogManager_GetSingleton_Bool)(false);
    if (g_syncDialog && !g_applying && coop::CurrentRole() == coop::Role::Host && r && self == gameMgr) {
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
// The award itself is owned by attribution.cpp: it scopes each kill to the player who caused it and
// redirects UGameplayLib::AddXP to that player. All that remains here is applying an incoming award.

static bool ApplyXP(const std::string& body) {
    if (!g_syncXP) return true;                 // `world xp 0` -- was set but never consulted
    float xp = (float)atof(body.c_str());
    if (xp <= 0.f) return true;
    Rva<std::remove_pointer_t<Fn_AddXP>>(es2rva::UGameplayLib_AddXP)(xp, false, false, 0.f);
    g_xpApplied += xp;
    return true;
}

// ---------------------------------------------------------------- mission rewards
//
// Only the host runs mission logic, so only the host reaches UMissionLib::AddNonItemRewards: without
// this a client finishes a mission alongside the host and is paid nothing. The grant set was read off
// the disassembly at 0x5E71518 rather than taken from docs/research/05 (which claims faction standing
// and OkkarCredits -- it grants neither):
//
//   Rewards.XP       +0x00  int32  -> AddXP.  `cvtdq2ps xmm0, xmm0` -- it is an INT, and reading it as
//                                    a float hands AddXP a denormal.
//   Rewards.Credits  +0x04  int32  -> ChangeCredits(credits, *MissionType == Job(2) ? JobReward(5)
//                                    : Undefined(0), true, false)
//   Rewards.Standing +0x0C  int32  -> IncJobScore, and only when *FactionGroup != 0
//
// (+0x08 OkkarCredits is read by nothing here.) ABI: RCX = const FMissionRewards* by hidden pointer,
// RDX / R8 = pointers to single enum bytes, R9 = const FName*.
//
// Rewards are per-player: each machine applies them to its OWN UPlayerData, which is what keeps the
// separate saves consistent. The host's own grant is the real call; the client replays the three
// primitives rather than AddNonItemRewards itself, so it never depends on having the mission's task
// record (the real function looks one up for its decal unlocks).
static void H_AddNonItemRewards(const void* rewards, const void* missionType, const void* factionGroup, const void* name) {
    // The host's own payout. If a client's killing blow completed the task, this runs INSIDE that
    // player's kill scope, where attribution redirects every AddXP to the killer -- who is about to get
    // the same XP again from the MR broadcast below, while the host got none. Mark it as ours.
    coop::PushLocalAward();
    o_AddNonItemRewards(rewards, missionType, factionGroup, name);
    coop::PopLocalAward();
    if (!g_syncRewards || g_applying || !rewards) return;
    if (coop::CurrentRole() != coop::Role::Host) return;
    const uint8_t* r = (const uint8_t*)rewards;
    const int xp = *(const int*)(r + 0x00), credits = *(const int*)(r + 0x04), standing = *(const int*)(r + 0x0C);
    const unsigned mt = missionType ? *(const uint8_t*)missionType : 0u;
    const unsigned fg = factionGroup ? *(const uint8_t*)factionGroup : 0u;
    if (!xp && !credits && !standing) return;
    coop::SendToAllClients(Format("MR|%d|%d|%d|%u|%u", xp, credits, standing, mt, fg));
    ++g_rewardSent;
    LOGF("[world] mission rewards -> clients: xp=%d credits=%d standing=%d (missionType=%u faction=%u)",
         xp, credits, standing, mt, fg);
}

// Mission ITEM rewards are not granted by any native funnel -- AddNonItemRewards deliberately skips them
// (hence the name) and the payout is Blueprint-driven at a station's claim UI. Two facts decide the
// design, and only a real mission completion can settle which applies:
//   * FTaskSaveGameData carries the whole FMissionRewards -- Items included -- at +0xD8, and this module
//     already mirrors that record to clients, so a client may already know what it is owed.
//   * With solo docking each player visits the station in their own session, so each could simply claim
//     their own copy locally, and no item would need to cross the wire at all.
// ES2 items are procedurally generated, so if a transfer IS needed it has to carry the generated item's
// state (the pattern loadout.cpp already uses for ships), not the descriptor. Until that is observed,
// this hook only watches: `world items 1` logs every inventory grant on whichever machine runs it.
static bool H_AddItemToInventory(UObject* item, bool b) {
    if (g_logItems)
        LOGF("[world] item -> inventory: %s (%s)", GetName(item).c_str(), ue::GetObjectClassName(item).c_str());
    return o_AddItemToInventory(item, b);
}

static bool ApplyRewards(const std::string& body) {
    int xp = 0, credits = 0, standing = 0; unsigned mt = 0, fg = 0;
    if (sscanf(body.c_str(), "%d|%d|%d|%u|%u", &xp, &credits, &standing, &mt, &fg) < 5) return true;
    g_applying = true;
    if (xp)      Rva<std::remove_pointer_t<Fn_AddXP>>(es2rva::UGameplayLib_AddXP)((float)xp, false, false, 0.f);
    if (credits) Rva<std::remove_pointer_t<Fn_ChangeCredits>>(es2rva::UGameplayLib_ChangeCredits)(
                     credits, mt == 2 ? 5 /*JobReward*/ : 0 /*Undefined*/, true, false);
    if (standing && fg != 0) Rva<std::remove_pointer_t<Fn_IncJobScore>>(es2rva::UMissionLib_IncJobScore)(standing);
    g_applying = false;
    ++g_rewardApplied;
    LOGF("[world] mission rewards applied: xp=%d credits=%d standing=%d", xp, credits, standing);
    return true;
}

// ---------------------------------------------------------------- snapshot on join
void OnPlayerJoined(APlayerController* pc) {
    if (!g_syncMissions || coop::CurrentRole() != coop::Role::Host || !pc) return;
    // Re-send every task we know about so a joiner is consistent, not just up to date from now on.
    for (auto& q : g_joinQueue) if (q.pc == pc) return;    // already queued for this controller
    JoinSnapshot snap; snap.pc = pc;
    for (auto& [key, s] : g_taskCache)
        snap.msgs.push_back(Format("MT|%s|%d|%d|%d|%s|%s", s.id.c_str(), s.state, s.stage, s.progress,
                                   s.locStr.c_str(), s.stationStr.c_str()));
    LOGF("[world] queued %d cached mission task(s) for joiner %s", (int)snap.msgs.size(), GetName((UObject*)pc).c_str());
    if (!snap.msgs.empty()) g_joinQueue.push_back(std::move(snap));
}

static void DrainJoinQueue() {
    if (g_joinQueue.empty()) return;
    JoinSnapshot& q = g_joinQueue.front();
    // The joiner may be gone again before its snapshot is through (docking rejoins are quick).
    if (!IsValidObject((UObject*)q.pc) || !players::ByController(q.pc)) { g_joinQueue.erase(g_joinQueue.begin()); return; }
    for (size_t n = 0; n < kSnapshotPerTick && q.next < q.msgs.size(); ++n, ++q.next) { coop::SendToClient(q.pc, q.msgs[q.next]); ++g_snapshotSent; }
    if (q.next >= q.msgs.size()) {
        LOGF("[world] snapshot of %d task(s) sent to %s", (int)q.msgs.size(), GetName((UObject*)q.pc).c_str());
        g_joinQueue.erase(g_joinQueue.begin());
    }
}

// ---------------------------------------------------------------- dispatch
bool OnServerOp(APlayerController*, const std::string&, const std::string&) { return false; }

bool OnClientOp(const std::string& op, const std::string& body) {
    if (op == "MT")  return ApplyTask(body);
    if (op == "MC")  return ApplyMissionCompleted(body);
    if (op == "TRK") return ApplyTrackedMission(body);
    if (op == "DLG") return ApplyDialog(body);
    if (op == "XP")  return ApplyXP(body);
    if (op == "MR")  return ApplyRewards(body);
    return false;
}

void Tick(float dt, bool isHost) {
    if (isHost) { DrainJoinQueue(); return; }
    if (!g_indicatorsDirty) return;
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
    if (a.size() > 2 && a[1] == "rewards")  g_syncRewards = a[2] == "1";
    if (a.size() > 2 && a[1] == "items") { g_logItems = a[2] == "1"; hooks::Enable("UInventoryLib::AddItemToRespectiveInventory", g_logItems); }
    // grant <xp> <credits> <standing> [missionType=2] [factionGroup=1]: run a real mission payout through
    // the game's own function, so the host hook and the client replay are exercised end to end. Safe with
    // a None task name -- AddNonItemRewards null-checks the FindTaskInPlayerData result before its decal
    // path (`test rax,rax / je`).
    if (a.size() > 4 && a[1] == "grant") {
        uint8_t rewards[32] = {};
        *(int*)(rewards + 0x00) = atoi(a[2].c_str());
        *(int*)(rewards + 0x04) = atoi(a[3].c_str());
        *(int*)(rewards + 0x0C) = atoi(a[4].c_str());
        uint8_t mt = a.size() > 5 ? (uint8_t)atoi(a[5].c_str()) : 2;
        uint8_t fg = a.size() > 6 ? (uint8_t)atoi(a[6].c_str()) : 1;
        FName none{};
        Rva<std::remove_pointer_t<Fn_AddNonItemRewards>>(es2rva::UMissionLib_AddNonItemRewards)(rewards, &mt, &fg, &none);
        out += "granted\n";
    }
    if (a.size() > 1 && a[1] == "refresh")  { Rva<std::remove_pointer_t<Fn_RefreshIndicators>>(es2rva::UMapLib_RefreshMissionAndWaypointIndicators)(); out += "indicators refreshed\n"; }
    // (XP is SENT by attribution.cpp -- see `attribution` for that counter; only the apply side is here.)
    out += Format("missions=%d dialog=%d xp=%d | sent: tasks=%llu dialog=%llu snapshot=%llu | applied: tasks=%llu dialog=%llu xp=%.1f\n",
                  (int)g_syncMissions, (int)g_syncDialog, (int)g_syncXP,
                  (unsigned long long)g_taskSent, (unsigned long long)g_dlgSent, (unsigned long long)g_snapshotSent,
                  (unsigned long long)g_taskApplied, (unsigned long long)g_dlgApplied, g_xpApplied);
    out += Format("rewards=%d logItems=%d | sent=%llu applied=%llu\n", (int)g_syncRewards, (int)g_logItems,
                  (unsigned long long)g_rewardSent, (unsigned long long)g_rewardApplied);
    if (UObject* pd = Rva<std::remove_pointer_t<Fn_GetPlayerData>>(es2rva::UGameplayLib_GetPlayerData)())
        out += Format("local player: credits=%d level=%d xp=%.0f\n",
                      UE_FIELD(int, pd, es2off::UPlayerData::Credits),
                      UE_FIELD(int, pd, es2off::UPlayerData::PlayerLevel),
                      UE_FIELD(float, pd, es2off::UPlayerData::XP));
    out += Format("cached tasks: %d\n", (int)g_taskCache.size());
    int n = 0;
    for (auto& [key, s] : g_taskCache) {
        if (n++ >= 20) { out += "  ...\n"; break; }
        out += Format("  %-28s state=%d stage=%d progress=%d loc=%s\n", s.id.c_str(), s.state, s.stage, s.progress, s.locStr.c_str());
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


// ---------------------------------------------------------------- cutscenes
// A cutscene in ES2 is a STREAMED SUB-LEVEL, identified end to end by an FName (`cutsceneLevelName`)
// whose level script actor derives from ACutsceneLevelScriptActor. UCutsceneSubSystem drives it:
//   LoadCutscene(name, attachToProxies, params, readyCb, finishedCb)
//     -> NotifyCutsceneLoaded(name, params) -> NotifyCutsceneReady(scriptActor, name)
//     -> NotifyPlayCutscene(name, proxies)  -> NotifyCutsceneFinished(name)
// The subsystem is per-WORLD and a client has its own instance, and every one of those entry points is
// a reflected UFunction, so a client can be driven through exactly the same sequence.
//
// The one genuinely hard part is `attachToProxies`, a TMap<FName, AActor*>: the cutscene binds real
// actors into the scene by role name. The KEYS are FNames both machines share; the VALUES are pointers
// that mean nothing on the other machine, so a client has to rebuild the map against its own actors.
//
// This is instrumentation FIRST, on purpose. Nothing is forwarded yet: campaign cutscenes are rare and
// none has been observed live, so guessing at the proxy contents would be inventing a design. These
// hooks record what a real cutscene actually passes, and `cutscene` prints it.
static uint64_t g_csLoad = 0, g_csPlay = 0, g_csFinish = 0;
struct CutsceneSighting { std::string what; std::string name; int proxies = -1; };
static std::vector<CutsceneSighting> g_csSeen;

// TMap is a TSet of tuples over a sparse array; its element count sits at the same offset a TArray's
// would (Data, ArrayNum). Read only the count -- enough to know whether proxies are even in play.
static int MapNum(const void* map) {
    if (!map) return -1;
    struct RawArray { void* Data; int32_t Num; int32_t Max; };
    const RawArray& a = *(const RawArray*)map;
    return (a.Num >= 0 && a.Num < 4096) ? a.Num : -1;
}

static void NoteCutscene(const char* what, FName name, const void* proxies) {
    if (g_csSeen.size() < 64)
        g_csSeen.push_back({what, name.ToString(), MapNum(proxies)});
    LOGF("[cutscene] %s name=%s proxies=%d role=%s", what, name.ToString().c_str(), MapNum(proxies),
         coop::CurrentRole() == coop::Role::Client ? "client" : "host");
}

using Fn_LoadCutscene   = void (*)(UObject*, FName, void*, UObject*, const void*, const void*);
using Fn_NotifyPlay     = void (*)(UObject*, FName, void*);
using Fn_NotifyFinished = void (*)(UObject*, FName);
static Fn_LoadCutscene   o_LoadCutscene = nullptr;
static Fn_NotifyPlay     o_NotifyPlay = nullptr;
static Fn_NotifyFinished o_NotifyFinished = nullptr;

static void H_LoadCutscene(UObject* self, FName name, void* proxies, UObject* params,
                           const void* readyCb, const void* finishedCb) {
    ++g_csLoad; NoteCutscene("load", name, proxies);
    o_LoadCutscene(self, name, proxies, params, readyCb, finishedCb);
}
static void H_NotifyPlayCutscene(UObject* self, FName name, void* proxies) {
    ++g_csPlay; NoteCutscene("play", name, proxies);
    o_NotifyPlay(self, name, proxies);
}
static void H_NotifyCutsceneFinished(UObject* self, FName name) {
    ++g_csFinish; NoteCutscene("finish", name, nullptr);
    o_NotifyFinished(self, name);
}

static void CmdCutscene(const console::Args&, std::string& out) {
    out += Format("load=%llu play=%llu finish=%llu  (host and client both log; a client seeing 0 while\n"
                  "the host counts up is exactly the bug we are chasing)\n",
                  (unsigned long long)g_csLoad, (unsigned long long)g_csPlay, (unsigned long long)g_csFinish);
    if (g_csSeen.empty()) { out += "no cutscene has been observed on this machine yet\n"; return; }
    for (auto& c : g_csSeen) out += Format("  %-7s %-40s proxies=%d\n", c.what.c_str(), c.name.c_str(), c.proxies);
}

void Register() {
    console::Register("world", "world [missions 0/1|dialog 0/1|xp 0/1|rewards 0/1|grant <xp> <cr> <standing>|items 0/1|refresh] - shared world-state sync status", CmdWorld);
    console::Register("missions", "missions [taskIdFilter] - mission task actors and this machine's PlayerData records", CmdMissions);
    console::Register("cutscene", "cutscene - cutscenes this machine has seen (name + proxy count)", CmdCutscene);
}

void OnInit() {
    hooks::Install("UMissionLib::UpdateTaskInPlayerData", es2rva::UMissionLib_UpdateTaskInPlayerData,
                   (void*)&H_UpdateTaskInPlayerData, (void**)&o_UpdateTask);
    hooks::Install("UDialogManager::EnqueueDialog", es2rva::UDialogManager_EnqueueDialog_Member,
                   (void*)&H_EnqueueDialog, (void**)&o_EnqueueDialog);
    hooks::Install("UPlayerData::OnMissionCompleted", es2rva::UPlayerData_OnMissionCompleted,
                   (void*)&H_OnMissionCompleted, (void**)&o_OnMissionCompleted);
    hooks::Install("ChangeTrackedMission_Internal", es2rva::ChangeTrackedMission_Internal,
                   (void*)&H_ChangeTrackedMission, (void**)&o_ChangeTrackedMission);
    hooks::Install("UMissionLib::AddNonItemRewards", es2rva::UMissionLib_AddNonItemRewards,
                   (void*)&H_AddNonItemRewards, (void**)&o_AddNonItemRewards);
    hooks::Install("UInventoryLib::AddItemToRespectiveInventory", es2rva::UInventoryLib_AddItemToRespectiveInventory,
                   (void*)&H_AddItemToInventory, (void**)&o_AddItemToInventory);
    hooks::Enable("UInventoryLib::AddItemToRespectiveInventory", false);    // observability, off by default
    hooks::Install("UCutsceneSubSystem::LoadCutscene", es2rva::UCutsceneSubSystem_LoadCutscene,
                   (void*)&H_LoadCutscene, (void**)&o_LoadCutscene);
    hooks::Install("UCutsceneSubSystem::NotifyPlayCutscene", es2rva::UCutsceneSubSystem_NotifyPlayCutscene,
                   (void*)&H_NotifyPlayCutscene, (void**)&o_NotifyPlay);
    hooks::Install("UCutsceneSubSystem::NotifyCutsceneFinished", es2rva::UCutsceneSubSystem_NotifyCutsceneFinished,
                   (void*)&H_NotifyCutsceneFinished, (void**)&o_NotifyFinished);
}
}
