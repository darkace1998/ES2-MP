// Per-player ship loadout.
//
// ES2 builds a spawned player pawn from ONE piece of state: AESPawn::ShipData (FShipData @ +0x4B8),
// which AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation fills from
// UInventoryLib::GetCurrentShip() — and that reads the single process-global UPlayerData. On a host,
// every joining client therefore gets the HOST's ship.
//
// Fix: each client serializes its own ship (FShipData -> FShipDataState -> engine text via
// UScriptStruct::ExportText) and ships it to the host. While the host spawns that client's pawn we
// substitute GetCurrentShip()'s return value with their ship, so ShipData *and* everything downstream
// of it (StaticRestoreState of the saved component state, weapon spawning, colours, decals) all use
// the client's own data — no post-hoc patching.
//
// ABI note (verified by disassembly): MSVC x64 returns large structs through a hidden pointer that is
// the FIRST argument; for member functions the order is (sret, this, ...). GetCurrentShip is static,
// so it is simply (sret).
#include "loadout.h"
#include "coop.h"
#include "players.h"
#include "console.h"
#include "log.h"
#include "hooks.h"
#include <windows.h>
#include <map>
#include <vector>
#include <cstdlib>

using namespace ue;
using es2coop::Format;

namespace loadout {

// ---------------------------------------------------------------- engine bindings
using Fn_GetCurrentShip   = void* (*)(void* sretShipData);                       // static, returns FShipData
using Fn_GetShipDataState = void* (*)(void* thisShipData, void* sretState);      // member: RCX=this, RDX=sret (verified at call site 05dff8f2)
using Fn_CreateShipDataFromState = void* (*)(void* sretShipData, void* state);   // static, returns FShipData
using Fn_StaticStruct     = UObject* (*)();
using Fn_InitializeStruct = void (*)(UObject* scriptStruct, void* dest, int arrayDim);
using Fn_DestroyStruct    = void (*)(UObject* scriptStruct, void* dest, int arrayDim);
using Fn_ExportText       = void (*)(UObject* scriptStruct, FString* out, const void* value, const void* defaults, UObject* owner, int portFlags, UObject* exportRootScope, bool allowNativeOverride);
using Fn_ImportText       = const wchar_t* (*)(UObject* scriptStruct, const wchar_t* buffer, void* value, UObject* owner, int portFlags, void* errorText, const FString* structName, bool allowNativeOverride);
using Fn_Void             = void (*)();

static Fn_GetCurrentShip o_GetCurrentShip = nullptr;

// FShipData is 976 bytes, FShipDataState is 1592 — both taken from the PDB.
static constexpr size_t kShipDataSize = 976;
static constexpr size_t kShipDataStateSize = 1592;

static UObject* ShipDataStateStruct() { return Rva<std::remove_pointer_t<Fn_StaticStruct>>(es2rva::FShipDataState_StaticStruct)(); }
static void InitStruct(UObject* ss, void* p) { Rva<std::remove_pointer_t<Fn_InitializeStruct>>(es2rva::UScriptStruct_InitializeStruct)(ss, p, 1); }
static void DestroyStructAt(UObject* ss, void* p) { Rva<std::remove_pointer_t<Fn_DestroyStruct>>(es2rva::UScriptStruct_DestroyStruct)(ss, p, 1); }

// ---------------------------------------------------------------- state
static std::map<int, std::string> g_stash;        // host: playerId -> serialized ship
static int g_substituting = -1;                   // host: player whose ship replaces GetCurrentShip()
static std::vector<uint8_t> g_subState;           // host: constructed FShipDataState for that player
static bool g_enabled = true;
static uint64_t g_subCalls = 0;
// UInventory::CreateShipDataFromState itself calls back into UInventoryLib::GetCurrentShip (it consults
// the local UPlayerData while rebuilding an inventory). Without this guard our hook re-enters it forever
// and the process dies with "Maximum number of UObjects exceeded".
static bool g_inMaterialise = false;
// Only the call made from inside AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation is the one
// that becomes AESPawn::ShipData. The same function (and unrelated HUD/inventory code) calls
// GetCurrentShip hundreds of times per spawn; substituting them all rebuilds an entire inventory each
// time and exhausts the UObject array. Filter on the return address, and cap it.
static constexpr uint32_t kSpawnFnLo = 0x5DEC54C, kSpawnFnHi = 0x5DEF078;
static constexpr uint64_t kMaxSubstitutions = 4;

// client-side send state (the blob is far larger than one reliable RPC, so it is chunked)
static std::string g_pendingSend;
static size_t g_sendOffset = 0;
static int g_sendSeq = 0, g_sendTotal = 0;
static double g_sendAccum = 0;
static constexpr size_t kChunk = 480;             // conservative: stays well inside one reliable bunch
static bool g_sentThisSession = false;
static std::string g_tweakFrom, g_tweakTo;   // provenance test: rewrite the blob before sending

// host-side receive state
static std::map<int, std::string> g_recvBuf;
// host-side respawn scheduling: a client's ship blob only arrives after its pawn was already spawned,
// so once it lands we rebuild that player's pawn with the substitution armed.
struct Respawn { double at = 0; bool armed = false; };
static std::map<int, Respawn> g_respawn;
static std::map<int, bool> g_applied;
static double g_hostNow = 0;

// ---------------------------------------------------------------- export / import
// The source of truth for "my ship" is the player's OWN UPlayerData, not the pawn: on a client the pawn
// arrives through the actor channel and its FShipData (which ES2 never replicates) is empty.
// UInventoryLib::GetCurrentShip() reads this process's UPlayerData, i.e. exactly this player's ship.
bool ExportLocalShip(std::string& out, std::string& err) {
    UObject* ss = ShipDataStateStruct();
    UObject* sd = Rva<std::remove_pointer_t<Fn_StaticStruct>>(es2rva::FShipData_StaticStruct)();
    if (!ss || !sd) { err = "StaticStruct() null"; return false; }

    // Both calls CONSTRUCT into their sret buffer, so the buffers must be raw (not pre-initialised),
    // and we own the results.
    std::vector<uint8_t> ship(kShipDataSize + 32, 0);
    void* shipData = ship.data();
    if (o_GetCurrentShip) o_GetCurrentShip(shipData);
    else Rva<std::remove_pointer_t<Fn_GetCurrentShip>>(es2rva::UInventoryLib_GetCurrentShip)(shipData);

    std::vector<uint8_t> state(kShipDataStateSize + 32, 0);
    void* sp = state.data();
    Rva<std::remove_pointer_t<Fn_GetShipDataState>>(es2rva::FShipData_GetShipDataState)(shipData, sp);

    FString text;
    Rva<std::remove_pointer_t<Fn_ExportText>>(es2rva::UScriptStruct_ExportText)(ss, &text, sp, nullptr, nullptr, 0, nullptr, true);
    out = text.ToUtf8();
    DestroyStructAt(ss, sp);
    DestroyStructAt(sd, shipData);
    if (out.empty()) { err = "ExportText produced nothing"; return false; }
    return true;
}

// Host: parse a client's text blob into a constructed FShipDataState we keep for the whole spawn.
static bool ImportState(const std::string& text, std::vector<uint8_t>& outState, std::string& err) {
    UObject* ss = ShipDataStateStruct();
    if (!ss) { err = "no FShipDataState struct"; return false; }
    outState.assign(kShipDataStateSize + 32, 0);
    void* sp = outState.data();
    InitStruct(ss, sp);                                  // ImportText needs a CONSTRUCTED target
    std::wstring w = es2coop::Utf8ToWide(text);
    FString name(std::string("ShipDataState"));
    const wchar_t* rest = Rva<std::remove_pointer_t<Fn_ImportText>>(es2rva::UScriptStruct_ImportText)(
        ss, w.c_str(), sp, nullptr, 0, nullptr, &name, true);
    if (!rest) { DestroyStructAt(ss, sp); outState.clear(); err = "ImportText failed"; return false; }
    return true;
}

// ---------------------------------------------------------------- GetCurrentShip substitution
static void* H_GetCurrentShip(void* sret) {
    uintptr_t retRva = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - g_base;   // clang/zig equivalent of MSVC _ReturnAddress()
    bool fromSpawner = retRva >= kSpawnFnLo && retRva < kSpawnFnHi;
    if (g_substituting >= 0 && !g_subState.empty() && !g_inMaterialise && fromSpawner && g_subCalls < kMaxSubstitutions) {
        // Build a FRESH FShipData for every call: the caller destroys whatever it receives, and the
        // spawner calls GetCurrentShip from more than one site, so a shared buffer would be double-freed.
        g_inMaterialise = true;
        Rva<std::remove_pointer_t<Fn_CreateShipDataFromState>>(es2rva::UInventory_CreateShipDataFromState)(sret, g_subState.data());
        g_inMaterialise = false;
        ++g_subCalls;
        LOGF("[loadout] substituted GetCurrentShip() for player %d (call %llu, ret +0x%llX)",
             g_substituting, (unsigned long long)g_subCalls, (unsigned long long)retRva);
        return sret;
    }
    return o_GetCurrentShip(sret);
}

bool HasStash(int playerId) { return g_stash.count(playerId) != 0; }

bool BeginSubstitution(int playerId) {
    if (!g_enabled) return false;
    auto it = g_stash.find(playerId);
    if (it == g_stash.end()) return false;
    std::string err;
    if (!ImportState(it->second, g_subState, err)) {
        LOGF("[loadout] player %d: could not import ship state (%s)", playerId, err.c_str());
        return false;
    }
    g_substituting = playerId;
    g_subCalls = 0;
    LOGF("[loadout] player %d: ship state imported, substituting for this spawn", playerId);
    return true;
}
void EndSubstitution() {
    if (!g_subState.empty()) {
        UObject* ss = ShipDataStateStruct();
        if (ss) DestroyStructAt(ss, g_subState.data());
        g_subState.clear();
    }
    if (g_substituting >= 0) LOGF("[loadout] substitution for player %d ended after %llu call(s)", g_substituting, (unsigned long long)g_subCalls);
    g_substituting = -1;
}

void StashForPlayer(int playerId, const std::string& text) {
    g_stash[playerId] = text;
    LOGF("[loadout] stashed ship for player %d (%zu chars)", playerId, text.size());
}

// ---------------------------------------------------------------- transport
static void StartSend() {
    std::string text, err;
    if (!ExportLocalShip(text, err)) { LOGF("[loadout] export failed: %s", err.c_str()); return; }
    if (!g_tweakFrom.empty()) {
        size_t n = 0, pos = 0;
        while ((pos = text.find(g_tweakFrom, pos)) != std::string::npos) { text.replace(pos, g_tweakFrom.size(), g_tweakTo); pos += g_tweakTo.size(); ++n; }
        LOGF("[loadout] tweak applied to outgoing ship: '%s' -> '%s' x%zu", g_tweakFrom.c_str(), g_tweakTo.c_str(), n);
    }
    g_pendingSend = text;
    g_sendOffset = 0;
    g_sendSeq = 0;
    g_sendTotal = (int)((text.size() + kChunk - 1) / kChunk);
    LOGF("[loadout] sending ship: %zu chars in %d chunks", text.size(), g_sendTotal);
}

void ClientTick(float dt) {
    if (g_pendingSend.empty()) return;
    g_sendAccum += dt;
    if (g_sendAccum < 0.05) return;               // ~20 chunks/s, keeps the reliable buffer happy
    g_sendAccum = 0;
    size_t n = g_pendingSend.size() - g_sendOffset;
    if (n > kChunk) n = kChunk;
    coop::SendToServer(Format("SD|%d|%d|", g_sendSeq, g_sendTotal) + g_pendingSend.substr(g_sendOffset, n));
    g_sendOffset += n;
    ++g_sendSeq;
    if (g_sendOffset >= g_pendingSend.size()) {
        LOGF("[loadout] ship sent (%d chunks)", g_sendSeq);
        g_pendingSend.clear();
        g_sendOffset = 0;
    }
}

bool OnServerOp(APlayerController* from, const std::string& op, const std::string& body) {
    if (op != "SD") return false;
    int seq = 0, total = 0;
    size_t p1 = body.find('|'); if (p1 == std::string::npos) return true;
    size_t p2 = body.find('|', p1 + 1); if (p2 == std::string::npos) return true;
    seq = atoi(body.substr(0, p1).c_str());
    total = atoi(body.substr(p1 + 1, p2 - p1 - 1).c_str());
    std::string payload = body.substr(p2 + 1);
    players::Player* pl = players::ByController(from);
    if (!pl) return true;
    if (seq == 0) g_recvBuf[pl->id].clear();
    g_recvBuf[pl->id] += payload;
    if (seq + 1 >= total) {
        StashForPlayer(pl->id, g_recvBuf[pl->id]);
        g_recvBuf[pl->id].clear();
        coop::SendToClient(from, "SDOK|1");
        if (g_enabled && !g_applied[pl->id]) g_respawn[pl->id] = Respawn{g_hostNow + 0.2, true};
    }
    return true;
}

bool OnClientOp(const std::string& op, const std::string& body) {
    if (op != "SDOK") return false;
    LOGF("[loadout] host acknowledged our ship (%s)", body.c_str());
    return true;
}

// Host: rebuild a player's pawn now that we have their real ship.
using Fn_RestartPlayer = void (*)(void* gameMode, void* controller);
using Fn_UnPossess = void (*)(void* controller);
void HostTick(float dt) {
    g_hostNow += dt;
    if (g_respawn.empty()) return;
    for (auto it = g_respawn.begin(); it != g_respawn.end(); ) {
        if (!it->second.armed || g_hostNow < it->second.at) { ++it; continue; }
        int id = it->first;
        players::Player* pl = players::ById(id);
        AGameModeBase* gm = GetGameMode(GetWorld());
        if (pl && pl->pc && gm) {
            // Spawn + possess the new ship FIRST, then retire the placeholder: the controller must never
            // be left without a pawn, or the client's HUD tick faults.
            AActor* oldPawn = UE_FIELD(AActor*, pl->pc, es2off::AController::Pawn);
            LOGF("[loadout] player %d: restarting with their own ship (old pawn %s)", id, GetName((UObject*)oldPawn).c_str());
            // AGameModeBase::RestartPlayerAtTransform only spawns when the controller has NO pawn
            // (`if (NewPlayer->GetPawn() == nullptr && ...)`), so unpossess first. Both calls happen in
            // this one frame, so nothing ticks while the controller is pawn-less.
            if (oldPawn) Rva<std::remove_pointer_t<Fn_UnPossess>>(es2rva::AController_UnPossess)(pl->pc);
            Rva<std::remove_pointer_t<Fn_RestartPlayer>>(es2rva::AGameModeBase_RestartPlayer)(gm, pl->pc);
            AActor* newPawn = UE_FIELD(AActor*, pl->pc, es2off::AController::Pawn);
            if (newPawn && newPawn != oldPawn && oldPawn && IsValidObject((UObject*)oldPawn)) {
                UFunction* destroy = FindFunction((UObject*)oldPawn, "K2_DestroyActor");
                if (destroy) ProcessEvent((UObject*)oldPawn, destroy, nullptr);
                LOGF("[loadout] player %d: placeholder retired, now flying %s", id, GetName((UObject*)newPawn).c_str());
            } else {
                LOGF("[loadout] player %d: restart produced pawn=%s (old=%s)", id, GetName((UObject*)newPawn).c_str(), GetName((UObject*)oldPawn).c_str());
            }
            g_applied[id] = true;
        }
        it = g_respawn.erase(it);
    }
}

// ---------------------------------------------------------------- owned ships
using Fn_GetPlayerData = UObject* (*)();
struct ShipInfo { int index; std::string templateId; bool current; };
static std::vector<ShipInfo> ListOwnedShips(int* currentOut) {
    std::vector<ShipInfo> out;
    UObject* pd = Rva<std::remove_pointer_t<Fn_GetPlayerData>>(es2rva::UGameplayLib_GetPlayerData)();
    if (!pd) return out;
    struct RawArray { char* Data; int32_t Num; int32_t Max; };
    RawArray& ships = UE_FIELD(RawArray, pd, es2off::UPlayerData::Ships);
    int cur = UE_FIELD(int32_t, pd, es2off::UPlayerData::CurrentShip);
    if (currentOut) *currentOut = cur;
    if (!ships.Data || ships.Num < 0 || ships.Num > 64) return out;
    for (int i = 0; i < ships.Num; ++i) {
        char* sd = ships.Data + (size_t)i * kShipDataSize;
        UObject* item = UE_FIELD(UObject*, sd, es2off::FShipData::ShipItemInstance);
        std::string tid = "(none)";
        if (item && IsValidObject(item)) tid = UE_FIELD(FName, item, es2off::UItem::ItemTemplateID).ToString();
        out.push_back(ShipInfo{i, tid, i == cur});
    }
    return out;
}
static bool SetCurrentShipIndex(int idx) {
    UObject* pd = Rva<std::remove_pointer_t<Fn_GetPlayerData>>(es2rva::UGameplayLib_GetPlayerData)();
    if (!pd) return false;
    struct RawArray { char* Data; int32_t Num; int32_t Max; };
    RawArray& ships = UE_FIELD(RawArray, pd, es2off::UPlayerData::Ships);
    if (idx < 0 || idx >= ships.Num) return false;
    UE_FIELD(int32_t, pd, es2off::UPlayerData::CurrentShip) = idx;
    LOGF("[loadout] CurrentShip -> %d", idx);
    return true;
}

static void CmdShips(const console::Args& a, std::string& out) {
    if (a.size() > 1) {
        int idx = atoi(a[1].c_str());
        out += SetCurrentShipIndex(idx) ? Format("current ship set to %d\n", idx) : "invalid ship index\n";
    }
    int cur = -1;
    auto ships = ListOwnedShips(&cur);
    out += Format("owned ships: %d (current=%d)\n", (int)ships.size(), cur);
    for (auto& s : ships) out += Format("  [%d]%s %s\n", s.index, s.current ? " *" : "  ", s.templateId.c_str());
}

// ---------------------------------------------------------------- commands
static void CmdShipData(const console::Args& a, std::string& out) {
    std::string sub = a.size() > 1 ? a[1] : "info";
    if (sub == "export" || sub == "info") {
        std::string text, err;
        if (!ExportLocalShip(text, err)) { out += "export failed: " + err + "\n"; return; }
        out += Format("exported ship state: %zu chars, %d chunks of %zu\n", text.size(),
                      (int)((text.size() + kChunk - 1) / kChunk), kChunk);
        out += "head: " + text.substr(0, 300) + "\n";
        if (sub == "export") {
            std::wstring path = es2coop::GetModDir() + L"\\shipdata.txt";
            FILE* f = _wfopen(path.c_str(), L"w");
            if (f) { fwrite(text.data(), 1, text.size(), f); fclose(f); out += "written to ES2Coop\\shipdata.txt\n"; }
        }
    } else if (sub == "send") {
        StartSend(); out += "queued\n";
    } else if (sub == "stash") {
        for (auto& [id, s] : g_stash) out += Format("  player %d: %zu chars applied=%d\n", id, s.size(), (int)g_applied[id]);
        if (g_stash.empty()) out += "  (none)\n";
    } else if (sub == "apply") {
        int id = a.size() > 2 ? atoi(a[2].c_str()) : 1;
        g_applied[id] = false;
        g_respawn[id] = Respawn{g_hostNow, true};
        out += Format("queued respawn of player %d with their ship\n", id);
    } else if (sub == "tweak") {
        if (a.size() > 3) { g_tweakFrom = a[2]; g_tweakTo = a[3]; out += Format("outgoing blob will rewrite '%s' -> '%s'\n", g_tweakFrom.c_str(), g_tweakTo.c_str()); }
        else { g_tweakFrom.clear(); g_tweakTo.clear(); out += "tweak cleared\n"; }
    } else if (sub == "weapons") {
        // list the equipped weapon items of a pawn: shipdata weapons <0xPawnAddr|pawn>
        APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
        AActor* pawn = nullptr;
        if (a.size() > 2 && a[2].rfind("0x", 0) == 0) pawn = (AActor*)strtoull(a[2].c_str(), nullptr, 16);
        else if (pc) pawn = UE_FIELD(AActor*, pc, es2off::AController::Pawn);
        if (!pawn || !IsValidObject((UObject*)pawn)) { out += "no pawn\n"; return; }
        out += Format("pawn %s\n", GetName((UObject*)pawn).c_str());
        for (const char* slot : {"PrimaryWeapons", "SecondaryWeapons"}) {
            UObject* wc = nullptr;
            for (auto& pr : GetProperties((UStruct*)GetClass((UObject*)pawn), true))
                if (pr.Name == slot) { wc = UE_FIELD(UObject*, pawn, pr.Offset); break; }
            if (!wc) continue;
            struct RawArray { char* Data; int32_t Num; int32_t Max; };
            RawArray& slots = UE_FIELD(RawArray, wc, es2off::UWeaponComponent::WeaponSlots);
            out += Format("  %s: %d slot(s)\n", slot, slots.Num);
            for (int i = 0; i < slots.Num && i < 8; ++i) {
                char* wi = slots.Data + (size_t)i * 80;   // sizeof(FWeaponInfo)
                UObject* item = UE_FIELD(UObject*, wi, es2off::FWeaponInfo::WeaponItem);
                if (item && IsValidObject(item))
                    out += Format("    [%d] %-22s level=%d rarity=%d seed=%d\n", i,
                                  UE_FIELD(FName, item, es2off::UItem::ItemTemplateID).ToString().c_str(),
                                  UE_FIELD(int32_t, item, es2off::UItem::ItemLevel),
                                  (int)UE_FIELD(uint8_t, item, es2off::UItem::Rarity),
                                  UE_FIELD(int32_t, item, es2off::UItem::Seed));
                else out += Format("    [%d] (empty)\n", i);
            }
        }
    } else if (sub == "on")  { g_enabled = true;  out += "loadout substitution ON\n"; }
    else if (sub == "off") { g_enabled = false; out += "loadout substitution OFF\n"; }
    else out += "usage: shipdata [info|export|send|stash|on|off]\n";
}

void Register() {
    console::Register("shipdata", "shipdata [info|export|send|stash|apply <id>|on|off] - per-player ship loadout", CmdShipData);
    console::Register("ships", "ships [index] - list the ships this player owns / pick the one to fly", CmdShips);
}

void OnInit() {
    hooks::Install("UInventoryLib::GetCurrentShip", es2rva::UInventoryLib_GetCurrentShip, (void*)&H_GetCurrentShip, (void**)&o_GetCurrentShip);
}

// client: send our ship once we are connected and flying
void MaybeSendOnJoin() {
    if (g_sentThisSession) return;
    APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
    AActor* pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
    if (!pawn) return;
    g_sentThisSession = true;
    StartSend();
}
void ResetSession() {
    g_sentThisSession = false; g_pendingSend.clear(); g_sendOffset = 0;
    g_stash.clear(); g_recvBuf.clear(); g_respawn.clear(); g_applied.clear();
}
}
