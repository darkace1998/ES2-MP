// ES2Coop — entry point.  Loaded as dwmapi.dll proxy into ES2-Win64-Shipping.exe.
#include <windows.h>
#include <string>
#include "log.h"
#include "ue.h"
#include "hooks.h"
#include "console.h"

namespace commands { void RegisterBasic(); }
namespace net { void Register(); void OnInit(); }
namespace menu { void Register(); void OnInit(); }
#include "coop.h"
#include "authority.h"
#include "combat.h"
#include "loadout.h"
#include "travel.h"
#include "world_state.h"
#include "loot.h"
#include "respawn.h"
#include "attribution.h"
#include "steamp2p.h"

namespace {
HMODULE g_self = nullptr;
bool g_initialized = false;

using Fn_UGameEngine_Tick = void (*)(void* self, float dt, bool idle);
Fn_UGameEngine_Tick g_origTick = nullptr;

void Detour_UGameEngine_Tick(void* self, float dt, bool idle) {
    console::PumpGameThread(dt);
    g_origTick(self, dt, idle);
}

std::wstring DllDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring p(path);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

DWORD WINAPI InitThread(LPVOID) {
    es2coop::LogInit(DllDir());
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    es2coop::LogW(L"host exe: %s", exe);
    es2coop::LogW(L"cmdline: %s", GetCommandLineW());

    std::string err;
    if (!ue::Init(err)) { LOGF("ue::Init failed: %s — mod disabled", err.c_str()); return 0; }
    LOGF("exe base %p, build timestamp matches PDB", (void*)ue::g_base);

    if (!hooks::Init()) return 0;
    commands::RegisterBasic();
    net::Register();
    menu::Register();
    coop::Register();
    authority::Register();
    combat::Register();
    loadout::Register();
    travel::Register();
    world_state::Register();
    loot::Register();
    respawn::Register();
    attribution::Register();
    steamp2p::Register();

    // console port: 27100 (scanning upwards for a free one) unless ES2COOP_CONSOLE_PORT pins it — then
    // it is exactly that port, so the harness and the mod can never disagree about which instance is which.
    int basePort = 27100;
    bool pinned = false;
    if (const wchar_t* env = _wgetenv(L"ES2COOP_CONSOLE_PORT")) { int p = _wtoi(env); if (p > 0) { basePort = p; pinned = true; } }
    console::Start(basePort, !pinned);

    // Wait for the engine to exist before hooking the tick (GEngine set during FEngineLoop::Init)
    for (int i = 0; i < 600 && !ue::GetEngine(); ++i) Sleep(100);
    LOGF("GEngine=%p GWorld=%p", (void*)ue::GetEngine(), (void*)ue::GetWorld());
    hooks::Install("UGameEngine::Tick", es2rva::UGameEngine_Tick, (void*)&Detour_UGameEngine_Tick, (void**)&g_origTick);
    net::OnInit();
    menu::OnInit();
    coop::OnInit();
    authority::OnInit();
    combat::OnInit();
    loadout::OnInit();
    travel::OnInit();
    world_state::OnInit();
    loot::OnInit();
    respawn::OnInit();
    attribution::OnInit();
    steamp2p::OnInit();
    g_initialized = true;
    LOGF("init complete");
    return 0;
}
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        // Only activate inside the game exe (the proxy could be picked up by other processes in the folder)
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring e(exe);
        for (auto& c : e) c = towlower(c);
        if (e.find(L"es2-win64-shipping.exe") != std::wstring::npos) {
            HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
