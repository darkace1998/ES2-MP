#pragma once
#include <string>
#include <cstdint>

namespace steamp2p {
void Register();
void OnInit();
void SetHosting(bool on);

// --- Steam identity / overlay, through steam_api64.dll's flat API ---
uint64_t LocalSteamId();                 // 0 if Steam is unavailable
std::string PersonaName();               // this player's Steam name ("" if unavailable)
std::string FriendName(uint64_t steamId);
// Opens Steam's invite dialog carrying a connect string, so the friend who accepts is launched
// straight into our session. Falls back to the plain friends list when the dialog is unavailable.
bool OpenInviteOverlay(const std::string& connectString);
// Publishes the same connect string as rich presence, which is what puts "Join Game" on our entry
// in a friend's list. Pass an empty string to clear it.
void SetConnectPresence(const std::string& connectString);
std::string ConnectString();             // "steam.<id>:7777" while hosting, else ""
}
