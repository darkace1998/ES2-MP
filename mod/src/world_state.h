#pragma once
#include <string>
#include "ue.h"

// Shared world state: mission/objective progress, dialog, and kill XP.
namespace world_state {
void Register();
void OnInit();
bool OnServerOp(ue::APlayerController* from, const std::string& op, const std::string& body);
bool OnClientOp(const std::string& op, const std::string& body);
void Tick(float dt, bool isHost);
void OnPlayerJoined(ue::APlayerController* pc);   // host: queue a full snapshot for a joiner (paced out in Tick)
void ResetSession();                              // the save this snapshot describes is gone
}
