#include "players.h"
#include "log.h"
#include <array>
#include <mutex>

using namespace ue;
using es2coop::Format;

namespace players {

static std::array<Player, kMaxPlayers> g_slots{};
static bool g_prune = true;
static int g_localId = 0;
static std::mutex g_mutex;

int LocalId() { return g_localId; }
void SetLocalId(int id) { g_localId = id; }

void Reset() {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& p : g_slots) p = Player{};
    LOGF("[players] registry reset");
}

Player* ById(int id) {
    if (id < 0 || id >= kMaxPlayers) return nullptr;
    Player& p = g_slots[id];
    return p.alive ? &p : nullptr;
}

Player* ByController(APlayerController* pc) {
    if (!pc) return nullptr;
    for (auto& p : g_slots) if (p.alive && p.pc == pc) return &p;
    return nullptr;
}

Player* ByPawn(AActor* pawn) {
    if (!pawn) return nullptr;
    for (auto& p : g_slots) if (p.alive && p.pawn == pawn) return &p;
    return nullptr;
}

Player* Local() {
    for (auto& p : g_slots) if (p.alive && p.local) return &p;
    return nullptr;
}

int Count() {
    int n = 0;
    for (auto& p : g_slots) if (p.alive) ++n;
    return n;
}

std::vector<Player*> All() {
    std::vector<Player*> out;
    for (auto& p : g_slots) if (p.alive) out.push_back(&p);
    return out;
}

Player* RegisterController(APlayerController* pc) {
    if (!pc) return nullptr;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& p : g_slots) if (p.alive && p.pc == pc) return &p;
    UNetConnection* conn = UE_FIELD(UNetConnection*, pc, es2off::APlayerController::NetConnection);
    bool isLocal = (conn == nullptr);          // no net connection => driven by this process
    // The local player always takes slot 0 on the host; remote players take the lowest free slot >= 1.
    int start = isLocal ? 0 : 1;
    for (int i = start; i < kMaxPlayers; ++i) {
        if (g_slots[i].alive) continue;
        Player& p = g_slots[i];
        p = Player{};
        p.id = i; p.pc = pc; p.conn = conn; p.local = isLocal; p.alive = true;
        p.pawn = UE_FIELD(AActor*, pc, es2off::AController::Pawn);
        p.name = isLocal ? "host" : Format("player%d", i);
        LOGF("[players] +%s id=%d pc=%s conn=%p pawn=%s", p.name.c_str(), p.id, GetName((UObject*)pc).c_str(), (void*)conn, GetName((UObject*)p.pawn).c_str());
        return &p;
    }
    LOGF("[players] registry full, refusing %s", GetName((UObject*)pc).c_str());
    return nullptr;
}

Player* RegisterLocalAs(APlayerController* pc, int id) {
    if (!pc || id < 0 || id >= kMaxPlayers) return nullptr;
    std::lock_guard<std::mutex> lk(g_mutex);
    Player& p = g_slots[id];
    p = Player{};
    p.id = id; p.pc = pc; p.conn = UE_FIELD(UNetConnection*, pc, es2off::APlayerController::NetConnection);
    p.local = true; p.alive = true;
    p.pawn = UE_FIELD(AActor*, pc, es2off::AController::Pawn);
    p.name = Format("me(p%d)", id);
    LOGF("[players] local player is id=%d pc=%s", id, GetName((UObject*)pc).c_str());
    return &p;
}

Player* RegisterRemoteAs(int id, AActor* pawn) {
    if (id < 0 || id >= kMaxPlayers || !pawn) return nullptr;
    std::lock_guard<std::mutex> lk(g_mutex);
    Player& p = g_slots[id];
    if (p.alive && p.local) return nullptr;              // never overwrite our own slot
    std::string keepName = p.alive ? p.name : std::string();
    p = Player{};
    p.id = id; p.pawn = pawn; p.local = false; p.alive = true;
    p.name = keepName.empty() ? Format("player%d", id) : keepName;
    LOGF("[players] remote player id=%d seen through pawn %s", id, GetName((UObject*)pawn).c_str());
    return &p;
}

void SetPruneDeparted(bool on) { g_prune = on; }
bool PruneDeparted() { return g_prune; }

void UnregisterController(APlayerController* pc) {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& p : g_slots) if (p.alive && p.pc == pc) {
        LOGF("[players] -%s id=%d", p.name.c_str(), p.id);
        p = Player{};
    }
}
void UnregisterId(int id) {
    if (id < 0 || id >= kMaxPlayers) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    Player& p = g_slots[id];
    if (p.alive && !p.local) { LOGF("[players] -%s id=%d", p.name.c_str(), p.id); p = Player{}; }
}

uint32_t Refresh() {
    uint32_t freed = 0;
    for (auto& p : g_slots) {
        if (!p.alive) continue;
        if (!p.pc) {
            // A client-side entry for another player: it lives exactly as long as the pawn it names.
            if (!IsValidObject((UObject*)p.pawn)) { LOGF("[players] id=%d remote pawn went away", p.id); freed |= 1u << p.id; p = Player{}; }
            continue;
        }
        if (!IsValidObject((UObject*)p.pc)) { LOGF("[players] id=%d controller went away", p.id); freed |= 1u << p.id; p = Player{}; continue; }
        // A disconnected client's controller stays readable until the next GC, so IsValidObject (which
        // deliberately means "dereferenceable", not "not pending-kill") keeps its slot alive for seconds
        // after it left. That matters because ES2 players disconnect and rejoin constantly under solo
        // docking: a rejoin landing while the corpse still held slot 1 was pushed to slot 2, and enough
        // cycles would walk off the end of the 4-slot registry. Pending-kill is the right test here.
        if (!p.local && g_prune && IsGarbage((UObject*)p.pc)) {
            LOGF("[players] id=%d disconnected (controller pending-kill) — freeing the slot", p.id);
            freed |= 1u << p.id;
            p = Player{}; continue;
        }
        p.pawn = UE_FIELD(AActor*, p.pc, es2off::AController::Pawn);
        if (p.pawn && !IsValidObject((UObject*)p.pawn)) p.pawn = nullptr;
    }
    return freed;
}

std::string Describe() {
    std::string s = Format("localId=%d count=%d\n", g_localId, Count());
    for (auto& p : g_slots) {
        if (!p.alive) continue;
        FVector loc{};
        if (p.pawn) loc = GetActorTransform(p.pawn).Translation;
        s += Format("  [%d] %-8s %s pc=%s pawn=%s loc=(%.0f, %.0f, %.0f) rx=%llu\n",
                    p.id, p.name.c_str(), p.local ? "local " : "remote",
                    GetName((UObject*)p.pc).c_str(), GetName((UObject*)p.pawn).c_str(),
                    loc.X, loc.Y, loc.Z, (unsigned long long)p.rxPackets);
    }
    return s;
}
}
