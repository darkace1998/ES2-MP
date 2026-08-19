#pragma once
#include <string>
#include "ue.h"

namespace coop {
enum class Role { None, Host, Client };
Role CurrentRole();
void Register();          // console commands
void OnInit();            // after engine exists (hooks, tick)
// RPC channel plumbing (called from net.cpp hooks)
bool OnServerMessage(ue::APlayerController* fromPC, const std::string& msg);   // host side; true = consumed
bool OnClientMessage(ue::APlayerController* toPC, const std::string& msg);     // client side; true = consumed
void SendToServer(const std::string& msg);                                      // client -> host (reliable)
void SendToClient(ue::APlayerController* pc, const std::string& msg);           // host -> one client (reliable)
void SendToAllClients(const std::string& msg);
void OnPostLogin(ue::APlayerController* pc);      // host: a client finished joining
void OnLogout(ue::APlayerController* pc);         // host: a client left
}
