// Generic debug commands for the TCP console.
#include "console.h"
#include "ue.h"
#include "log.h"
#include "hooks.h"
#include <windows.h>
#include <cstdlib>
#include <algorithm>

using namespace ue;
using es2coop::Format;

namespace {

UObject* ResolveObject(const std::string& spec, std::string& err) {
    if (spec.rfind("0x", 0) == 0 || spec.rfind("0X", 0) == 0) {
        UObject* o = reinterpret_cast<UObject*>(strtoull(spec.c_str(), nullptr, 16));
        if (!IsValidObject(o)) { err = "not a live UObject: " + spec; return nullptr; }
        return o;
    }
    // Aliases: say so when one resolves to nothing (a client has no game mode, a dead player no pawn),
    // instead of answering with an empty line that the harness cannot tell from success.
    auto alias = [&](UObject* o) { if (!o) err = "'" + spec + "' is null right now"; return o; };
    if (spec == "world") return alias((UObject*)GetWorld());
    if (spec == "engine") return alias((UObject*)GetEngine());
    if (spec == "gi") return alias((UObject*)GetGameInstance());
    if (spec == "pc") return alias((UObject*)GetFirstLocalPlayerController(GetWorld()));
    if (spec == "gm") return alias((UObject*)GetGameMode(GetWorld()));
    if (spec == "pawn") { APlayerController* pc = GetFirstLocalPlayerController(GetWorld()); return alias(pc ? UE_FIELD(UObject*, pc, es2off::AController::Pawn) : nullptr); }
    if (spec == "netdriver") return alias((UObject*)GetNetDriver(GetWorld()));
    UObject* o = FindObject(spec);
    if (!o) {
        // maybe a class name
        UClass* c = FindClass(spec);
        if (c) return (UObject*)c;
        err = "object not found: " + spec;
    }
    return o;
}

void CmdStatus(const console::Args&, std::string& out) {
    UWorld* w = GetWorld();
    out += Format("frame=%llu objects=%d\n", *Rva<uint64_t>(es2rva::GFrameCounter), NumObjects());
    out += Format("world=%s (%p) netmode=%s gamemode=%s\n", WorldName(w).c_str(), (void*)w, NetModeName(GetNetMode(w)), w ? GetFullName((UObject*)GetGameMode(w)).c_str() : "-");
    if (w) {
        UNetDriver* nd = GetNetDriver(w);
        out += Format("netdriver=%s\n", nd ? GetFullName((UObject*)nd).c_str() : "none");
        APlayerController* pc = GetFirstLocalPlayerController(w);
        out += Format("localPC=%s (%p)\n", pc ? GetFullName((UObject*)pc).c_str() : "none", (void*)pc);
        if (pc) {
            UObject* pawn = UE_FIELD(UObject*, pc, es2off::AController::Pawn);
            out += Format("pawn=%s (%p)\n", pawn ? GetFullName(pawn).c_str() : "none", (void*)pawn);
            if (pawn) { FTransform t = GetActorTransform((AActor*)pawn); out += Format("pawnLoc=(%.1f, %.1f, %.1f) role=%d remoteRole=%d replicates=%d\n", t.Translation.X, t.Translation.Y, t.Translation.Z, GetRole((AActor*)pawn), GetRemoteRole((AActor*)pawn), (int)GetReplicates((AActor*)pawn)); }
        }
        UGameInstance* gi = GetGameInstance();
        out += Format("gameinstance=%s\n", gi ? GetFullName((UObject*)gi).c_str() : "none");
        // levels
        TArray<UObject*>& levels = UE_FIELD(TArray<UObject*>, w, es2off::UWorld::Levels);
        out += Format("levels=%d:", levels.Num);
        for (int i = 0; i < levels.Num && i < 40; ++i) out += " " + GetName(GetOuter(levels[i]));
        out += "\n";
    }
}

void CmdExec(const console::Args& a, std::string& out) {
    std::string cmd;
    for (size_t i = 1; i < a.size(); ++i) cmd += (i > 1 ? " " : "") + a[i];
    if (cmd.empty()) { out = "usage: exec <console command>\n"; return; }
    bool ok = ExecConsoleCommand(cmd);
    out += ok ? "executed: " + cmd + "\n" : "no world\n";
}

void CmdObjects(const console::Args& a, std::string& out) {
    if (a.size() < 2) { out = "usage: objects <Class> [max=50] [filter]\n"; return; }
    UClass* c = FindClass(a[1]);
    // Blueprint classes (WG_Crosshair_C and friends) are not in the native class registry, so also
    // accept an instance -- "class 0xADDR" then reports that instance's class.
    if (!c) {
        std::string err;
        if (UObject* o = ResolveObject(a[1], err)) c = GetClass(o);
    }
    if (!c) { out = "class not found: " + a[1] + "\n"; return; }
    int max = a.size() > 2 ? atoi(a[2].c_str()) : 50;
    std::string filter = a.size() > 3 ? a[3] : "";
    int n = 0, shown = 0;
    ForEachObject([&](UObject* o) {
        if (!IsA(o, c)) return true;
        std::string fn = GetFullName(o);
        if (!filter.empty() && fn.find(filter) == std::string::npos) return true;
        ++n;
        if (shown < max) { out += Format("%p [%d] %s%s\n", (void*)o, GetInternalIndex(o), fn.c_str(), (GetObjectFlags(o) & 0x10) ? " (CDO)" : ""); ++shown; }
        return true;
    });
    out += Format("total %d matching %s\n", n, GetName((UObject*)c).c_str());
}

void CmdActors(const console::Args& a, std::string& out) {
    std::string cls = a.size() > 1 ? a[1] : "Actor";
    UClass* c = FindClass(cls);
    if (!c) { out = "class not found: " + cls + "\n"; return; }
    int max = a.size() > 2 ? atoi(a[2].c_str()) : 50;
    auto actors = GetAllActorsOfClass(GetWorld(), c);
    int shown = 0;
    for (AActor* act : actors) {
        if (shown++ >= max) break;
        FTransform t = GetActorTransform(act);
        out += Format("%p %s loc=(%.0f, %.0f, %.0f) role=%d/%d rep=%d\n", (void*)act, GetFullName((UObject*)act).c_str(), t.Translation.X, t.Translation.Y, t.Translation.Z, GetRole(act), GetRemoteRole(act), (int)GetReplicates(act));
    }
    out += Format("total %d actors of %s\n", (int)actors.size(), GetName((UObject*)c).c_str());
}

void CmdClass(const console::Args& a, std::string& out) {
    if (a.size() < 2) { out = "usage: class <Class>\n"; return; }
    UClass* c = FindClass(a[1]);
    // Blueprint classes (WG_Crosshair_C and friends) are not in the native class registry, so also
    // accept an instance -- "class 0xADDR" then reports that instance's class.
    if (!c) {
        std::string err;
        if (UObject* o = ResolveObject(a[1], err)) c = GetClass(o);
    }
    if (!c) { out = "class not found: " + a[1] + "\n"; return; }
    out += "chain:";
    for (UClass* k = c; k; k = GetSuperClass(k)) out += " " + GetName((UObject*)k);
    out += Format("\nsize=%d flags=0x%X cdo=%p\n", UE_FIELD(int32_t, c, es2off::UStruct::PropertiesSize), UE_FIELD(uint32_t, c, es2off::UClass::ClassFlags), (void*)GetDefaultObject(c));
    auto props = GetProperties((UStruct*)c, true);
    for (auto& p : props) out += Format("  0x%04X %-28s %-24s dim=%d size=%d flags=0x%llX (%s)\n", p.Offset, p.Name.c_str(), p.TypeName.c_str(), p.ArrayDim, p.ElementSize, (unsigned long long)p.Flags, GetName((UObject*)p.Owner).c_str());
    // functions: walk Children (UField*) of each struct in chain
    out += "functions:\n";
    for (UClass* k = c; k; k = GetSuperClass(k)) {
        for (UObject* f = UE_FIELD(UObject*, k, es2off::UStruct::Children); f; f = UE_FIELD(UObject*, f, es2off::UField::Next)) {
            out += Format("  %s::%s flags=0x%X parms=%d\n", GetName((UObject*)k).c_str(), GetName(f).c_str(), UE_FIELD(uint32_t, f, es2off::UFunction::FunctionFlags), (int)UE_FIELD(uint8_t, f, es2off::UFunction::NumParms));
        }
    }
}


// Raw memory read. Needed for native C++ members that have no UProperty and so never show up in
// `props` -- FWeaponInfo's spawned-instance array, for instance, which is what gates weapon switching.
void CmdPeek(const console::Args& a, std::string& out) {
    if (a.size() < 3) { out = "usage: peek <obj|0xaddr> <hexOff> [count=8] [q|d|b]\n"; return; }
    // A leading '!' reads a bare address, so heap blocks that are not UObjects (a TArray's element
    // buffer, say) can be inspected too.
    char* obj = nullptr;
    if (a[1][0] == '!') {
        obj = (char*)strtoull(a[1].c_str() + 1, nullptr, 16);
    } else {
        std::string err; UObject* o = ResolveObject(a[1], err);
        if (!o) { out = err + "\n"; return; }
        obj = (char*)o;
    }
    uint32_t off = (uint32_t)strtoul(a[2].c_str(), nullptr, 16);
    int n = a.size() > 3 ? atoi(a[3].c_str()) : 8;
    char kind = a.size() > 4 ? a[4][0] : 'q';
    if (n < 1 || n > 64) n = 8;
    char* base = obj + off;
    // Probing raw addresses is how this command earns its keep, so it must never be able to fault the
    // game: confirm the whole span is committed, readable memory first.
    {
        MEMORY_BASIC_INFORMATION mbi{};
        size_t span = (size_t)n * (kind == 'b' ? 1 : kind == 'd' ? 4 : 8);
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!VirtualQuery(base, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !(mbi.Protect & readable) ||
            (char*)mbi.BaseAddress + mbi.RegionSize < base + span) {
            out = Format("unreadable memory at %p\n", (void*)base); return;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (kind == 'b')      out += Format("  +0x%04X %02X\n", off + i, (unsigned)(uint8_t)base[i]);
        else if (kind == 'd') out += Format("  +0x%04X %d\n", off + i * 4, *(int32_t*)(base + i * 4));
        else {
            void* v = *(void**)(base + i * 8);
            out += Format("  +0x%04X %p", off + i * 8, v);
            if (v && IsValidObject((UObject*)v)) out += "  " + GetFullName((UObject*)v);
            out += "\n";
        }
    }
}

void CmdProps(const console::Args& a, std::string& out) {
    if (a.size() < 2) { out = "usage: props <obj|0xaddr|world|pc|pawn|gm|gi|engine|netdriver> [filter]\n"; return; }
    std::string err; UObject* o = ResolveObject(a[1], err);
    if (!o) { out = err + "\n"; return; }
    std::string filter = a.size() > 2 ? a[2] : "";
    out += Format("%s (%p)\n", GetFullName(o).c_str(), (void*)o);
    auto props = GetProperties((UStruct*)GetClass(o), true);
    for (auto& p : props) {
        if (!filter.empty() && p.Name.find(filter) == std::string::npos) continue;
        out += Format("  0x%04X %-30s %-20s = %s\n", p.Offset, p.Name.c_str(), p.TypeName.c_str(), PropValueToString(o, p).c_str());
    }
}

void CmdSet(const console::Args& a, std::string& out) {
    if (a.size() < 4) { out = "usage: set <obj> <prop> <value>\n"; return; }
    std::string err; UObject* o = ResolveObject(a[1], err);
    if (!o) { out = err + "\n"; return; }
    auto props = GetProperties((UStruct*)GetClass(o), true);
    for (auto& p : props) if (p.Name == a[2]) {
        std::string val; for (size_t i = 3; i < a.size(); ++i) val += (i > 3 ? " " : "") + a[i];
        bool ok = SetPropValueFromString(o, p, val);
        out += ok ? Format("%s.%s = %s\n", GetName(o).c_str(), p.Name.c_str(), PropValueToString(o, p).c_str()) : "unsupported type " + p.TypeName + "\n";
        return;
    }
    out += "property not found: " + a[2] + "\n";
}

void CmdCall(const console::Args& a, std::string& out) {
    if (a.size() < 3) { out = "usage: call <obj> <Function> [int/float args...]  (simple params only)\n"; return; }
    std::string err; UObject* o = ResolveObject(a[1], err);
    if (!o) { out = err + "\n"; return; }
    // calling a function on a class name -> use its CDO (static BlueprintFunctionLibrary functions).
    // IsA(ClassClass) rather than "its class is exactly Class", so Blueprint-generated classes count too.
    if (IsA(o, ClassClass())) { o = GetDefaultObject((UClass*)o); if (!o) { out = "class has no default object\n"; return; } }
    UFunction* fn = FindFunction(o, a[2].c_str());
    if (!fn) { out = "function not found: " + a[2] + "\n"; return; }
    int parmsSize = UE_FIELD(uint16_t, fn, es2off::UFunction::ParmsSize);
    std::vector<char> parms(parmsSize + 16, 0);
    // fill params in order from args (ints/floats/bools/strings). A UFunction's property list also
    // carries its LOCAL variables (Blueprint functions especially), which sit past ParmsSize — writing an
    // argument into one of those overran the parms buffer. Only CPF_Parm properties take arguments.
    constexpr uint64_t CPF_Parm = 0x80, CPF_ReturnParm = 0x400;
    auto props = GetProperties((UStruct*)fn, false);
    size_t ai = 3;
    for (auto& p : props) {
        if (!(p.Flags & CPF_Parm) || (p.Flags & CPF_ReturnParm)) continue;
        if (ai >= a.size()) break;
        const std::string& v = a[ai++];
        if (p.TypeName == "ObjectProperty" || p.TypeName == "ClassProperty") {
            std::string e2; UObject* po = (v == "null" || v == "0") ? nullptr : ResolveObject(v, e2);
            *(UObject**)(parms.data() + p.Offset) = po;
            if (!e2.empty()) out += "  warn: param " + p.Name + ": " + e2 + "\n";
        } else if (!SetPropValueFromString(parms.data(), p, v)) out += "  warn: could not set param " + p.Name + " (" + p.TypeName + ")\n";
    }
    if (ai < a.size()) out += Format("  warn: %d extra argument(s) ignored\n", (int)(a.size() - ai));
    ProcessEvent(o, fn, parms.data());
    out += "called " + a[2] + "\n";
    for (auto& p : props) if (p.Flags & CPF_ReturnParm) out += "  return " + PropValueToString((UObject*)parms.data(), p) + "\n";
    // ProcessEvent leaves the caller owning every parameter/return value; release the engine-allocated
    // buffers we put in (or got back) so a string-taking call does not leak an FMemory block per use.
    for (auto& p : props) {
        if (!(p.Flags & CPF_Parm)) continue;
        char* at = parms.data() + p.Offset;
        if (p.TypeName == "StrProperty") ((FString*)at)->Reset();
        else if (p.TypeName == "ArrayProperty") ((TArray<char>*)at)->FreeData();   // element buffer only
    }
}

void CmdFunc(const console::Args& a, std::string& out) {
    if (a.size() < 3) { out = "usage: func <Class> <Function> - show UFunction parameters\n"; return; }
    UClass* c = FindClass(a[1]);
    if (!c) { out = "class not found\n"; return; }
    UObject* cdo = GetDefaultObject(c);
    UFunction* fn = FindFunction(cdo, a[2].c_str());
    if (!fn) { out = "function not found\n"; return; }
    out += Format("%s flags=0x%X parmsSize=%d numParms=%d\n", GetPathName((UObject*)fn).c_str(), UE_FIELD(uint32_t, fn, es2off::UFunction::FunctionFlags), (int)UE_FIELD(uint16_t, fn, es2off::UFunction::ParmsSize), (int)UE_FIELD(uint8_t, fn, es2off::UFunction::NumParms));
    for (auto& p : GetProperties((UStruct*)fn, false)) {
        std::string extra;
        if (p.TypeName == "ObjectProperty" || p.TypeName == "ClassProperty") { UObject* pc = UE_FIELD(UObject*, p.Prop, es2off::FObjectPropertyBase::PropertyClass); extra = pc ? " of " + GetName(pc) : ""; }
        if (p.TypeName == "StructProperty") { UObject* st = UE_FIELD(UObject*, p.Prop, es2off::FStructProperty::Struct); extra = st ? " " + GetName(st) : ""; }
        if (p.TypeName == "ByteProperty" || p.TypeName == "EnumProperty") { UObject* en = UE_FIELD(UObject*, p.Prop, p.TypeName == "ByteProperty" ? es2off::FByteProperty::Enum : es2off::FEnumProperty::Enum); extra = en ? " " + GetName(en) : ""; }
        out += Format("  0x%03X %-24s %s%s%s%s\n", p.Offset, p.Name.c_str(), p.TypeName.c_str(), extra.c_str(), (p.Flags & 0x400) ? " [return]" : "", (p.Flags & 0x100) ? " [out]" : "");
    }
}

void CmdFind(const console::Args& a, std::string& out) {
    if (a.size() < 2) { out = "usage: find <substring> [max=50]\n"; return; }
    int max = a.size() > 2 ? atoi(a[2].c_str()) : 50;
    int n = 0;
    ForEachObject([&](UObject* o) {
        std::string nm = GetName(o);
        if (nm.find(a[1]) != std::string::npos) { if (n < max) out += Format("%p %s\n", (void*)o, GetFullName(o).c_str()); ++n; }
        return true;
    });
    out += Format("%d matches\n", n);
}

void CmdHooks(const console::Args&, std::string& out) { for (auto& s : hooks::List()) out += s + "\n"; }
void CmdLog(const console::Args& a, std::string& out) { std::string m; for (size_t i = 1; i < a.size(); ++i) m += (i > 1 ? " " : "") + a[i]; LOGF("[console] %s", m.c_str()); out = "ok\n"; }
void CmdPing(const console::Args&, std::string& out) { out = Format("pong (console thread %lu)\n", GetCurrentThreadId()); }
void CmdHelp(const console::Args&, std::string& out) { out = console::HelpText(); }

void CmdLoadObj(const console::Args& a, std::string& out) {
    if (a.size() < 2) { out = "usage: load <object path>  (StaticLoadObject)\n"; return; }
    UObject* o = LoadObject(a[1]);
    out += o ? Format("%p %s\n", (void*)o, GetFullName(o).c_str()) : "load failed\n";
}

void CmdCDO(const console::Args& a, std::string& out) {
    if (a.size() < 2) { out = "usage: cdo <Class> [filter]  (dump class default object props)\n"; return; }
    UClass* c = FindClass(a[1]);
    if (!c) { out = "class not found\n"; return; }
    UObject* cdo = GetDefaultObject(c);
    console::Args b = {"props", Format("0x%llX", (unsigned long long)(uintptr_t)cdo)};
    if (a.size() > 2) b.push_back(a[2]);
    CmdProps(b, out);
}

void CmdSubclasses(const console::Args& a, std::string& out) {
    if (a.size() < 2) { out = "usage: subclasses <Class> [max=100]\n"; return; }
    UClass* c = FindClass(a[1]);
    if (!c) { out = "class not found\n"; return; }
    int max = a.size() > 2 ? atoi(a[2].c_str()) : 100; int n = 0;
    // Every class object, not only native ones: Blueprint classes are instances of
    // BlueprintGeneratedClass / WidgetBlueprintGeneratedClass, which IsA(Class) covers.
    UClass* classClass = ClassClass();
    ForEachObject([&](UObject* o) {
        if (!classClass || !IsA(o, classClass)) return true;
        if (!IsChildOf((UStruct*)o, (UStruct*)c)) return true;
        if (n++ < max) out += Format("%p %s\n", (void*)o, GetPathName(o).c_str());
        return true;
    });
    out += Format("%d subclasses\n", n);
}
} // namespace

namespace commands {
void RegisterBasic() {
    console::Register("help", "list commands", CmdHelp, false);
    console::Register("ping", "liveness check (console thread)", CmdPing, false);
    console::Register("status", "world / netmode / player summary", CmdStatus);
    console::Register("exec", "exec <UE console command>", CmdExec);
    console::Register("objects", "objects <Class> [max] [filter] - list live objects of class", CmdObjects);
    console::Register("actors", "actors <Class> [max] - actors in GWorld with location/role", CmdActors);
    console::Register("class", "class <Class> - chain, properties, functions", CmdClass);
    console::Register("peek", "peek <obj|0xaddr> <hexOff> [count] [q|d|b] - raw memory", CmdPeek);
    console::Register("props", "props <obj|0xaddr|world|pc|pawn|gm|gi|engine|netdriver> [filter] - dump property values", CmdProps);
    console::Register("set", "set <obj> <prop> <value> - set a simple property", CmdSet);
    console::Register("call", "call <obj> <Function> [args] - ProcessEvent a UFunction", CmdCall);
    console::Register("find", "find <substring> [max] - objects by name substring", CmdFind);
    console::Register("func", "func <Class> <Function> - show UFunction parameters", CmdFunc);
    console::Register("load", "load <path> - StaticLoadObject", CmdLoadObj);
    console::Register("cdo", "cdo <Class> [filter] - dump class default object", CmdCDO);
    console::Register("subclasses", "subclasses <Class> [max] - list subclasses (incl. blueprints)", CmdSubclasses);
    console::Register("hooks", "list installed hooks", CmdHooks, false);
    console::Register("log", "log <text> - write to mod log", CmdLog, false);
}
}
