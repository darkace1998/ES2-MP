#pragma once
#include <string>
#include "ue.h"

namespace combat {
void Register();
void OnInit();
// message handlers, called from coop's dispatcher
bool OnServerOp(ue::APlayerController* from, const std::string& op, const std::string& body);
bool OnClientOp(const std::string& op, const std::string& body);
void Tick(float dt, bool isHost);
void OnWorldChanged();          // drop every actor/component-keyed cache (the pointers are all dead)
void ClientAimTick(float dt);   // client: stream where our weapons point, so the host can aim them
void SetClientDamageBlock(bool on);
void ClientDamageNumbersTick(float dt);
}
