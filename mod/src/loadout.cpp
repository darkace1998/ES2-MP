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
// ABI note (verified by disassembly): MSVC x64 returns large structs through a hidden pointer. For a
// static/free function it is the first argument, (sret, ...); for a MEMBER function `this` stays in RCX
// and the sret pointer follows it, (this, sret, ...) — see FShipData::GetShipDataState below.
// GetCurrentShip is static, so it is simply (sret).
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
using Fn_ShipDataAssign   = void* (*)(void* dstShipData, const void* srcShipData);
using Fn_UpdateShipModules= void (*)(AActor* pawn, bool respawnWeapons);
using Fn_ReinitShip       = void (*)();
using Fn_PawnVoid         = void (*)(AActor* pawn);
using Fn_CreateWeaponInfo = void (*)(UObject* item, void* outWeaponInfo);
using Fn_SpawnWeapons     = void (*)(UObject* weaponComponent);
using Fn_EquipWeapon      = void (*)(UObject* weaponComponent, int slot, bool a, bool b, bool c);
using Fn_WeaponInfoCtor   = void* (*)(void* weaponInfo);

static Fn_GetCurrentShip o_GetCurrentShip = nullptr;

// FShipData is 976 bytes, FShipDataState is 1592 — both taken from the PDB.
static constexpr size_t kShipDataSize = es2off::FShipData::__size;
static constexpr size_t kShipDataStateSize = 1592;
static_assert(kShipDataSize == 976, "FShipData size changed — re-check every raw-buffer use in this file");

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
static constexpr uint32_t kSpawnFnLo = es2rva::AESGameModeBase_SpawnDefaultPawnAtTransform_Implementation, kSpawnFnHi = 0x5DEF078;   // [start, end) of that function
static_assert(kSpawnFnLo == 0x5DEC54C, "SpawnDefaultPawnAtTransform moved — re-derive kSpawnFnHi (the function's end) from the PDB");
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
// The client's own hull/armour condition, parsed out of its blob (see ApplyClientCondition).
struct Condition { float health = -1.f, armor = -1.f; };
static std::map<int, Condition> g_condition;
// Off by default: a joiner arrives in the condition its OWN save says, damage included. Turn it on
// (`shipdata repairjoin 1` on the host) to hand every joining player a repaired hull instead -- useful
// when the saves involved are parked at near-zero hull and a session would start one hit from death.
static bool g_repairOnJoin = false;
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

// The exported FShipDataState carries the client's own ship condition as `Health=` (hull ratio) and
// `ArmorRatio=`, but the spawn does not use them: a joining client's ship comes up with the HOST's saved
// condition instead. Verified by provenance -- a blob rewritten to `Health=1.000000` still produced 0.01
// on the host, which is the host's own UPlayerData value, and changing the host's LIVE hull to 0.6 did
// not move it either. So a joiner's bars read whatever the host's saved ship was (near-zero here), and
// the first thing that ever corrects them is dying, because respawn restores to full. Keep the client's
// numbers when we stash the blob so the pawn we spawn for them can be set to their own condition.
static UObject* ComponentOfClass(AActor* pawn, const char* clsName) {
    UClass* want = FindClass(clsName);
    if (!want || !pawn) return nullptr;
    for (auto& pr : GetProperties((UStruct*)GetClass((UObject*)pawn), true)) {
        if (pr.TypeName != "ObjectProperty") continue;
        UObject* v = UE_FIELD(UObject*, pawn, pr.Offset);
        if (v && IsValidObject(v) && IsA(v, want)) return v;
    }
    return nullptr;
}

static float ParseRatio(const std::string& text, const char* key) {
    size_t at = text.find(key);
    if (at == std::string::npos) return -1.f;
    float v = (float)atof(text.c_str() + at + strlen(key));
    return (v >= 0.f && v <= 1.f) ? v : -1.f;
}

// A player that logs in again must have its ship applied again. `g_applied` is what stops the host
// re-substituting on every blob, but it is keyed by player id, and a rejoin reuses the id: after a dock
// (which is a disconnect and a reconnect, see travel.cpp) the flag was still set from the first join, so
// no substitution respawn was armed and the client kept flying the placeholder the host had built from
// its OWN UPlayerData -- the host's ship model, the host's stats, a near-empty shield. Being destroyed
// was the only way out, because that respawn spawns through the substitution again.
void OnPlayerJoined(int playerId) {
    g_applied.erase(playerId);
    g_respawn.erase(playerId);
    g_recvBuf.erase(playerId);
    g_condition.erase(playerId);
    LOGF("[loadout] player %d (re)joined — their ship will be applied again", playerId);
}

void StashForPlayer(int playerId, const std::string& text) {
    g_condition[playerId] = Condition{ParseRatio(text, "Health="), ParseRatio(text, "ArmorRatio=")};
    LOGF("[loadout] player %d ship condition: hull=%.3f armor=%.3f",
         playerId, g_condition[playerId].health, g_condition[playerId].armor);
    g_stash[playerId] = text;
    LOGF("[loadout] stashed ship for player %d (%zu chars)", playerId, text.size());
}

// ---------------------------------------------------------------- client-local ship
//
// AESPawn::ShipData does not replicate. The host builds the client's server-side pawn correctly (that is
// what the loadout substitution is for), but the client's LOCAL copy of its own ship arrives through the
// actor channel with a default, empty FShipData — no weapons, no modules, nothing for the equipment UI
// to show. The client already owns the right data in its own UPlayerData, so materialising it locally
// costs nothing and is purely cosmetic/local: it is what makes the client able to see its guns fire and
// to open its ship/equipment screens.
static uint64_t g_localApplied = 0;
static bool g_autoLocalShip = true;
static Fn_PawnVoid o_BeginPlay = nullptr;
static Fn_PawnVoid o_PostInitComponents = nullptr;
static uint64_t g_preBeginPlay = 0, g_prePostInit = 0;

