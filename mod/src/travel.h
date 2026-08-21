#pragma once
#include <string>
#include "ue.h"
namespace travel {
void Register();
void OnInit();
void ApplyOriginShiftPolicy(bool multiplayer);
void Tick(float dt, bool isHost);
void SetHostAddress(const std::string& addr);
void SetListenPort(int port);
int  ListenPort();
bool DoChangeLocation(const std::string& locationId);
bool OnServerOp(ue::APlayerController* from, const std::string& op, const std::string& body);
bool OnClientOp(const std::string& op, const std::string& body);
}
