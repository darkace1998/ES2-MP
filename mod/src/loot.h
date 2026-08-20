#pragma once
#include <string>
namespace loot {
void Register();
void OnInit();
bool OnClientOp(const std::string& op, const std::string& body);
}
