// Per-player registry (N-player from the start; max 4).
#pragma once
#include <string>
#include <vector>
#include "ue.h"

namespace players {

constexpr int kMaxPlayers = 4;

struct Player {
    int id = -1;                       // 0 = host, 1..3 = clients (assigned by the host in join order)
    ue::APlayerController* pc = nullptr;
    ue::UNetConnection* conn = nullptr;   // null for the host's own local player
    ue::AActor* pawn = nullptr;
    bool local = false;                // this player is driven by this process
    bool alive = false;
    std::string name;

    // --- remote ship smoothing state (host side, for a client-driven pawn) ---
    ue::FVector tgtLoc{};
    ue::FQuat   tgtRot{};
    ue::FVector tgtVel{};
    double      tgtTime = 0;           // GFrameCounter-independent seconds when the sample arrived
    bool        hasTarget = false;
    uint64_t    rxPackets = 0;
    double      lastRxTime = 0;
};

void Reset();
// Host: register a joining controller, assign the next free id. Returns the player.
Player* RegisterController(ue::APlayerController* pc);
// Client side: record our own controller under the id the host assigned us.
Player* RegisterLocalAs(ue::APlayerController* pc, int id);
void UnregisterController(ue::APlayerController* pc);
Player* ByController(ue::APlayerController* pc);
Player* ByPawn(ue::AActor* pawn);
Player* ById(int id);
Player* Local();
int Count();
std::vector<Player*> All();
// Refresh pawn pointers / drop dead entries. Call from the game thread each tick.
void Refresh();
std::string Describe();
// The id this process believes it is (host = 0; client learns it from the host's WELCOME).
int LocalId();
void SetLocalId(int id);
}
