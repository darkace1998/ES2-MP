#include "ue.h"
#include "log.h"
#include <windows.h>
#include <unordered_map>
#include <mutex>
#include <cstring>
#include <cmath>

namespace ue {
uintptr_t g_base = 0;

// ---------------------------------------------------------------- engine function typedefs
using Fn_FName_ToString   = void (*)(const FName*, FString*);
using Fn_FName_Ctor       = FName* (*)(FName*, const wchar_t*, int);
using Fn_Malloc           = void* (*)(size_t, uint32_t);
using Fn_Free             = void (*)(void*);
using Fn_StaticFindObject = UObject* (*)(UClass*, UObject*, const wchar_t*, bool);
using Fn_StaticLoadObject = UObject* (*)(UClass*, UObject*, const wchar_t*, const wchar_t*, uint32_t, void*, bool, const void*);
using Fn_GetPathName      = void (*)(const UObject*, const UObject*, FString*);
using Fn_IsChildOf        = bool (*)(const UStruct*, const UStruct*);
using Fn_FindFunctionByName = UFunction* (*)(const UClass*, FName, int);
using Fn_ProcessEvent     = void (*)(UObject*, UFunction*, void*);
using Fn_ExecConsoleCmd   = void (*)(const UObject*, const FString*, APlayerController*);
using Fn_InternalGetNetMode = int (*)(const UWorld*);
using Fn_GetFirstLocalPC  = APlayerController* (*)(UEngine*, const UWorld*);
using Fn_GetTransform     = const FTransform* (*)(const AActor*);
using Fn_SetActorTransform= bool (*)(AActor*, const FTransform*, bool, void*, int);
using Fn_SetReplicates    = void (*)(AActor*, bool);
using Fn_GetAllActorsOfClass = void (*)(const UObject*, UClass**, TArray<AActor*>*);   // TSubclassOf<> is passed by hidden reference (non-trivial struct)
using Fn_FindPropertyByName = FProperty* (*)(const UStruct*, FName);

// ---------------------------------------------------------------- basics
void* Malloc(size_t n, uint32_t align) { return Rva<std::remove_pointer_t<Fn_Malloc>>(es2rva::FMemory_Malloc)(n, align); }
void  Free(void* p) { if (p) Rva<std::remove_pointer_t<Fn_Free>>(es2rva::FMemory_Free)(p); }

std::string FName::ToString() const {
    FString s;
    Rva<std::remove_pointer_t<Fn_FName_ToString>>(es2rva::FName_ToString)(this, &s);
    return s.ToUtf8();
}
FName FName::Make(const wchar_t* s) {
    FName n;
    Rva<std::remove_pointer_t<Fn_FName_Ctor>>(es2rva::FName_Ctor_Wide)(&n, s, 1 /*FNAME_Add*/);
    return n;
}

FString::FString(const std::string& utf8) { Set(es2coop::Utf8ToWide(utf8).c_str()); }
void FString::Set(const wchar_t* s) {
    Reset();
    if (!s) return;
    size_t len = wcslen(s);
    Num = Max = (int32_t)len + 1;
    Data = (wchar_t*)Malloc(sizeof(wchar_t) * Num, 0);
    memcpy(Data, s, sizeof(wchar_t) * Num);
}
std::string FString::ToUtf8() const { return es2coop::WideToUtf8(ToWide()); }

// ---------------------------------------------------------------- object array
static inline char* ObjItemsChunk(int chunk) {
    char* arr = reinterpret_cast<char*>(g_base + es2rva::GUObjectArray) + es2off::FUObjectArray::ObjObjects;
    auto** chunks = UE_FIELD(char**, arr, es2off::FChunkedFixedUObjectArray::Objects);
    return chunks ? chunks[chunk] : nullptr;
}
int32_t NumObjects() {
    char* arr = reinterpret_cast<char*>(g_base + es2rva::GUObjectArray) + es2off::FUObjectArray::ObjObjects;
    return UE_FIELD(int32_t, arr, es2off::FChunkedFixedUObjectArray::NumElements);
}
static constexpr int kItemsPerChunk = 64 * 1024;
UObject* ObjectAt(int32_t index) {
    if (index < 0 || index >= NumObjects()) return nullptr;
    char* chunk = ObjItemsChunk(index / kItemsPerChunk);
    if (!chunk) return nullptr;
    char* item = chunk + (size_t)(index % kItemsPerChunk) * es2off::FUObjectItem::__size;
    return UE_FIELD(UObject*, item, es2off::FUObjectItem::Object);
}
static int32_t ObjectItemFlags(int32_t index) {
    char* chunk = ObjItemsChunk(index / kItemsPerChunk);
    if (!chunk) return 0;
    char* item = chunk + (size_t)(index % kItemsPerChunk) * es2off::FUObjectItem::__size;
    return UE_FIELD(int32_t, item, es2off::FUObjectItem::Flags);
}
void ForEachObject(const std::function<bool(UObject*)>& fn) {
    int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o) continue;
        if (!fn(o)) break;
    }
}
static bool SafeRead(const void* p, void* out, size_t n) {
    SIZE_T got = 0;
    return p && ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}
