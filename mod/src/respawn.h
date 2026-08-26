#pragma once
#include <string>
namespace respawn {
void Register();
void OnInit();
void Tick(float dt, bool isHost);
bool OnClientOp(const std::string& op, const std::string& body);
void OnPlayerLeft(int id);      // host: forget that slot's death state before the next joiner inherits it
void Reset();                   // session / world change: nothing here outlives either
}
