#include "log.h"
#include <windows.h>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace es2coop {
static FILE* g_file = nullptr;
static std::mutex g_mutex;
static std::wstring g_modDir;

std::wstring GetModDir() { return g_modDir; }

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
std::string VFormat(const char* fmt, va_list ap) {
    char buf[4096];
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap2);
    va_end(ap2);
    if (n < 0) return {};
    if (n < (int)sizeof buf) return std::string(buf, n);
    std::string s(n + 1, 0);
    vsnprintf(s.data(), n + 1, fmt, ap);
    s.resize(n);
    return s;
}
std::string Format(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::string s = VFormat(fmt, ap);
    va_end(ap);
    return s;
}

void LogInit(const std::wstring& dllDir) {
    g_modDir = dllDir + L"\\ES2Coop";
    CreateDirectoryW(g_modDir.c_str(), nullptr);
    std::wstring logDir = g_modDir + L"\\logs";
    CreateDirectoryW(logDir.c_str(), nullptr);
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s\\es2coop-%lu.log", logDir.c_str(), GetCurrentProcessId());
    g_file = _wfopen(path, L"w");
    // also maintain a "latest" copy path for convenience
    std::wstring latest = logDir + L"\\latest.txt";
    FILE* f = _wfopen(latest.c_str(), L"w");
    if (f) { fwprintf(f, L"%s\n", path); fclose(f); }
    Log("=== ES2Coop log start (pid %lu) ===", GetCurrentProcessId());
}

static void WriteLine(const std::string& line) {
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[64];
    snprintf(ts, sizeof ts, "[%02d:%02d:%02d.%03d][t%lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) { fputs(ts, g_file); fputs(line.c_str(), g_file); fputc('\n', g_file); fflush(g_file); }
    std::string dbg = std::string("[ES2Coop] ") + line + "\n";
    OutputDebugStringA(dbg.c_str());
}

void Log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::string s = VFormat(fmt, ap);
    va_end(ap);
    WriteLine(s);
}
void LogW(const wchar_t* fmt, ...) {
    wchar_t buf[4096];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(buf, 4096, fmt, ap);
    va_end(ap);
    buf[4095] = 0;
    WriteLine(WideToUtf8(buf));
}
}