bool IsValidObject(const UObject* o) {
    if (!o || (reinterpret_cast<uintptr_t>(o) & 7)) return false;
    int32_t idx = 0;
    if (!SafeRead(reinterpret_cast<const char*>(o) + es2off::UObjectBase::InternalIndex, &idx, sizeof idx)) return false;
    if (idx < 0 || idx >= NumObjects()) return false;
    return ObjectAt(idx) == o;
}

// ---------------------------------------------------------------- names/paths
std::string GetName(const UObject* o) { return o ? GetFName(o).ToString() : "null"; }
std::string GetPathName(const UObject* o) {
    if (!o) return "null";
    FString s;
    Rva<std::remove_pointer_t<Fn_GetPathName>>(es2rva::UObjectBaseUtility_GetPathName)(o, nullptr, &s);
    return s.ToUtf8();
}
std::string GetObjectClassName(const UObject* o) { return o ? GetName((const UObject*)GetClass(o)) : "null"; }
std::string GetFullName(const UObject* o) { return o ? GetObjectClassName(o) + " " + GetPathName(o) : "null"; }
bool IsChildOf(const UStruct* s, const UStruct* base) {
    if (!s || !base) return false;
    return Rva<std::remove_pointer_t<Fn_IsChildOf>>(es2rva::UStruct_IsChildOf)(s, base);
}
bool IsA(const UObject* o, const UClass* cls) { return o && cls && IsChildOf((const UStruct*)GetClass(o), (const UStruct*)cls); }

// ---------------------------------------------------------------- finders
static std::mutex g_classCacheMutex;
static std::unordered_map<std::string, UClass*> g_classCache;
static UClass* g_classClass = nullptr;   // UClass::StaticClass()

static UClass* ResolveClassClass() {
    if (g_classClass) return g_classClass;
    // The class of any UClass object is "Class"; find via the first object whose class's class is itself.
    ForEachObject([&](UObject* o) {
        UClass* c = GetClass(o);
        if (c && GetClass((UObject*)c) == c) { g_classClass = c; return false; }
        return true;
    });
    return g_classClass;
}

