// Instanced co-op loot.
//
// ES2's pickups do not replicate at all (APickupBase never sets bReplicates), and the whole collection
// pipeline is hard-wired to local player 0: ULootCollectComponent::OnBeginOverlapCollect and
// APickupBase::OnCollect both compare the collector against UGameplayStatics::GetPlayerPawn(world, 0).
// So on a client, loot simply does not exist.
//
// Rather than fight that, we mirror the *drop* and let every machine run the untouched vanilla pipeline
// on its own local copy: the host announces "an item of this template/level dropped here", each client
// spawns its own pickup, and each player's collection lands in their own UPlayerData. That is instanced
// loot — nobody can take another player's drop, which is also the friendlier co-op behaviour.
#include "loot.h"
#include "coop.h"
#include "players.h"
#include "console.h"
#include "log.h"
#include "hooks.h"
#include <windows.h>
#include <cstdlib>
#include <cstdio>

using namespace ue;
using es2coop::Format;

namespace loot {

using Fn_SpawnPickupFromItem   = AActor* (*)(UObject* wco, UObject* item, const FVector* loc, const FRotator* rot, bool* outOk);
using Fn_SpawnPickupFromItemID = AActor* (*)(UObject* wco, FName itemId, const FVector* loc, int level, const FRotator* rot, bool* outOk);
using Fn_SpawnPickups          = bool (*)(UObject* wco, const void* entries, const FVector* loc, const FRotator* rot, void* outPickups);
using Fn_GetFirstCargoItem     = UObject* (*)(UObject* inventory);

static Fn_SpawnPickupFromItem o_SpawnPickupFromItem = nullptr;
static Fn_SpawnPickups        o_SpawnPickups = nullptr;

static bool g_enabled = true;
static bool g_applying = false;          // client: we are spawning a mirrored pickup, do not re-broadcast
// SpawnPickups and SpawnPickupFromItemID both end up calling SpawnPickupFromItem, so a single drop would
// otherwise be announced several times. Only the OUTERMOST spawn call announces.
static int g_spawnDepth = 0;
struct SpawnScope { SpawnScope() { ++g_spawnDepth; } ~SpawnScope() { --g_spawnDepth; } bool outermost() const { return g_spawnDepth == 1; } };
static uint64_t g_sent = 0, g_applied = 0, g_skipped = 0;

// ---------------------------------------------------------------- helpers
static void BroadcastItem(UObject* item, const FVector& loc) {
    if (!item || !IsValidObject(item)) return;
    std::string tid = UE_FIELD(FName, item, es2off::UItem::ItemTemplateID).ToString();
    if (tid.empty() || tid == "None") { ++g_skipped; return; }
    int level = UE_FIELD(int32_t, item, es2off::UItem::ItemLevel);
    int amount = UE_FIELD(int32_t, item, es2off::UItem::Amount);
    ++g_sent;
    coop::SendToAllClients(Format("LOOT|%s|%d|%d|%.1f|%.1f|%.1f", tid.c_str(), level, amount, loc.X, loc.Y, loc.Z));
}

// ---------------------------------------------------------------- host hooks
static AActor* H_SpawnPickupFromItem(UObject* wco, UObject* item, const FVector* loc, const FRotator* rot, bool* outOk) {
    SpawnScope scope;
    AActor* r = o_SpawnPickupFromItem(wco, item, loc, rot, outOk);
    if (scope.outermost() && g_enabled && !g_applying && coop::CurrentRole() == coop::Role::Host && r && loc)
        BroadcastItem(item, *loc);
    return r;
}

static bool H_SpawnPickups(UObject* wco, const void* entries, const FVector* loc, const FRotator* rot, void* outPickups) {
    SpawnScope scope;
    bool r = o_SpawnPickups(wco, entries, loc, rot, outPickups);
    if (!scope.outermost() || !g_enabled || g_applying || coop::CurrentRole() != coop::Role::Host || !r || !outPickups) return r;
    struct RawArray { AActor** Data; int32_t Num; int32_t Max; };
    RawArray& out = *reinterpret_cast<RawArray*>(outPickups);
    if (!out.Data || out.Num <= 0 || out.Num > 512) return r;
    for (int i = 0; i < out.Num; ++i) {
        AActor* p = out.Data[i];
        if (!p || !IsValidObject((UObject*)p)) continue;
        // APickupBase::PickupEntry(+0x378).PickupInventory(+0x10) -> first cargo item
        UObject* inv = UE_FIELD(UObject*, p, es2off::APickupBase::PickupEntry + es2off::FPickupEntry::PickupInventory);
        if (!inv || !IsValidObject(inv)) continue;
        UObject* item = Rva<std::remove_pointer_t<Fn_GetFirstCargoItem>>(es2rva::UInventory_GetFirstCargoItem)(inv);
        FTransform t = GetActorTransform(p);
        BroadcastItem(item, t.Translation);
    }
    return r;
}

// ---------------------------------------------------------------- client apply
bool OnClientOp(const std::string& op, const std::string& body) {
    if (op != "LOOT") return false;
    if (!g_enabled) return true;
    char tid[128] = {0};
    int level = 0, amount = 1;
    double x = 0, y = 0, z = 0;
    if (sscanf(body.c_str(), "%127[^|]|%d|%d|%lf|%lf|%lf", tid, &level, &amount, &x, &y, &z) < 6) return true;
    UWorld* w = GetWorld();
    if (!w) return true;
    FVector loc{x, y, z};
    FRotator rot{};
    bool ok = false;
    g_applying = true;
    AActor* p = Rva<std::remove_pointer_t<Fn_SpawnPickupFromItemID>>(es2rva::UGameplayLib_SpawnPickupFromItemID)(
        (UObject*)w, FName::Make(std::string(tid)), &loc, level, &rot, &ok);
    g_applying = false;
    if (p) SetActorTransform(p, [&]{ FTransform t = GetActorTransform(p); t.Translation = loc; return t; }(), false, 1);
    if (p) ++g_applied;
    static int logged = 0;
    if (logged++ < 12)
        LOGF("[loot] mirrored '%s' lvl %d x%d at (%.0f %.0f %.0f) -> %s", tid, level, amount, x, y, z,
             p ? GetName((UObject*)p).c_str() : "FAILED");
    return true;
}

// ---------------------------------------------------------------- commands
static void CmdLoot(const console::Args& a, std::string& out) {
    if (a.size() > 2 && a[1] == "on")  g_enabled = a[2] == "1";
    if (a.size() > 1 && a[1] == "off") g_enabled = false;
    if (a.size() > 3 && a[1] == "test") {
        // loot test <templateId> <level> : spawn one locally (host will mirror it to clients)
        UWorld* w = GetWorld();
        APlayerController* pc = GetFirstLocalPlayerController(w);
        AActor* pawn = pc ? UE_FIELD(AActor*, pc, es2off::AController::Pawn) : nullptr;
        if (!w || !pawn) { out = "no pawn\n"; return; }
        FTransform t = GetActorTransform(pawn);
        FVector loc{t.Translation.X + 1500, t.Translation.Y, t.Translation.Z};
        FRotator rot{};
        bool ok = false;
        AActor* p = Rva<std::remove_pointer_t<Fn_SpawnPickupFromItemID>>(es2rva::UGameplayLib_SpawnPickupFromItemID)(
            (UObject*)w, FName::Make(a[2]), &loc, atoi(a[3].c_str()), &rot, &ok);
        out += Format("spawned %s ok=%d at (%.0f %.0f %.0f)\n", p ? GetName((UObject*)p).c_str() : "nothing", (int)ok, loc.X, loc.Y, loc.Z);
        // no explicit broadcast here: the SpawnPickupFromItem hook inside this call already announced it
    }
    UClass* c = FindClass("/Script/ES2.PickupBase");
    int n = c ? (int)GetAllActorsOfClass(GetWorld(), c).size() : -1;
    out += Format("loot mirroring=%d | sent=%llu applied=%llu skipped=%llu | pickups in this world: %d\n",
                  (int)g_enabled, (unsigned long long)g_sent, (unsigned long long)g_applied,
                  (unsigned long long)g_skipped, n);
}

void Register() {
    console::Register("loot", "loot [on 0/1|off|test <templateId> <level>] - instanced co-op loot status", CmdLoot);
}
void OnInit() {
    hooks::Install("UGameplayLib::SpawnPickupFromItem", es2rva::UGameplayLib_SpawnPickupFromItem,
                   (void*)&H_SpawnPickupFromItem, (void**)&o_SpawnPickupFromItem);
    hooks::Install("UGameplayLib::SpawnPickups", es2rva::UGameplayLib_SpawnPickups,
                   (void*)&H_SpawnPickups, (void**)&o_SpawnPickups);
}
}
