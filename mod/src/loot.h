#pragma once
#include <string>
namespace loot {
void Register();
void OnInit();
bool OnClientOp(const std::string& op, const std::string& body);
void ClientTick();      // spawn the mirrored pickups queued by OnClientOp (never from the receive path)
}