UClass* FindClass(const std::string& nameIn) {
    std::string name = nameIn;
    {
        std::lock_guard<std::mutex> lk(g_classCacheMutex);
        auto it = g_classCache.find(name);
        if (it != g_classCache.end() && IsValidObject((UObject*)it->second)) return it->second;
    }
    UClass* found = nullptr;
    if (name.rfind("/", 0) == 0) {
        found = (UClass*)FindObject(name, nullptr);
    } else {
        // accept "AESPawn"/"UShipMovementComponent" style: strip the prefix letter only if it matches A/U + Uppercase
        std::string shortName = name;
        if (shortName.size() > 2 && (shortName[0] == 'A' || shortName[0] == 'U' || shortName[0] == 'F') && isupper((unsigned char)shortName[1])) {
            // try exact first, then stripped
        }
        UClass* classClass = ResolveClassClass();
        std::string stripped = (shortName.size() > 2 && (shortName[0] == 'A' || shortName[0] == 'U') && isupper((unsigned char)shortName[1])) ? shortName.substr(1) : "";
        ForEachObject([&](UObject* o) {
            if (GetClass(o) != classClass) return true;
            std::string n = GetName(o);
            if (n == shortName || (!stripped.empty() && n == stripped)) { found = (UClass*)o; return false; }
            return true;
        });
    }
    if (found) { std::lock_guard<std::mutex> lk(g_classCacheMutex); g_classCache[name] = found; }
    return found;
}
UObject* FindObject(const std::string& pathName, UClass* cls) {
    std::wstring w = es2coop::Utf8ToWide(pathName);
    return Rva<std::remove_pointer_t<Fn_StaticFindObject>>(es2rva::StaticFindObject)(cls, nullptr, w.c_str(), false);
}
UObject* LoadObject(const std::string& pathName, UClass* cls) {
    std::wstring w = es2coop::Utf8ToWide(pathName);
    return Rva<std::remove_pointer_t<Fn_StaticLoadObject>>(es2rva::StaticLoadObject)(cls, nullptr, w.c_str(), nullptr, 0, nullptr, false, nullptr);
}
std::vector<UObject*> FindObjectsOfClass(UClass* cls, bool includeDefault) {
    std::vector<UObject*> out;
    if (!cls) return out;
    ForEachObject([&](UObject* o) {
        if (IsA(o, cls)) {
            if (!includeDefault && (GetObjectFlags(o) & 0x10 /*RF_ClassDefaultObject*/)) return true;
            out.push_back(o);
        }
        return true;
    });
    return out;
}
std::vector<AActor*> GetAllActorsOfClass(UWorld* world, UClass* cls) {
    std::vector<AActor*> out;
    if (!world || !cls) return out;
    TArray<AActor*> arr;
    UClass* clsArg = cls;
    Rva<std::remove_pointer_t<Fn_GetAllActorsOfClass>>(es2rva::UGameplayStatics_GetAllActorsOfClass)((const UObject*)world, &clsArg, &arr);
    for (int i = 0; i < arr.Num; ++i) out.push_back(arr.Data[i]);
    arr.FreeData();
    return out;
}

// ---------------------------------------------------------------- functions
UFunction* FindFunction(UObject* obj, const char* name) {
    if (!obj) return nullptr;
    std::wstring w(name, name + strlen(name));
    FName fn = FName::Make(w.c_str());
    return Rva<std::remove_pointer_t<Fn_FindFunctionByName>>(es2rva::UClass_FindFunctionByName)(GetClass(obj), fn, 1 /*IncludeSuper*/);
}
void ProcessEvent(UObject* obj, UFunction* fn, void* parms) {
    Rva<std::remove_pointer_t<Fn_ProcessEvent>>(es2rva::UObject_ProcessEvent)(obj, fn, parms);
}

// ---------------------------------------------------------------- engine/world
UEngine* GetEngine() { return *Rva<UEngine*>(es2rva::GEngine); }
UWorld* GetWorld() { return UE_FIELD(UWorld*, Rva<char>(es2rva::GWorld), es2off::UWorldProxy::World); }
UGameInstance* GetGameInstance() { UWorld* w = GetWorld(); return w ? UE_FIELD(UGameInstance*, w, es2off::UWorld::OwningGameInstance) : nullptr; }
int GetNetMode(UWorld* w) { return w ? Rva<std::remove_pointer_t<Fn_InternalGetNetMode>>(es2rva::UWorld_InternalGetNetMode)(w) : -1; }
const char* NetModeName(int nm) { switch (nm) { case 0: return "Standalone"; case 1: return "DedicatedServer"; case 2: return "ListenServer"; case 3: return "Client"; default: return "?"; } }
APlayerController* GetFirstLocalPlayerController(UWorld* w) { UEngine* e = GetEngine(); return (e && w) ? Rva<std::remove_pointer_t<Fn_GetFirstLocalPC>>(es2rva::UEngine_GetFirstLocalPlayerController)(e, w) : nullptr; }
AGameModeBase* GetGameMode(UWorld* w) { return w ? UE_FIELD(AGameModeBase*, w, es2off::UWorld::AuthorityGameMode) : nullptr; }
UNetDriver* GetNetDriver(UWorld* w) { return w ? UE_FIELD(UNetDriver*, w, es2off::UWorld::NetDriver) : nullptr; }
std::string WorldName(UWorld* w) { return w ? GetName((UObject*)w) : "null"; }
bool ExecConsoleCommand(const std::string& cmd) {
    UWorld* w = GetWorld();
    if (!w) return false;
    FString s(cmd);
    Rva<std::remove_pointer_t<Fn_ExecConsoleCmd>>(es2rva::UKismetSystemLibrary_ExecuteConsoleCommand)((const UObject*)w, &s, nullptr);
    return true;
}

