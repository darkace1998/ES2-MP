#pragma once
#include <string>
#include <vector>
#include <functional>

namespace console {
using Args = std::vector<std::string>;
using Handler = std::function<void(const Args& args, std::string& out)>;
using TickFn = std::function<void(float dt)>;

// TCP server on 127.0.0.1. scan=true: first free port in [basePort, basePort+9]; scan=false: exactly basePort or nothing.
void Start(int basePort, bool scan = true);
int  Port();
void Register(const std::string& name, const std::string& help, Handler h, bool onGameThread = true);
void RegisterTick(const std::string& name, TickFn fn);   // called every UGameEngine::Tick on the game thread
void PumpGameThread(float dt);           // drains queued commands + tick callbacks; call from game thread only
bool IsGameThread();
std::string Dispatch(const std::string& line, bool alreadyOnGameThread = false);  // run a command line (blocks until done)
std::string HelpText();
}