// Copy this machine's own current ship into a pawn's FShipData. Returns false if we have no ship.
static bool CopyOwnShipInto(AActor* pawn) {
    UObject* sdStruct = Rva<std::remove_pointer_t<Fn_StaticStruct>>(es2rva::FShipData_StaticStruct)();
    if (!sdStruct || !pawn) return false;
    std::vector<uint8_t> ship(kShipDataSize + 32, 0);
    void* tmp = ship.data();
    if (o_GetCurrentShip) o_GetCurrentShip(tmp);
    else Rva<std::remove_pointer_t<Fn_GetCurrentShip>>(es2rva::UInventoryLib_GetCurrentShip)(tmp);
    bool ok = UE_FIELD(UObject*, tmp, es2off::FShipData::ShipItemInstance) != nullptr;
    if (ok) {
        void* dst = reinterpret_cast<char*>(pawn) + es2off::AESPawn::ShipData;
        Rva<std::remove_pointer_t<Fn_ShipDataAssign>>(es2rva::FShipData_Assign)(dst, tmp);
    }
    DestroyStructAt(sdStruct, tmp);
    return ok;
}

// Is this (client-side) pawn our own ship? At PostInitializeComponents time there is no controller yet
// and the net role is not settled, so use the class's own flag: AESPawn::bIsPlayerPawn is set on the
// player-ship Blueprint and false for every NPC ship.
static bool LooksLikeOurShip(AActor* pawn) {
    if (!pawn) return false;
    UClass* esPawn = FindClass("/Script/ES2.ESPawn");
    if (!esPawn || !IsA((UObject*)pawn, esPawn)) return false;
    return UE_FIELD(bool, pawn, es2off::AESPawn::bIsPlayerPawn);
}

// THE fix for "the client cannot shoot / cannot equip".
//
// ES2 never replicates AESPawn::ShipData, so a client's own ship arrives empty. The build that turns
// ShipData into weapons, devices and consumables is NOT authority-gated — it is
// UWeaponComponent::InitializeComponent (and UDeviceComponent::Init, UConsumableComponent::InitializeComponent),
// each gated only on `Owner->ShipData.Inventory != null` — no authority check is involved anywhere.
//
// Ordering matters and is easy to get wrong. AActor::PostActorConstruction (0x156A448) runs:
//     PreInitializeComponents   (vtable +0x528)
//     AActor::InitializeComponents()      <-- every component's InitializeComponent runs HERE
//     PostInitializeComponents  (vtable +0x530)
// so filling ShipData "before PostInitializeComponents" is still ONE STEP TOO LATE: the device and
// consumable components have already looked at a null Inventory and left their slot arrays empty.
// Weapons only appeared to work because the mod rebuilds those explicitly (ClientBuildWeaponsTick).
// H_PreInitComponents below is the earliest point that fixes all three through the vanilla path;
// this PostInit hook stays as a backstop and no-ops once ShipItemInstance is set.
static void H_PostInitComponents(AActor* pawn) {
    if (pawn && coop::CurrentRole() == coop::Role::Client && g_autoLocalShip && LooksLikeOurShip(pawn)) {
        void* sd = reinterpret_cast<char*>(pawn) + es2off::AESPawn::ShipData;
        if (UE_FIELD(UObject*, sd, es2off::FShipData::ShipItemInstance) == nullptr && CopyOwnShipInto(pawn)) {
            ++g_prePostInit;
            LOGF("[loadout] filled our own ShipData on %s before PostInitializeComponents", GetName((UObject*)pawn).c_str());
        }
    }
    o_PostInitComponents(pawn);
}

// The real fix for devices and consumables: fill ShipData before AActor::InitializeComponents runs,
// so UDeviceComponent::Init and UConsumableComponent::InitializeComponent see a populated Inventory
// and build their slot arrays the vanilla way.
static Fn_PawnVoid o_PreInitComponents = nullptr;
static uint64_t g_prePreInit = 0;
static void H_PreInitComponents(AActor* pawn) {
    if (pawn && coop::CurrentRole() == coop::Role::Client && g_autoLocalShip && LooksLikeOurShip(pawn)) {
        void* sd = reinterpret_cast<char*>(pawn) + es2off::AESPawn::ShipData;
        if (UE_FIELD(UObject*, sd, es2off::FShipData::ShipItemInstance) == nullptr && CopyOwnShipInto(pawn)) {
            ++g_prePreInit;
            LOGF("[loadout] filled our own ShipData on %s before PreInitializeComponents", GetName((UObject*)pawn).c_str());
        }
    }
    o_PreInitComponents(pawn);
}

// Kept as a late backstop for pawns that somehow slipped past the above (e.g. a pawn that existed before
// the mod decided we were a client).
static void H_BeginPlay(AActor* pawn) {
    if (pawn && coop::CurrentRole() == coop::Role::Client && g_autoLocalShip) {
        uint8_t role = UE_FIELD(uint8_t, pawn, es2off::AActor::Role);
        void* sd = reinterpret_cast<char*>(pawn) + es2off::AESPawn::ShipData;
        bool empty = UE_FIELD(UObject*, sd, es2off::FShipData::ShipItemInstance) == nullptr;
        if (role == 2 /*ROLE_AutonomousProxy = our own ship*/ && empty) {
            if (CopyOwnShipInto(pawn)) {
                ++g_preBeginPlay;
                LOGF("[loadout] filled our own ShipData on %s before BeginPlay", GetName((UObject*)pawn).c_str());
            }
        }
    }
    o_BeginPlay(pawn);
}