// ---------------------------------------------------------------- actors
FTransform GetActorTransform(AActor* a) { return *Rva<std::remove_pointer_t<Fn_GetTransform>>(es2rva::AActor_GetTransform)(a); }
bool SetActorTransform(AActor* a, const FTransform& t, bool sweep, int teleport) { return Rva<std::remove_pointer_t<Fn_SetActorTransform>>(es2rva::AActor_SetActorTransform)(a, &t, sweep, nullptr, teleport); }
void SetReplicates(AActor* a, bool b) { Rva<std::remove_pointer_t<Fn_SetReplicates>>(es2rva::AActor_SetReplicates)(a, b); }
void SetReplicateMovement(AActor* a, bool b) { Rva<std::remove_pointer_t<Fn_SetReplicates>>(es2rva::AActor_SetReplicateMovement)(a, b); }

// ---------------------------------------------------------------- reflection
std::string FieldClassName(const FField* f) {
    if (!f) return "null";
    const char* fc = UE_FIELDC(const char*, f, es2off::FField::ClassPrivate);
    if (!fc) return "?";
    return UE_FIELDC(FName, fc, es2off::FFieldClass::Name).ToString();
}
static inline FField* FieldNext(const FField* f) { return UE_FIELDC(FField*, f, es2off::FField::Next); }
static inline FName FieldName(const FField* f) { return UE_FIELDC(FName, f, es2off::FField::NamePrivate); }

std::vector<PropInfo> GetProperties(UStruct* s, bool includeSuper) {
    std::vector<PropInfo> out;
    // collect chain root-first so base-class props come first
    std::vector<UStruct*> chain;
    for (UStruct* c = s; c; c = includeSuper ? GetSuperStruct(c) : nullptr) { chain.push_back(c); if (!includeSuper) break; }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        UStruct* c = *it;
        for (FField* f = UE_FIELD(FField*, c, es2off::UStruct::ChildProperties); f; f = FieldNext(f)) {
            PropInfo p;
            p.Prop = (FProperty*)f; p.Name = FieldName(f).ToString(); p.TypeName = FieldClassName(f);
            p.Offset = UE_FIELD(int32_t, f, es2off::FProperty::Offset_Internal);
            p.ElementSize = UE_FIELD(int32_t, f, es2off::FProperty::ElementSize);
            p.ArrayDim = UE_FIELD(int32_t, f, es2off::FProperty::ArrayDim);
            p.Flags = UE_FIELD(uint64_t, f, es2off::FProperty::PropertyFlags);
            p.Owner = c;
            out.push_back(p);
        }
    }
    return out;
}
FProperty* FindProperty(UStruct* s, const std::string& name) {
    for (UStruct* c = s; c; c = GetSuperStruct(c))
        for (FField* f = UE_FIELD(FField*, c, es2off::UStruct::ChildProperties); f; f = FieldNext(f))
            if (FieldName(f).ToString() == name) return (FProperty*)f;
    return nullptr;
}

static std::string EnumValueName(UObject* uenum, int64_t value);

