// dwmapi.dll proxy: forwards the 4 functions ES2-Win64-Shipping.exe imports to the real dwmapi.
#include <windows.h>

namespace {
HMODULE g_real = nullptr;
FARPROC Resolve(const char* name) {
    if (!g_real) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        wcscat_s(sys, MAX_PATH, L"\\dwmapi.dll");
        g_real = LoadLibraryW(sys);
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}
}

extern "C" {
__declspec(dllexport) HRESULT WINAPI DwmFlush() {
    static auto fn = (HRESULT(WINAPI*)())Resolve("DwmFlush");
    return fn ? fn() : S_OK;
}
__declspec(dllexport) HRESULT WINAPI DwmIsCompositionEnabled(BOOL* pfEnabled) {
    static auto fn = (HRESULT(WINAPI*)(BOOL*))Resolve("DwmIsCompositionEnabled");
    if (fn) return fn(pfEnabled);
    if (pfEnabled) *pfEnabled = TRUE;
    return S_OK;
}
__declspec(dllexport) HRESULT WINAPI DwmGetCompositionTimingInfo(HWND hwnd, void* pTimingInfo) {
    static auto fn = (HRESULT(WINAPI*)(HWND, void*))Resolve("DwmGetCompositionTimingInfo");
    return fn ? fn(hwnd, pTimingInfo) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI DwmSetWindowAttribute(HWND hwnd, DWORD attr, LPCVOID pv, DWORD cb) {
    static auto fn = (HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD))Resolve("DwmSetWindowAttribute");
    return fn ? fn(hwnd, attr, pv, cb) : E_NOTIMPL;
}
}
