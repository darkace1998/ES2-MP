#include "console.h"
#include "log.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <map>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <sstream>
#include <algorithm>

namespace console {
struct Cmd { std::string help; Handler h; bool onGameThread; };
static std::map<std::string, Cmd> g_cmds;
static std::mutex g_cmdsMutex;
static std::vector<std::pair<std::string, TickFn>> g_ticks;
static std::mutex g_ticksMutex;
static int g_port = 0;
static DWORD g_gameThreadId = 0;

struct Job { std::string line; std::string out; bool done = false; std::mutex m; std::condition_variable cv; };
static std::deque<std::shared_ptr<Job>> g_queue;
static std::mutex g_queueMutex;

bool IsGameThread() { return g_gameThreadId != 0 && GetCurrentThreadId() == g_gameThreadId; }

void Register(const std::string& name, const std::string& help, Handler h, bool onGameThread) {
    std::lock_guard<std::mutex> lk(g_cmdsMutex);
    g_cmds[name] = Cmd{help, std::move(h), onGameThread};
}
void RegisterTick(const std::string& name, TickFn fn) {
    std::lock_guard<std::mutex> lk(g_ticksMutex);
    g_ticks.emplace_back(name, std::move(fn));
}
std::string HelpText() {
    std::lock_guard<std::mutex> lk(g_cmdsMutex);
    std::string s;
    for (auto& [n, c] : g_cmds) s += es2coop::Format("  %-28s %s\n", n.c_str(), c.help.c_str());
    return s;
}

static Args Tokenize(const std::string& line) {
    Args out; std::string cur; bool inq = false;
    for (char ch : line) {
        if (ch == '"') { inq = !inq; continue; }
        if (!inq && (ch == ' ' || ch == '\t')) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } continue; }
        cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Look a command up WITHOUT holding g_cmdsMutex across anything else: HelpText() takes the same
// (non-recursive) mutex, so building the "unknown command" reply inside the locked scope self-deadlocked
// the calling thread — one typo then hung every later console command and, once the game thread
// blocked on a queued job, the game itself.
static bool LookupCmd(const std::string& name, Cmd* out) {
    std::lock_guard<std::mutex> lk(g_cmdsMutex);
    auto it = g_cmds.find(name);
    if (it == g_cmds.end()) return false;
    if (out) *out = it->second;
    return true;
}

static std::string RunNow(const std::string& line) {
    Args a = Tokenize(line);
    if (a.empty()) return "";
    Cmd cmd;
    if (!LookupCmd(a[0], &cmd)) return "unknown command: " + a[0] + "\n" + HelpText();
    std::string out;
    try { cmd.h(a, out); } catch (const std::exception& e) { out += std::string("exception: ") + e.what() + "\n"; } catch (...) { out += "unknown exception\n"; }
    return out;
}

std::string Dispatch(const std::string& line, bool alreadyOnGameThread) {
    Args a = Tokenize(line);
    if (a.empty()) return "";
    Cmd cmd;
    if (!LookupCmd(a[0], &cmd)) return "unknown command: " + a[0] + "\n" + HelpText();
    if (!cmd.onGameThread || alreadyOnGameThread || IsGameThread()) return RunNow(line);
    auto job = std::make_shared<Job>();
    job->line = line;
    { std::lock_guard<std::mutex> lk(g_queueMutex); g_queue.push_back(job); }
    std::unique_lock<std::mutex> lk(job->m);
    if (!job->cv.wait_for(lk, std::chrono::seconds(30), [&] { return job->done; })) {
        // Withdraw the job if the game thread has not picked it up yet. Otherwise it would still run
        // whenever the tick resumes (after a loading screen, say) — long after the caller was told it
        // failed and probably re-issued it, so `listen`/`connect`/`LoadGame` ran twice.
        { std::lock_guard<std::mutex> q(g_queueMutex);
          auto it = std::find(g_queue.begin(), g_queue.end(), job);
          if (it != g_queue.end()) g_queue.erase(it); }
        return "ERROR: timeout waiting for game thread (is the game ticking? loading screen?)\n";
    }
    return job->out;
}

void PumpGameThread(float dt) {
    g_gameThreadId = GetCurrentThreadId();
    // commands
    for (;;) {
        std::shared_ptr<Job> job;
        { std::lock_guard<std::mutex> lk(g_queueMutex); if (g_queue.empty()) break; job = g_queue.front(); g_queue.pop_front(); }
        std::string out = RunNow(job->line);
        { std::lock_guard<std::mutex> lk(job->m); job->out = std::move(out); job->done = true; }
        job->cv.notify_all();
    }
    // ticks
    std::vector<std::pair<std::string, TickFn>> ticks;
    { std::lock_guard<std::mutex> lk(g_ticksMutex); ticks = g_ticks; }
    for (auto& [n, fn] : ticks) {
        try { fn(dt); } catch (...) { LOGF("tick %s threw", n.c_str()); }
    }
}

// ---------------------------------------------------------------- TCP server
static void ClientThread(SOCKET s) {
    std::string buf;
    char tmp[4096];
    std::string banner = es2coop::Format("ES2Coop console (pid %lu). Type 'help'. Responses end with a line '<<END>>'.\n<<END>>\n", GetCurrentProcessId());
    send(s, banner.c_str(), (int)banner.size(), 0);
    for (;;) {
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        buf.append(tmp, n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line == "quit" || line == "exit") { closesocket(s); return; }
            std::string out = Dispatch(line);
            if (out.empty() || out.back() != '\n') out += "\n";
            out += "<<END>>\n";
            const char* p = out.c_str(); int left = (int)out.size();
            while (left > 0) { int w = send(s, p, left, 0); if (w <= 0) { closesocket(s); return; } p += w; left -= w; }
        }
    }
    closesocket(s);
}

static void AcceptThread(SOCKET ls) {
    for (;;) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) { Sleep(100); continue; }
        std::thread(ClientThread, c).detach();
    }
}

int Port() { return g_port; }

void Start(int basePort, bool scan) {
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { LOGF("console: socket() failed %d", WSAGetLastError()); return; }
    BOOL one = 1; setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof one);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int port = 0;
    // When the port was requested explicitly (ES2COOP_CONSOLE_PORT, i.e. launched by the harness) bind
    // exactly that one: the scripts record the REQUESTED port, so quietly taking the next free one made
    // the harness drive a stale instance on 27100 while the fresh host sat on 27101.
    const int span = scan ? 10 : 1;
    for (int p = basePort; p < basePort + span; ++p) {
        addr.sin_port = htons((u_short)p);
        if (bind(ls, (sockaddr*)&addr, sizeof addr) == 0) { port = p; break; }
    }
    if (!port) {
        if (scan) LOGF("console: no free port in %d..%d", basePort, basePort + span - 1);
        else LOGF("console: port %d is taken (stale instance? run scripts/kill.sh) — console DISABLED, err %d", basePort, WSAGetLastError());
        closesocket(ls); return;
    }
    if (listen(ls, 4) != 0) { LOGF("console: listen failed %d", WSAGetLastError()); closesocket(ls); return; }
    g_port = port;
    // write port file for tooling
    std::wstring pf = es2coop::GetModDir() + L"\\console-" + std::to_wstring(GetCurrentProcessId()) + L".port";
    FILE* f = _wfopen(pf.c_str(), L"w"); if (f) { fprintf(f, "%d\n", port); fclose(f); }
    LOGF("console listening on 127.0.0.1:%d", port);
    std::thread(AcceptThread, ls).detach();
}
}
