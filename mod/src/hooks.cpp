#include "hooks.h"
#include "log.h"
#include "ue.h"
#include <windows.h>
#include <map>
#include "../third_party/minhook/include/MinHook.h"

namespace hooks {
struct Entry { uint32_t rva; void* target; void* detour; bool enabled; };
static std::map<std::string, Entry> g_hooks;

bool Init() {
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { LOGF("MH_Initialize failed: %s", MH_StatusToString(st)); return false; }
    return true;
}
bool Install(const char* name, uint32_t rva, void* detour, void** original) {
    void* target = reinterpret_cast<void*>(ue::g_base + rva);
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK) { LOGF("hook %s: MH_CreateHook(%p) failed: %s", name, target, MH_StatusToString(st)); return false; }
    st = MH_EnableHook(target);
    if (st != MH_OK) { LOGF("hook %s: MH_EnableHook failed: %s", name, MH_StatusToString(st)); return false; }
    g_hooks[name] = Entry{rva, target, detour, true};
    LOGF("hook %s installed at rva 0x%X (%p)", name, rva, target);
    return true;
}
bool Enable(const char* name, bool enable) {
    auto it = g_hooks.find(name);
    if (it == g_hooks.end()) return false;
    MH_STATUS st = enable ? MH_EnableHook(it->second.target) : MH_DisableHook(it->second.target);
    if (st == MH_OK) it->second.enabled = enable;
    return st == MH_OK;
}
std::vector<std::string> List() {
    std::vector<std::string> out;
    for (auto& [n, e] : g_hooks) out.push_back(es2coop::Format("%-48s rva=0x%X %s", n.c_str(), e.rva, e.enabled ? "on" : "off"));
    return out;
}
}
