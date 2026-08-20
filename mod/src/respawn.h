#pragma once
#include <string>
namespace respawn {
void Register();
void OnInit();
void Tick(float dt, bool isHost);
bool OnClientOp(const std::string& op, const std::string& body);
}