static bool LocalShipIsEmpty(AActor* pawn) {
    if (!pawn) return false;
    void* sd = reinterpret_cast<char*>(pawn) + es2off::AESPawn::ShipData;
    return UE_FIELD(UObject*, sd, es2off::FShipData::ShipItemInstance) == nullptr;
}

bool ApplyOwnShipLocally(AActor* pawn, std::string& err) {
    if (!pawn || !IsValidObject((UObject*)pawn)) { err = "no pawn"; return false; }
    UObject* sdStruct = Rva<std::remove_pointer_t<Fn_StaticStruct>>(es2rva::FShipData_StaticStruct)();
    if (!sdStruct) { err = "no FShipData struct"; return false; }

    std::vector<uint8_t> ship(kShipDataSize + 32, 0);
    void* tmp = ship.data();
    if (o_GetCurrentShip) o_GetCurrentShip(tmp);                       // constructs into tmp; we own it
    else Rva<std::remove_pointer_t<Fn_GetCurrentShip>>(es2rva::UInventoryLib_GetCurrentShip)(tmp);

    if (UE_FIELD(UObject*, tmp, es2off::FShipData::ShipItemInstance) == nullptr) {
        DestroyStructAt(sdStruct, tmp);
        err = "this machine's UPlayerData has no current ship";
        return false;
    }
    void* dst = reinterpret_cast<char*>(pawn) + es2off::AESPawn::ShipData;
    Rva<std::remove_pointer_t<Fn_ShipDataAssign>>(es2rva::FShipData_Assign)(dst, tmp);   // deep copy
    DestroyStructAt(sdStruct, tmp);

    // ...and make the pawn actually build itself from it. UpdateShipModules(true) rebuilds the modules
    // and respawns the weapon ACTORS, but the weapon SLOTS (FWeaponInfo::WeaponItem) are filled from the
    // ship's inventory by ReinitShipAfterPotentialChanges, which resolves the local UPlayerData and
    // GetPlayerPawn(0) — on a client that is exactly this pawn.
    Rva<std::remove_pointer_t<Fn_UpdateShipModules>>(es2rva::AESPawn_UpdateShipModules)(pawn, true);
    Rva<std::remove_pointer_t<Fn_ReinitShip>>(es2rva::UInventoryLib_ReinitShipAfterPotentialChanges)();
    Rva<std::remove_pointer_t<Fn_UpdateShipModules>>(es2rva::AESPawn_UpdateShipModules)(pawn, true);
    ++g_localApplied;
    LOGF("[loadout] materialised our own ship on the local pawn %s", GetName((UObject*)pawn).c_str());
    return true;
}

// The vanilla "build my ship from ShipData" path is authority-gated, so on a client nothing fills
// UWeaponComponent::WeaponSlots. Do it explicitly from the ship's own inventory: each slot's FWeaponInfo
// is built from the corresponding UItem, then the component spawns the weapon actors.
static constexpr size_t kWeaponInfoSize = es2off::FWeaponInfo::__size;    // sizeof(FWeaponInfo), from the PDB
struct RawPtrArray { UObject** Data; int32_t Num; int32_t Max; };

int BuildWeaponsLocally(AActor* pawn, std::string& log) {
    if (!pawn || !IsValidObject((UObject*)pawn)) { log += "no pawn\n"; return 0; }
    UObject* inv = UE_FIELD(UObject*, reinterpret_cast<char*>(pawn) + es2off::AESPawn::ShipData, es2off::FShipData::Inventory);
    if (!inv || !IsValidObject(inv)) { log += "pawn has no ship inventory\n"; return 0; }

    struct Slot { const char* prop; uint32_t invOff; } kinds[2] = {
        {"PrimaryWeapons",   es2off::UInventory::PrimaryWeapons},
        {"SecondaryWeapons", es2off::UInventory::SecondaryWeapons},
    };
    int filled = 0;
    for (auto& k : kinds) {
        UObject* wc = nullptr;
        for (auto& p : GetProperties((UStruct*)GetClass((UObject*)pawn), true))
            if (p.Name == k.prop) { wc = UE_FIELD(UObject*, pawn, p.Offset); break; }
        if (!wc || !IsValidObject(wc)) { log += Format("%s: no component\n", k.prop); continue; }

        RawPtrArray& items = UE_FIELD(RawPtrArray, inv, k.invOff);
        struct RawArr { char* Data; int32_t Num; int32_t Max; };
        RawArr& slots = UE_FIELD(RawArr, wc, es2off::UWeaponComponent::WeaponSlots);
        RawPtrArray& sockets = UE_FIELD(RawPtrArray, wc, es2off::UWeaponComponent::WeaponSockets);
        log += Format("%s: %d slot(s), %d socket(s), %d item(s) in inventory\n", k.prop, slots.Num, sockets.Num, items.Num);
        if (!items.Data || items.Num <= 0) continue;

        // The client's secondary component often arrives with sockets but ZERO slots, because the array is
        // sized by the authority-only setup. Grow it here: FWeaponInfo is non-POD (soft class ptr, arrays),
        // so every element must be default-constructed, not memset.
        if (slots.Num <= 0 && sockets.Num > 0) {
            int want = sockets.Num < items.Num ? sockets.Num : items.Num;
            void* mem = Malloc(kWeaponInfoSize * want, 0);
            if (mem) {
                memset(mem, 0, kWeaponInfoSize * want);
                for (int i = 0; i < want; ++i)
                    Rva<std::remove_pointer_t<Fn_WeaponInfoCtor>>(es2rva::FWeaponInfo_Ctor)(reinterpret_cast<char*>(mem) + (size_t)i * kWeaponInfoSize);
                slots.Data = reinterpret_cast<char*>(mem);
                slots.Num = want;
                slots.Max = want;
                log += Format("  grew %s to %d slot(s)\n", k.prop, want);
            }
        }
        if (!slots.Data || slots.Num <= 0) continue;

        int n = slots.Num < items.Num ? slots.Num : items.Num;
        for (int i = 0; i < n; ++i) {
            UObject* item = items.Data[i];
            if (!item || !IsValidObject(item)) continue;
            void* slot = slots.Data + (size_t)i * kWeaponInfoSize;
            Rva<std::remove_pointer_t<Fn_CreateWeaponInfo>>(es2rva::UWeaponComponent_CreateWeaponInfoFromItem)(item, slot);
            ++filled;
            log += Format("  slot %d <- %s\n", i, UE_FIELD(FName, item, es2off::UItem::ItemTemplateID).ToString().c_str());
        }
        Rva<std::remove_pointer_t<Fn_SpawnWeapons>>(es2rva::UWeaponComponent_SpawnWeapons)(wc);
    }
    return filled;
}