std::string PropValueToString(UObject* obj, const PropInfo& p, int depth) {
    char* base = reinterpret_cast<char*>(obj) + p.Offset;
    const std::string& t = p.TypeName;
    char buf[256];
    auto hex = [&](const void* ptr) { snprintf(buf, sizeof buf, "%p", ptr); return std::string(buf); };
    if (t == "BoolProperty") {
        uint8_t mask = UE_FIELD(uint8_t, p.Prop, es2off::FBoolProperty::FieldMask);
        uint8_t byteOff = UE_FIELD(uint8_t, p.Prop, es2off::FBoolProperty::ByteOffset);
        return ((*(uint8_t*)(base + byteOff)) & mask) ? "true" : "false";
    }
    if (t == "IntProperty") return std::to_string(*(int32_t*)base);
    if (t == "Int64Property") return std::to_string(*(int64_t*)base);
    if (t == "Int16Property") return std::to_string(*(int16_t*)base);
    if (t == "Int8Property") return std::to_string(*(int8_t*)base);
    if (t == "UInt32Property") return std::to_string(*(uint32_t*)base);
    if (t == "UInt64Property") return std::to_string(*(uint64_t*)base);
    if (t == "UInt16Property") return std::to_string(*(uint16_t*)base);
    if (t == "FloatProperty") { snprintf(buf, sizeof buf, "%g", *(float*)base); return buf; }
    if (t == "DoubleProperty") { snprintf(buf, sizeof buf, "%g", *(double*)base); return buf; }
    if (t == "ByteProperty") {
        UObject* en = UE_FIELD(UObject*, p.Prop, es2off::FByteProperty::Enum);
        uint8_t v = *(uint8_t*)base;
        return en ? EnumValueName(en, v) + "(" + std::to_string(v) + ")" : std::to_string(v);
    }
    if (t == "EnumProperty") {
        UObject* en = UE_FIELD(UObject*, p.Prop, es2off::FEnumProperty::Enum);
        FProperty* under = UE_FIELD(FProperty*, p.Prop, es2off::FEnumProperty::UnderlyingProp);
        int64_t v = 0; int sz = under ? UE_FIELD(int32_t, under, es2off::FProperty::ElementSize) : 1;
        memcpy(&v, base, sz > 8 ? 8 : sz);
        return (en ? EnumValueName(en, v) : "") + "(" + std::to_string(v) + ")";
    }
    if (t == "NameProperty") return ((FName*)base)->ToString();
    if (t == "StrProperty") { FString* s = (FString*)base; return "\"" + s->ToUtf8() + "\""; }
    if (t == "TextProperty") return "<FText>";
    if (t == "ObjectProperty" || t == "ClassProperty" || t == "WeakObjectProperty" || t == "LazyObjectProperty" || t == "SoftObjectProperty" || t == "SoftClassProperty" || t == "InterfaceProperty") {
        if (t == "ObjectProperty" || t == "ClassProperty") {
            UObject* o = *(UObject**)base;
            if (!o) return "None";
            return IsValidObject(o) ? GetFullName(o) + " " + hex(o) : "<invalid " + hex(o) + ">";
        }
        return "<" + t + ">";
    }
    if (t == "StructProperty") {
        UStruct* st = UE_FIELD(UStruct*, p.Prop, es2off::FStructProperty::Struct);
        std::string sn = st ? GetName((UObject*)st) : "?";
        if (sn == "Vector") { FVector* v = (FVector*)base; snprintf(buf, sizeof buf, "(%.2f, %.2f, %.2f)", v->X, v->Y, v->Z); return buf; }
        if (sn == "Rotator") { FRotator* v = (FRotator*)base; snprintf(buf, sizeof buf, "(P=%.2f Y=%.2f R=%.2f)", v->Pitch, v->Yaw, v->Roll); return buf; }
        if (sn == "Vector2D") { double* v = (double*)base; snprintf(buf, sizeof buf, "(%.2f, %.2f)", v[0], v[1]); return buf; }
        if (sn == "Guid") { uint32_t* g = (uint32_t*)base; snprintf(buf, sizeof buf, "%08X%08X%08X%08X", g[0], g[1], g[2], g[3]); return buf; }
        if (depth < 1 && st) {
            std::string s = sn + "{";
            auto props = GetProperties(st, true);
            int n = 0;
            for (auto& sp : props) { if (n++ > 12) { s += " ..."; break; } s += " " + sp.Name + "=" + PropValueToString((UObject*)base, sp, depth + 1); }
            return s + " }";
        }
        return "<" + sn + ">";
    }
    if (t == "ArrayProperty") {
        TArray<char>* arr = (TArray<char>*)base;
        FProperty* inner = UE_FIELD(FProperty*, p.Prop, es2off::FArrayProperty::Inner);
        std::string s = "[" + std::to_string(arr->Num) + "]";
        if (inner && depth < 1 && arr->Num > 0) {
            PropInfo ip; ip.Prop = inner; ip.Name = "e"; ip.TypeName = FieldClassName((FField*)inner); ip.Offset = 0;
            ip.ElementSize = UE_FIELD(int32_t, inner, es2off::FProperty::ElementSize); ip.ArrayDim = 1; ip.Flags = 0; ip.Owner = nullptr;
            s += "{";
            int n = arr->Num < 8 ? arr->Num : 8;
            for (int i = 0; i < n; ++i) s += (i ? ", " : " ") + PropValueToString((UObject*)(arr->Data + (size_t)i * ip.ElementSize), ip, depth + 1);
            if (arr->Num > n) s += ", ...";
            s += " }";
        }
        return s;
    }
    if (t == "MapProperty" || t == "SetProperty") return "<" + t + ">";
    if (t == "DelegateProperty" || t == "MulticastInlineDelegateProperty" || t == "MulticastSparseDelegateProperty") return "<delegate>";
    return "<" + t + " size " + std::to_string(p.ElementSize) + ">";
}

