#pragma once
#include <string>
#include <cstdarg>

namespace es2coop {
// File logger.  Log file: <dll dir>\ES2Coop\logs\es2coop-<pid>.log  (+ OutputDebugString)
void LogInit(const std::wstring& dllDir);
void Log(const char* fmt, ...);
void LogW(const wchar_t* fmt, ...);
std::wstring GetModDir();         // <dll dir>\ES2Coop
std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);
std::string Format(const char* fmt, ...);
std::string VFormat(const char* fmt, va_list ap);
}
#define LOGF(...) ::es2coop::Log(__VA_ARGS__)