void ClientLocalShipTick() {
    if (!g_autoLocalShip) return;
    APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
    AActor* pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
    if (!pawn) return;
    UClass* esPawn = FindClass("/Script/ES2.ESPawn");
    if (!esPawn || !IsA((UObject*)pawn, esPawn)) return;
    if (!LocalShipIsEmpty(pawn)) return;
    std::string err;
    if (!ApplyOwnShipLocally(pawn, err)) {
        static int warned = 0;
        if (warned++ < 5) LOGF("[loadout] could not materialise local ship: %s", err.c_str());
    }
}

// Even with ShipData in place the client's weapon slots stay empty (the vanilla build is authority-gated),
// so build them explicitly once per pawn.
static AActor* g_builtFor = nullptr;
void ClientBuildWeaponsTick() {
    if (!g_autoLocalShip) return;
    if (coop::CurrentRole() != coop::Role::Client) return;
    APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
    AActor* pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
    if (!pawn || pawn == g_builtFor) return;
    UClass* esPawn = FindClass("/Script/ES2.ESPawn");
    if (!esPawn || !IsA((UObject*)pawn, esPawn)) return;
    UObject* inv = UE_FIELD(UObject*, reinterpret_cast<char*>(pawn) + es2off::AESPawn::ShipData, es2off::FShipData::Inventory);
    if (!inv || !IsValidObject(inv)) return;          // ship data not materialised yet
    std::string log;
    int n = BuildWeaponsLocally(pawn, log);
    g_builtFor = pawn;
    LOGF("[loadout] built %d local weapon slot(s) on %s\n%s", n, GetName((UObject*)pawn).c_str(), log.c_str());
}

// ---------------------------------------------------------------- transport
static bool StartSend() {
    std::string text, err;
    if (!ExportLocalShip(text, err)) { LOGF("[loadout] export failed: %s", err.c_str()); return false; }
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
    return true;
}

// One chunk per 50 ms meant the 60-chunk ship blob took ~3.9 s of the join -- and on a machine running at
// ~21 FPS the accumulator often needed two ticks per chunk, so it was closer to 15 chunks/s than 20.
// Several chunks per tick is well within UE's reliable window (256 bunches per channel; the whole blob is
// 60), and it turns the transfer into a rounding error next to the rest of the join.
static int g_chunksPerTick = 8;

