// Minimal Unreal Engine 5.5 access layer for ES2, driven by PDB-generated RVAs/offsets.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include "../../sdk/gen/rvas.h"
#include "../../sdk/gen/offsets.h"

namespace ue {

extern uintptr_t g_base;              // ES2-Win64-Shipping.exe base
template <class T> inline T* Rva(uint32_t rva) { return reinterpret_cast<T*>(g_base + rva); }
template <class T> inline T& FieldRef(void* obj, uint32_t off) { return *reinterpret_cast<T*>(reinterpret_cast<char*>(obj) + off); }
template <class T> inline const T& FieldRefC(const void* obj, uint32_t off) { return *reinterpret_cast<const T*>(reinterpret_cast<const char*>(obj) + off); }
#define UE_FIELD(T, obj, off) (::ue::FieldRef<T>((void*)(obj), (off)))
#define UE_FIELDC(T, obj, off) (::ue::FieldRefC<T>((const void*)(obj), (off)))

// ---------------------------------------------------------------- basic types
struct FName {
    uint32_t ComparisonIndex = 0;
    uint32_t Number = 0;
    bool operator==(const FName& o) const { return ComparisonIndex == o.ComparisonIndex && Number == o.Number; }
    bool IsNone() const { return ComparisonIndex == 0 && Number == 0; }
    std::string ToString() const;
    static FName Make(const wchar_t* s);       // FNAME_Add
    static FName Make(const std::string& utf8); // decodes UTF-8 (byte-widening turned every non-ASCII name into garbage)
};

void* Malloc(size_t n, uint32_t align = 0);
void  Free(void* p);

template <class T> struct TArray {
    T* Data = nullptr; int32_t Num = 0; int32_t Max = 0;
    T& operator[](int i) { return Data[i]; }
    const T& operator[](int i) const { return Data[i]; }
    T* begin() { return Data; } T* end() { return Data + Num; }
    void Reserve(int n) { if (n > Max) { T* nd = (T*)Malloc(sizeof(T) * n); if (Data) { memcpy(nd, Data, sizeof(T) * Num); Free(Data); } Data = nd; Max = n; } }
    void Add(const T& v) { if (Num == Max) Reserve(Max ? Max * 2 : 8); Data[Num++] = v; }
    void FreeData() { if (Data) Free(Data); Data = nullptr; Num = Max = 0; }
};

// FString that owns its buffer through FMemory so the engine can free/realloc it.
struct FString {
    wchar_t* Data = nullptr; int32_t Num = 0; int32_t Max = 0;
    FString() = default;
    explicit FString(const wchar_t* s) { Set(s); }
    explicit FString(const std::wstring& s) { Set(s.c_str()); }
    explicit FString(const std::string& utf8);
    FString(const FString&) = delete; FString& operator=(const FString&) = delete;
    FString(FString&& o) noexcept { Data = o.Data; Num = o.Num; Max = o.Max; o.Data = nullptr; o.Num = o.Max = 0; }
    FString& operator=(FString&& o) noexcept { if (this != &o) { Reset(); Data = o.Data; Num = o.Num; Max = o.Max; o.Data = nullptr; o.Num = o.Max = 0; } return *this; }
    ~FString() { Reset(); }
    void Reset() { if (Data) Free(Data); Data = nullptr; Num = Max = 0; }
    void Set(const wchar_t* s);
    std::wstring ToWide() const { return (Data && Num > 0) ? std::wstring(Data, Num - 1) : std::wstring(); }
    std::string ToUtf8() const;
    const wchar_t* c_str() const { return Data ? Data : L""; }
};

struct FVector { double X = 0, Y = 0, Z = 0; };
struct FRotator { double Pitch = 0, Yaw = 0, Roll = 0; };
struct alignas(16) FQuat { double X = 0, Y = 0, Z = 0, W = 1; };
struct alignas(16) FTransform { FQuat Rotation; alignas(16) FVector Translation; double _pad0 = 0; alignas(16) FVector Scale3D{1,1,1}; double _pad1 = 0; };
static_assert(sizeof(FTransform) == 96, "FTransform size");

// virtual call helper: VCall<Ret, Args...>(obj, slot, args...)
template <class Ret, class... Args> inline Ret VCall(const void* obj, int slot, Args... args) {
    using Fn = Ret (*)(const void*, Args...);
    void** vt = *reinterpret_cast<void** const*>(obj);
    return reinterpret_cast<Fn>(vt[slot])(obj, args...);
}
namespace vt { // vtable slots from the PDB (tools/pdb_types.py vtable <Class>)
    constexpr int UNetConnection_LowLevelGetRemoteAddress = 93;
    constexpr int UNetConnection_LowLevelDescribe = 94;
    constexpr int UNetConnection_Describe = 95;
    // ES2 OVERRIDES this one (UMovementRootComponent::SetSimulatePhysics), which is the class every ship's
    // CollisionRoot0 actually is — calling the UPrimitiveComponent RVA directly would run the wrong body.
    constexpr int UPrimitiveComponent_SetSimulatePhysics = 213;
}

// ---------------------------------------------------------------- UObject family (opaque; accessed via offsets)
struct UObject; struct UClass; struct UStruct; struct UFunction; struct FProperty; struct FField; struct UWorld; struct UEngine; struct AActor; struct APlayerController; struct UGameInstance; struct UNetDriver; struct UNetConnection; struct AGameModeBase;

inline UClass*  GetClass(const UObject* o)   { return UE_FIELDC(UClass*, o, es2off::UObjectBase::ClassPrivate); }
inline FName    GetFName(const UObject* o)   { return UE_FIELDC(FName, o, es2off::UObjectBase::NamePrivate); }
inline UObject* GetOuter(const UObject* o)   { return UE_FIELDC(UObject*, o, es2off::UObjectBase::OuterPrivate); }
inline int32_t  GetInternalIndex(const UObject* o) { return UE_FIELDC(int32_t, o, es2off::UObjectBase::InternalIndex); }
inline uint32_t GetObjectFlags(const UObject* o) { return UE_FIELDC(uint32_t, o, es2off::UObjectBase::ObjectFlags); }
inline UStruct* GetSuperStruct(const UStruct* s) { return UE_FIELDC(UStruct*, s, es2off::UStruct::SuperStruct); }
inline UClass*  GetSuperClass(const UClass* c)   { return (UClass*)GetSuperStruct((const UStruct*)c); }
inline UObject* GetDefaultObject(const UClass* c) { return UE_FIELDC(UObject*, c, es2off::UClass::ClassDefaultObject); }

std::string GetName(const UObject* o);
std::string GetPathName(const UObject* o);
std::string GetFullName(const UObject* o);          // "ClassName /Path/To.Object"
std::string GetObjectClassName(const UObject* o);
bool IsChildOf(const UStruct* s, const UStruct* base);
bool IsA(const UObject* o, const UClass* cls);
bool IsValidObject(const UObject* o);              // pointer is a live, dereferenceable UObject (may be pending-kill)
bool IsGarbage(const UObject* o);                  // true if not valid, or MarkAsGarbage'd / unreachable (pending GC)
UClass* ClassClass();                              // UClass::StaticClass() — IsA(o, ClassClass()) is true for every class object, Blueprint-generated ones included

// object array
int32_t NumObjects();
UObject* ObjectAt(int32_t index);                  // may be null
void ForEachObject(const std::function<bool(UObject*)>& fn);  // return false to stop

// finders (cached for classes)
UClass*  FindClass(const std::string& shortOrPathName);   // "ESPawn", "AESPawn", "/Script/ES2.ESPawn", "Actor"
UObject* FindObject(const std::string& pathName, UClass* cls = nullptr);
UObject* LoadObject(const std::string& pathName, UClass* cls = nullptr);
std::vector<UObject*> FindObjectsOfClass(UClass* cls, bool includeDefault = false);
std::vector<AActor*> GetAllActorsOfClass(UWorld* world, UClass* cls);

// functions
UFunction* FindFunction(UObject* obj, const char* name);
void ProcessEvent(UObject* obj, UFunction* fn, void* parms);

// engine/world
UEngine* GetEngine();
UWorld*  GetWorld();
UGameInstance* GetGameInstance();
int GetNetMode(UWorld* w);      // 0 standalone,1 dedicated,2 listen,3 client
const char* NetModeName(int nm);
APlayerController* GetFirstLocalPlayerController(UWorld* w);
AGameModeBase* GetGameMode(UWorld* w);
UNetDriver* GetNetDriver(UWorld* w);
std::string WorldName(UWorld* w);
bool ExecConsoleCommand(const std::string& cmd);   // via UKismetSystemLibrary::ExecuteConsoleCommand

// actor helpers
FTransform GetActorTransform(AActor* a);
bool SetActorTransform(AActor* a, const FTransform& t, bool sweep = false, int teleport = 1 /*ETeleportType::TeleportPhysics*/);
inline uint8_t GetRole(AActor* a) { return UE_FIELD(uint8_t, a, es2off::AActor::Role); }
inline uint8_t GetRemoteRole(AActor* a) { return UE_FIELD(uint8_t, a, es2off::AActor::RemoteRole); }
inline bool GetReplicates(AActor* a) { return (UE_FIELD(uint8_t, a, es2off::AActor::bReplicates_off) & es2off::AActor::bReplicates_mask) != 0; }
inline bool GetReplicateMovement(AActor* a) { return (UE_FIELD(uint8_t, a, es2off::AActor::bReplicateMovement_off) & es2off::AActor::bReplicateMovement_mask) != 0; }
void SetReplicates(AActor* a, bool b);
void SetReplicateMovement(AActor* a, bool b);

// ---------------------------------------------------------------- cross-machine actor identity
// The server-assigned FNetworkGUID is the one identity both machines agree on; payloads carry guids,
// never pointers. 0 means "not replicated (yet)".
void*    LocalGuidCache();                         // this world's net driver's FNetGUIDCache, or null
uint64_t NetGuidOf(const UObject* actor);          // 0 when unknown / unreplicated
// Resolve a guid back to an actor on THIS machine. The cache's own reverse lookup is only trustworthy
// on a client, so the answer is round-tripped (NetGuidOf(result) == guid) and otherwise found by
// scanning actors on the reliable object -> guid direction. A failed scan is remembered for a few
// frames so a caller that retries every tick (weapon ticks do) does not walk GUObjectArray per frame.
AActor*  ActorFromNetGuid(uint64_t guid);
uint64_t GuidScans();                              // how many full scans ActorFromNetGuid has run
void     ResetGuidLookup();                        // on a world change: the memo is per world

// ---------------------------------------------------------------- reflection (FProperty)
struct PropInfo {
    FProperty* Prop; std::string Name; std::string TypeName; int32_t Offset; int32_t ElementSize; int32_t ArrayDim; uint64_t Flags; UStruct* Owner;
};
std::vector<PropInfo> GetProperties(UStruct* s, bool includeSuper = true);
std::string PropValueToString(UObject* obj, const PropInfo& p, int depth = 0);   // best-effort
std::string FieldClassName(const FField* f);
FProperty* FindProperty(UStruct* s, const std::string& name);
bool SetPropValueFromString(void* container, const PropInfo& p, const std::string& value);

// ---------------------------------------------------------------- init
bool Init(std::string& err);   // resolves base, verifies build timestamp
}
