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
bool SendToServer(const std::string& msg);                                      // client -> host (reliable); false = no local controller yet, nothing sent
void SendToClient(ue::APlayerController* pc, const std::string& msg);           // host -> one client (reliable)
void SendToAllClients(const std::string& msg);
void OnPostLogin(ue::APlayerController* pc);      // host: a client finished joining
void OnLogout(ue::APlayerController* pc);         // host: a client left
void OnSaveLoad();                                // either side: a save is being loaded outside a session (per-save caches go)
// --- lobby roster (names for the main-menu lobby overview) ---
std::string LocalPlayerName();
void BroadcastRoster();
std::string RosterName(int id);   // "" when that slot is empty
int RosterCount();
// A grant the HOST is making for itself (mission payout) while a remote player's kill scope is open:
// attribution must not redirect it. Counted, so nested scopes compose.
void PushLocalAward();
void PopLocalAward();
bool InLocalAward();
}