void ClientTick(float dt) {
    (void)dt;
    if (g_pendingSend.empty()) return;
    for (int i = 0; i < g_chunksPerTick && g_sendOffset < g_pendingSend.size(); ++i) {
        size_t n = g_pendingSend.size() - g_sendOffset;
        if (n > kChunk) n = kChunk;
        coop::SendToServer(Format("SD|%d|%d|", g_sendSeq, g_sendTotal) + g_pendingSend.substr(g_sendOffset, n));
        g_sendOffset += n;
        ++g_sendSeq;
    }
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
// ---------------------------------------------------------------- HUD rebinding
//
// ES2's ingame HUD widget is created under the GameInstance, not the pawn, so it outlives both a
// ClientTravel into the host's world and the placeholder swap below. It resolves the player pawn and
// its weapon/device/consumable components once, when it is constructed, and ES2 never rebinds it —
// a single-player game never replaces the player pawn out from under the HUD.
//
// The result on a client: after we retire the placeholder ship (and after any respawn), the widget
// still points at the destroyed pawn, so weapon swaps, the travel/cruise drive charge and the
// device/consumable slots all stop updating while the underlying state moves normally.
//
// ES2 already ships the cure: WG_Ingame_HUD_C::ReInit, reachable as PC -> MyHUD -> IngameHudWidget.

// ReInit re-resolves what the top-level HUD widget caches, but its children bind the pawn's components
// in their own Construct and ReInit does not cascade into them. After a pawn swap the crosshair
// (WG_Crosshair_C::PlayerWeaponComponent) and the equipment bar (WG_HUD_Equipment_C::WeaponComponent)
// still point at the RETIRED pawn's components, so a weapon switch fires OnWeaponSwitched on a component
// nothing on screen listens to and the reticle never changes shape. Re-running Construct on just those
// children rebinds the pointers and re-subscribes their delegates; children already correct are skipped,
// and this only runs on a pawn change, so no widget is reconstructed on a normal frame.
static void RebindStaleChildWidgets(UObject* widget, AActor* pawn) {
    UClass* compCls = FindClass("ActorComponent");
    if (!compCls) return;
    for (auto& p : GetProperties((UStruct*)GetClass(widget), true)) {
        if (p.TypeName != "ObjectProperty") continue;
        UObject* child = UE_FIELD(UObject*, widget, p.Offset);
        if (!child || !IsValidObject(child)) continue;
        UFunction* ctor = FindFunction(child, "Construct");
        if (!ctor) continue;                       // no init graph of its own -- nothing to redo
        bool stale = false;
        for (auto& q : GetProperties((UStruct*)GetClass(child), true)) {
            if (q.TypeName != "ObjectProperty") continue;
            UObject* v = UE_FIELD(UObject*, child, q.Offset);
            if (!v || !IsValidObject(v) || !IsA(v, compCls)) continue;
            AActor* owner = UE_FIELD(AActor*, v, es2off::UActorComponent::OwnerPrivate);
            if (owner && owner != pawn) { stale = true; break; }
        }
        if (!stale) continue;
        ProcessEvent(child, ctor, nullptr);
        LOGF("[loadout] re-Construct %s (held a component from a retired pawn)", GetName(child).c_str());
    }
}

// Calling the game's own function re-resolves everything the TOP-LEVEL widget cached. Its children
// keep their own cached component pointers and ReInit does not reach them, so RebindStaleChildWidgets
// above finishes the job. The widget property is looked up by reflection rather than a fixed offset so
// this keeps working across HUD Blueprint variants.
// Runs for host and client alike: the host's pawn is swapped on respawn too.
static AActor* g_lastHudPawn = nullptr;
void HudRebindTick() {
    UWorld* w = GetWorld();
    APlayerController* pc = w ? GetFirstLocalPlayerController(w) : nullptr;
    AActor* pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
    if (!pawn || pawn == g_lastHudPawn || !IsValidObject((UObject*)pawn)) return;
    UObject* hud = UE_FIELD(UObject*, pc, es2off::APlayerController::MyHUD);
    if (!hud || !IsValidObject(hud)) return;          // no HUD yet: retry next tick, pawn not latched
    UObject* widget = nullptr;
    for (auto& p : GetProperties((UStruct*)GetClass(hud), true)) {
        if (p.Name != "IngameHudWidget" || p.TypeName != "ObjectProperty") continue;
        widget = UE_FIELD(UObject*, hud, p.Offset);
        break;
    }
    if (!widget || !IsValidObject(widget)) return;
    UFunction* fn = FindFunction(widget, "ReInit");
    if (!fn) return;
    g_lastHudPawn = pawn;
    ProcessEvent(widget, fn, nullptr);
    RebindStaleChildWidgets(widget, pawn);
    LOGF("[loadout] HUD rebound to %s", GetName((UObject*)pawn).c_str());
}

// ---------------------------------------------------------------- crosshair reticle
// ES2 keeps the reticle's shape in WG_Crosshair_C::WeaponCategory -- an item sub-category id such as
// cat_coil_gun -- and changes it by calling that widget's SetWeaponCategory when the player switches
// weapon. On a client that call never arrives. Everything upstream of it works: the weapon component's
// EquippedSlotIndex moves and the HUD's slot highlight follows, so the client ends up firing a thermo
// gun with a coil gun's reticle still on screen. Push the value ourselves from whatever is equipped,
// which also repairs the mismatch a client starts with after its ship is rebuilt locally.

// PC -> MyHUD -> IngameHudWidget -> Crosshair, plus the two fields we need off it.
struct CrosshairRef {
    UObject* widget = nullptr;      // WG_Crosshair_C
    UObject* weaponComp = nullptr;  // its cached PlayerWeaponComponent
    uint32_t catOff = 0;            // offset of its WeaponCategory FName
    explicit operator bool() const { return widget && weaponComp && catOff; }
};

static CrosshairRef FindCrosshair() {
    CrosshairRef r;
    UWorld* w = GetWorld();
    APlayerController* pc = w ? GetFirstLocalPlayerController(w) : nullptr;
    UObject* hud = pc ? UE_FIELD(UObject*, pc, es2off::APlayerController::MyHUD) : nullptr;
    if (!hud || !IsValidObject(hud)) return r;
    UObject* root = nullptr;
    for (auto& p : GetProperties((UStruct*)GetClass(hud), true))
        if (p.Name == "IngameHudWidget" && p.TypeName == "ObjectProperty") { root = UE_FIELD(UObject*, hud, p.Offset); break; }
    if (!root || !IsValidObject(root)) return r;
    for (auto& p : GetProperties((UStruct*)GetClass(root), true))
        if (p.Name == "Crosshair" && p.TypeName == "ObjectProperty") { r.widget = UE_FIELD(UObject*, root, p.Offset); break; }
    if (!r.widget || !IsValidObject(r.widget)) { r.widget = nullptr; return r; }
    for (auto& p : GetProperties((UStruct*)GetClass(r.widget), true)) {
        if (p.Name == "PlayerWeaponComponent" && p.TypeName == "ObjectProperty") r.weaponComp = UE_FIELD(UObject*, r.widget, p.Offset);
        else if (p.Name == "WeaponCategory" && p.TypeName == "NameProperty") r.catOff = p.Offset;
    }
    return r;
}

// Read an FName out of a UFunction, or pass one in. ES2 exposes both halves we need this way:
// UItem::GetSubCategoryID returns the id, WG_Crosshair_C::SetWeaponCategory consumes it.
static bool CallNameFn(UObject* obj, const char* fnName, FName* inOut, bool isSetter) {
    UFunction* fn = obj ? FindFunction(obj, fnName) : nullptr;
    if (!fn) return false;
    std::vector<char> parms(UE_FIELD(uint16_t, fn, es2off::UFunction::ParmsSize) + 16, 0);
    for (auto& p : GetProperties((UStruct*)fn, false)) {
        const bool isReturn = (p.Flags & 0x400 /*CPF_ReturnParm*/) != 0;
        if (isSetter == isReturn || p.TypeName != "NameProperty") continue;
        if (isSetter) *(FName*)(parms.data() + p.Offset) = *inOut;
        ProcessEvent(obj, fn, parms.data());
        if (!isSetter) *inOut = *(FName*)(parms.data() + p.Offset);
        return true;
    }
    return false;
}

// The UItem equipped in a weapon component's active slot, or null.
static UObject* EquippedItem(UObject* wc) {
    if (!wc || !IsValidObject(wc)) return nullptr;
    struct RawArray { char* Data; int32_t Num; int32_t Max; };
    RawArray& slots = UE_FIELD(RawArray, wc, es2off::UWeaponComponent::WeaponSlots);
    int32_t idx = UE_FIELD(int32_t, wc, es2off::UWeaponComponent::EquippedSlotIndex);
    if (idx < 0 || idx >= slots.Num || !slots.Data) return nullptr;
    UObject* item = *(UObject**)(slots.Data + (size_t)idx * es2off::FWeaponInfo::__size
                                            + es2off::FWeaponInfo::WeaponItem);
    return (item && IsValidObject(item)) ? item : nullptr;
}

static double g_reticleNext = 0;
void CrosshairCategoryTick(float dt) {
    if (coop::CurrentRole() != coop::Role::Client) return;   // only the client misses ES2's own call
    g_reticleNext -= dt;
    if (g_reticleNext > 0) return;
    g_reticleNext = 0.2;                                     // 5 Hz is well under a human weapon swap
    CrosshairRef c = FindCrosshair();
    if (!c) return;
    UObject* item = EquippedItem(c.weaponComp);
    if (!item) return;
    FName want{};
    if (!CallNameFn(item, "GetSubCategoryID", &want, false) || want.IsNone()) return;
    if (UE_FIELD(FName, c.widget, c.catOff) == want) return; // already right: no call, no churn
    CallNameFn(c.widget, "SetWeaponCategory", &want, true);
    LOGF("[loadout] reticle -> %s", want.ToString().c_str());
}

// Reports each stage of the lookup above, so a silent bail-out can be located.
static void CmdReticle(const console::Args&, std::string& out) {
    out += Format("role=%d (client=%d)\n", (int)coop::CurrentRole(), (int)coop::Role::Client);
    CrosshairRef c = FindCrosshair();
    out += Format("crosshair=%p weaponComp=%p catOff=0x%X cur=%s\n", (void*)c.widget, (void*)c.weaponComp,
                  c.catOff, c.catOff ? UE_FIELD(FName, c.widget, c.catOff).ToString().c_str() : "?");
    if (!c) return;
    out += Format("equippedSlotIndex=%d\n", UE_FIELD(int32_t, c.weaponComp, es2off::UWeaponComponent::EquippedSlotIndex));
    UObject* item = EquippedItem(c.weaponComp);
    out += Format("item=%p %s\n", (void*)item, item ? GetFullName(item).c_str() : "");
    if (!item) return;
    FName want{};
    bool ok = CallNameFn(item, "GetSubCategoryID", &want, false);
    out += Format("GetSubCategoryID ok=%d -> %s\n", (int)ok, want.ToString().c_str());
}

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
            }
            // Their ship, their condition -- not the host's. Through ES2's own setter so the bars and
            // delegates follow (a raw field write leaves the HUD stale).
            Condition c = g_condition[id];
            if (g_repairOnJoin) { c.health = 1.f; c.armor = 1.f; }
            // The blob carries no shield ratio -- shields are not saved condition, they regenerate --
            // so bring it up with the hull, which is what the death respawn already does. Left alone it
            // comes up near empty and reads as "my shield is broken" on a freshly joined ship.
            for (auto& [cls, ratio] : {std::pair<const char*, float>{"HealthComponent", c.health},
                                       std::pair<const char*, float>{"ArmorComponent", c.armor},
                                       std::pair<const char*, float>{"ShieldComponent", 1.f}}) {
                if (ratio < 0.f) continue;
                if (UObject* comp = ComponentOfClass(newPawn, cls)) {
                    if (UFunction* fn = FindFunction(comp, "SetCurrentHitpointsWithRatio")) {
                        float r = ratio; ProcessEvent(comp, fn, &r);
                    }
                }
            }
            if (c.health >= 0.f) {
                LOGF("[loadout] player %d: restored their own condition (hull=%.3f armor=%.3f)",
                     id, c.health, c.armor);
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
    } else if (sub == "local") {
        APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
        AActor* pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
        std::string err;
        if (a.size() > 2 && a[2] == "auto") { g_autoLocalShip = a.size() > 3 ? a[3] == "1" : true; }
        else if (a.size() > 2 && a[2] == "build") { std::string log; int n = BuildWeaponsLocally(pawn, log); out += log + Format("filled %d slot(s)\n", n); }
        else out += ApplyOwnShipLocally(pawn, err) ? "applied our own ship to the local pawn\n" : ("failed: " + err + "\n");
        out += Format("autoLocalShip=%d applied=%llu prePreInit=%llu prePostInit=%llu preBeginPlay=%llu localShipEmpty=%d\n", (int)g_autoLocalShip,
                      (unsigned long long)g_localApplied, (unsigned long long)g_prePreInit,
                      (unsigned long long)g_prePostInit,
                      (unsigned long long)g_preBeginPlay, (int)LocalShipIsEmpty(pawn));
    } else if (sub == "stash") {
        out += Format("  repairOnJoin=%d\n", (int)g_repairOnJoin);
        for (auto& [id, s] : g_stash)
            out += Format("  player %d: %zu chars applied=%d hull=%.3f armor=%.3f\n", id, s.size(),
                          (int)g_applied[id], g_condition[id].health, g_condition[id].armor);
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
            // EquippedSlotIndex is the "which weapon am I holding" the HUD highlights — the one that
            // has to move when a swap happens.
            out += Format("  %s: %d slot(s), equipped=%d\n", slot, slots.Num,
                          UE_FIELD(int32_t, wc, es2off::UWeaponComponent::EquippedSlotIndex));
            for (int i = 0; i < slots.Num && i < 8; ++i) {
                char* wi = slots.Data + (size_t)i * es2off::FWeaponInfo::__size;
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
    } else if (sub == "devices") {
        // The equipment half of "the client can't equip anything": UDeviceComponent::Init and
        // UConsumableComponent::InitializeComponent fill these from ShipData.Inventory during
        // PostInitializeComponents. Empty slots here mean the ship data arrived too late.
        APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
        AActor* pawn = nullptr;
        if (a.size() > 2 && a[2].rfind("0x", 0) == 0) pawn = (AActor*)strtoull(a[2].c_str(), nullptr, 16);
        else if (pc) pawn = UE_FIELD(AActor*, pc, es2off::AController::Pawn);
        if (!pawn || !IsValidObject((UObject*)pawn)) { out += "no pawn\n"; return; }
        out += Format("pawn %s\n", GetName((UObject*)pawn).c_str());

        struct RawArray { char* Data; int32_t Num; int32_t Max; };
        auto listSlots = [&](const char* label, UObject* comp, uint32_t arrOff, uint32_t itemOff, uint32_t stride) {
            if (!comp) { out += Format("  %s: (no component)\n", label); return; }
            RawArray& slots = UE_FIELD(RawArray, comp, arrOff);
            out += Format("  %s: %d slot(s)\n", label, slots.Num);
            for (int i = 0; i < slots.Num && i < 8; ++i) {
                UObject* item = UE_FIELD(UObject*, slots.Data + (size_t)i * stride, itemOff);
                if (item && IsValidObject(item))
                    out += Format("    [%d] %-22s level=%d rarity=%d\n", i,
                                  UE_FIELD(FName, item, es2off::UItem::ItemTemplateID).ToString().c_str(),
                                  UE_FIELD(int32_t, item, es2off::UItem::ItemLevel),
                                  (int)UE_FIELD(uint8_t, item, es2off::UItem::Rarity));
                else out += Format("    [%d] (empty)\n", i);
            }
        };
        // Find the components by class rather than by property name, so this does not depend on
        // which Blueprint ship variant the player is flying.
        UObject *dev = nullptr, *con = nullptr;
        for (auto& pr : GetProperties((UStruct*)GetClass((UObject*)pawn), true)) {
            // Only real object properties: reading an arbitrary offset as a UObject* and
            // validating it dereferences garbage and takes the process down.
            if (pr.TypeName.find("ObjectProperty") == std::string::npos || pr.ElementSize != 8) continue;
            UObject* v = UE_FIELD(UObject*, pawn, pr.Offset);
            if (!v || !IsValidObject(v)) continue;
            std::string cn = ue::GetObjectClassName(v);
            if (!dev && cn.find("DeviceComponent") != std::string::npos) dev = v;
            if (!con && cn.find("ConsumableComponent") != std::string::npos) con = v;
        }
        listSlots("Devices", dev, es2off::UDeviceComponent::DeviceSlots, es2off::FDeviceInfo::DeviceItem, es2off::FDeviceInfo::__size);
        listSlots("Consumables", con, es2off::UConsumableComponent::ConsumableSlots, es2off::FConsumableInfo::ConsumableItem, es2off::FConsumableInfo::__size);

        // and the ship item itself, which is what the equipment UI hangs everything off
        void* sd = reinterpret_cast<char*>(pawn) + es2off::AESPawn::ShipData;
        UObject* shipItem = UE_FIELD(UObject*, sd, es2off::FShipData::ShipItemInstance);
        UObject* inv = UE_FIELD(UObject*, sd, es2off::FShipData::Inventory);
        // What the components were *supposed* to read: the inventory arrays themselves.
        if (inv && IsValidObject(inv)) {
            RawArray& d = UE_FIELD(RawArray, inv, es2off::UInventory::Devices);
            RawArray& c = UE_FIELD(RawArray, inv, es2off::UInventory::Consumables);
            out += Format("  Inventory.Devices=%d Consumables=%d\n", d.Num, c.Num);
            for (int i = 0; i < d.Num && i < 8; ++i) {
                UObject* it = ((UObject**)d.Data)[i];
                out += Format("    inv.dev[%d] %s\n", i, it && IsValidObject(it)
                              ? UE_FIELD(FName, it, es2off::UItem::ItemTemplateID).ToString().c_str() : "NULL");
            }
            for (int i = 0; i < c.Num && i < 8; ++i) {
                UObject* it = ((UObject**)c.Data)[i];
                out += Format("    inv.con[%d] %s\n", i, it && IsValidObject(it)
                              ? UE_FIELD(FName, it, es2off::UItem::ItemTemplateID).ToString().c_str() : "NULL");
            }
        }
        out += Format("  ShipItemInstance: %s | Inventory: %s\n",
                      shipItem && IsValidObject(shipItem) ? UE_FIELD(FName, shipItem, es2off::UItem::ItemTemplateID).ToString().c_str() : "NULL",
                      inv && IsValidObject(inv) ? GetName(inv).c_str() : "NULL");
    } else if (sub == "repair") {
        // Hull does not regenerate in ES2, so a save parked at 1% starts every session one hit from
        // death. Run on the HOST: it owns every player's pawn, and repairing a client's own copy would
        // just be overwritten by the next HP mirror a tenth of a second later.
        if (coop::CurrentRole() == coop::Role::Client) { out += "run this on the host — it owns every pawn\n"; return; }
        std::string who = a.size() > 2 ? a[2] : "all";
        for (auto* pl : players::All()) {
            if (who != "all" && pl->id != atoi(who.c_str())) continue;
            if (!pl->pawn) continue;
            for (const char* cls : {"HealthComponent", "ArmorComponent", "ShieldComponent"}) {
                if (UObject* comp = ComponentOfClass(pl->pawn, cls)) {
                    if (UFunction* fn = FindFunction(comp, "SetCurrentHitpointsWithRatio")) {
                        float r = 1.f; ProcessEvent(comp, fn, &r);
                    }
                }
            }
            out += Format("repaired player %d (%s)\n", pl->id, GetName((UObject*)pl->pawn).c_str());
        }
    } else if (sub == "repairjoin") {
        if (a.size() > 2) g_repairOnJoin = a[2] == "1";
        out += Format("repairOnJoin=%d\n", (int)g_repairOnJoin);
    } else if (sub == "rate") {
        if (a.size() > 2 && atoi(a[2].c_str()) > 0) g_chunksPerTick = atoi(a[2].c_str());
        out += Format("chunksPerTick=%d\n", g_chunksPerTick);
    } else if (sub == "on")  { g_enabled = true;  out += "loadout substitution ON\n"; }
    else if (sub == "off") { g_enabled = false; out += "loadout substitution OFF\n"; }
    else out += "usage: shipdata [info|export|send|stash|apply <id>|weapons|devices|local|tweak <a> <b>|repair [id|all]|repairjoin 0/1|rate N|on|off]\n";
}

