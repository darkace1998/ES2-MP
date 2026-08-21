#pragma once
#include <string>
#include "ue.h"

namespace loadout {
void Register();
void OnInit();
// Client: serialize this player's live ship (type + all equipped items) to portable text.
bool ExportLocalShip(std::string& out, std::string& err);
// Host: hold a client's serialized ship so the next pawn spawned for them uses it.
void StashForPlayer(int playerId, const std::string& text);
bool HasStash(int playerId);
// Called by the spawn hook: while spawning for this player, UInventoryLib::GetCurrentShip is
// substituted with their own ship. Returns false if nothing is stashed.
bool BeginSubstitution(int playerId);
void EndSubstitution();
// message ops
bool OnServerOp(ue::APlayerController* from, const std::string& op, const std::string& body);
bool OnClientOp(const std::string& op, const std::string& body);
void ClientTick(float dt);
void HostTick(float dt);
void MaybeSendOnJoin();
void HudRebindTick();                   // re-point ES2's ingame HUD widget after the local pawn is replaced
void CrosshairCategoryTick(float dt);   // keep the client's reticle matching the equipped weapon
void ResetSession();
void OnWorldChanged();                  // forget the pawn latches (HUD rebind / weapon build) — the pointers are dead
void ClientLocalShipTick();
void ClientBuildWeaponsTick();
bool ApplyOwnShipLocally(ue::AActor* pawn, std::string& err);
int BuildWeaponsLocally(ue::AActor* pawn, std::string& log);
}
