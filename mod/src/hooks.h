#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hooks {
bool Init();
// Install an inline hook at exe RVA.  *original receives the trampoline.  Returns false on failure (logged).
bool Install(const char* name, uint32_t rva, void* detour, void** original);
bool Enable(const char* name, bool enable);
std::vector<std::string> List();
}