void Register() {
    console::Register("reticle", "reticle - why the client's reticle is or is not updating", CmdReticle);
    console::Register("shipdata", "shipdata [info|export|send|stash|apply <id>|weapons|devices|local|repair [id|all]|repairjoin 0/1|rate N|on|off] - per-player ship loadout", CmdShipData);
    console::Register("ships", "ships [index] - list the ships this player owns / pick the one to fly", CmdShips);
}

void OnInit() {
    hooks::Install("UInventoryLib::GetCurrentShip", es2rva::UInventoryLib_GetCurrentShip, (void*)&H_GetCurrentShip, (void**)&o_GetCurrentShip);
    hooks::Install("AESPawn::PreInitializeComponents", es2rva::AESPawn_PreInitializeComponents,
                   (void*)&H_PreInitComponents, (void**)&o_PreInitComponents);
    hooks::Install("AESPawn::PostInitializeComponents", es2rva::AESPawn_PostInitializeComponents,
                   (void*)&H_PostInitComponents, (void**)&o_PostInitComponents);
    hooks::Install("AESPawn::BeginPlay", es2rva::AESPawn_BeginPlay, (void*)&H_BeginPlay, (void**)&o_BeginPlay);
}

// client: send our ship once we are connected and flying
void MaybeSendOnJoin() {
    if (g_sentThisSession) return;
    // Don't wait for a pawn: the export reads this process's UPlayerData (via GetCurrentShip), not the
    // pawn, so a pawn is not a precondition — and waiting for one delayed the whole loadout by the time
    // it took the host to spawn and replicate a placeholder ship. The real precondition is simply that
    // the export succeeds, so gate on that and retry next tick if the player data is not ready yet.
    // Latching before the attempt (as this used to) meant a single early failure was never retried.
    if (!StartSend()) return;
    g_sentThisSession = true;
}
void ResetSession() {
    g_sentThisSession = false; g_pendingSend.clear(); g_sendOffset = 0;
    g_stash.clear(); g_recvBuf.clear(); g_respawn.clear(); g_applied.clear(); g_condition.clear();
}
// The two "done for this pawn" latches compare raw pointers. After a map change the allocator can hand
// the new pawn the old address, and the HUD (which lives under the GameInstance and survives the
// travel) would then never be rebound, nor the weapons rebuilt.
void OnWorldChanged() {
    g_builtFor = nullptr;
    g_lastHudPawn = nullptr;
    g_reticleNext = 0;
}
}