bool SetPropValueFromString(void* container, const PropInfo& p, const std::string& value) {
    char* base = reinterpret_cast<char*>(container) + p.Offset;
    const std::string& t = p.TypeName;
    try {
        if (t == "BoolProperty") {
            uint8_t mask = UE_FIELD(uint8_t, p.Prop, es2off::FBoolProperty::FieldMask);
            uint8_t byteOff = UE_FIELD(uint8_t, p.Prop, es2off::FBoolProperty::ByteOffset);
            bool b = (value == "1" || value == "true" || value == "True");
            uint8_t& byte = *(uint8_t*)(base + byteOff);
            if (b) byte |= mask; else byte &= ~mask;
            return true;
        }
        if (t == "IntProperty") { *(int32_t*)base = std::stoi(value); return true; }
        if (t == "Int64Property") { *(int64_t*)base = std::stoll(value); return true; }
        if (t == "UInt32Property") { *(uint32_t*)base = (uint32_t)std::stoul(value); return true; }
        if (t == "ByteProperty") { *(uint8_t*)base = (uint8_t)std::stoi(value); return true; }
        if (t == "EnumProperty") { FProperty* under = UE_FIELD(FProperty*, p.Prop, es2off::FEnumProperty::UnderlyingProp); int sz = under ? UE_FIELD(int32_t, under, es2off::FProperty::ElementSize) : 1; int64_t v = std::stoll(value); memcpy(base, &v, sz > 8 ? 8 : sz); return true; }
        if (t == "FloatProperty") { *(float*)base = std::stof(value); return true; }
        if (t == "DoubleProperty") { *(double*)base = std::stod(value); return true; }
        if (t == "NameProperty") { *(FName*)base = FName::Make(value); return true; }
        if (t == "StrProperty") { FString* s = (FString*)base; s->Reset(); s->Set(es2coop::Utf8ToWide(value).c_str()); return true; }
        if (t == "StructProperty") {
            UStruct* st = UE_FIELD(UStruct*, p.Prop, es2off::FStructProperty::Struct);
            std::string sn = st ? GetName((UObject*)st) : "?";
            if (sn == "Vector" || sn == "Rotator") { double a = 0, b = 0, c = 0; if (sscanf(value.c_str(), "%lf %lf %lf", &a, &b, &c) == 3 || sscanf(value.c_str(), "%lf,%lf,%lf", &a, &b, &c) == 3) { double* d = (double*)base; d[0] = a; d[1] = b; d[2] = c; return true; } }
        }
    } catch (...) { return false; }
    return false;
}

static std::string EnumValueName(UObject* uenum, int64_t value) {
    // UEnum::Names is TArray<TPair<FName, int64>> — layout: UField (0x30) + CppType FString (0x10) + Names TArray(0x10) ... we look it up by reflection-free offsets:
    // To stay robust we use the UEnum::GetNameByValue UFunction? Not exposed. Use known UE5.5 layout: UEnum { UField(0x30); FString CppType @0x30; TArray<TPair<FName,int64>> Names @0x40; ... }
    struct Pair { FName Name; int64_t Value; };
    TArray<Pair>* names = (TArray<Pair>*)((char*)uenum + es2off::UEnum::Names);
    if (names->Num < 0 || names->Num > 4096) return "?";
    for (int i = 0; i < names->Num; ++i) if (names->Data[i].Value == value) {
        std::string n = names->Data[i].Name.ToString();
        size_t p = n.rfind("::"); return p == std::string::npos ? n : n.substr(p + 2);
    }
    return "?";
}

// ---------------------------------------------------------------- init
bool Init(std::string& err) {
    HMODULE h = GetModuleHandleW(nullptr);
    g_base = reinterpret_cast<uintptr_t>(h);
    // verify PE timestamp matches the PDB we generated from
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    uint32_t ts = nt->FileHeader.TimeDateStamp;
    if (ts != es2rva::PE_TIMESTAMP) {
        err = es2coop::Format("exe timestamp 0x%08X != expected 0x%08X (game updated? regenerate sdk)", ts, es2rva::PE_TIMESTAMP);
        return false;
    }
    return true;
}
}
