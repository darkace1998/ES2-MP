// Multiplayer entry point in ES2's own main menu.
//
// ES2's main menu is a Blueprint widget (WG_MainMenu_New_C) whose entries all live in one VerticalBox
// called BoxAllButtons, each entry a WG_MainMenu_Root_Button_C. Rather than draw an overlay of our own,
// we build a button of that same class, label it, and splice it into that box — so it inherits the
// game's look, hover animation, gamepad navigation and focus handling for free.
//
// Click detection: the button's activation runs through a Blueprint function (OnBtnClicked), and
// Blueprint calls all funnel through UObject::ProcessEvent. We hook that once and compare two pointers,
// which is far cheaper and far less invasive than swapping a UFunction's native entry point.
#include "menu.h"
#include "coop.h"
#include "players.h"
#include "console.h"
#include "log.h"
#include "hooks.h"
#include "ue.h"
#include <windows.h>
#include <string>
#include <vector>

using namespace ue;
using es2coop::Format;

namespace menu {

// TSubclassOf is a "by value" parameter that MSVC passes INDIRECTLY (verified by disassembly: Create
// opens with `mov rbp, qword ptr [rdx]`, dereferencing the argument). Passing the UClass* directly
// makes it read the class pointer as an address and bail out with null.
using Fn_CreateWidget = UObject* (*)(UObject* worldContext, UClass* const* widgetClass, APlayerController* owner);
using Fn_AddChild     = void* (*)(UObject* panel, UObject* child, void* slotTemplate);
using Fn_RemoveChild  = bool  (*)(UObject* panel, UObject* child);
using Fn_TextFromStr  = void  (*)(void* sretText, const FString* str);   // static => (sret, args)
using Fn_ProcessEvent = void  (*)(UObject*, UFunction*, void*);

static Fn_ProcessEvent o_ProcessEvent = nullptr;

// The menu is rebuilt whenever the player returns to it, so everything here is re-resolved rather
// than cached across worlds.
static UObject* g_menu = nullptr;        // WG_MainMenu_New_C instance
static UObject* g_button = nullptr;      // our injected entry
static UObject* g_statusButton = nullptr;// a second, non-interactive entry used as the lobby readout
static bool g_enabled = true;
static uint64_t g_clicks = 0;
static double g_accum = 0;
static std::string g_lastStatus;
// Set when the player picks MULTIPLAYER in the menu: the listen server can only be created once a
// real map is up, so we arm here and fire as soon as one is.
static bool g_armHost = false;
// Kept across `menu rebuild` (which deliberately clears the cache) so a rebuild replaces our entry
// instead of stacking another copy into the box.
static UObject* g_injected = nullptr;
static UObject* g_injectedBox = nullptr;

// ---------------------------------------------------------------- helpers
// FindClass() only matches exact UClass objects, so Blueprint classes (WidgetBlueprintGeneratedClass)
// are invisible to it. Match on the class NAME of live objects instead.
static UObject* FindLiveByClassName(const char* className, bool skipDefaults = true) {
    UObject* found = nullptr;
    ForEachObject([&](UObject* o) {
        if (skipDefaults && (GetObjectFlags(o) & 0x10 /*RF_ClassDefaultObject*/)) return true;
        UClass* c = GetClass(o);
        if (!c) return true;
        if (GetName((UObject*)c) != className) return true;
        // the live menu lives under the GameInstance; class-default widget trees do not
        if (GetPathName(o).find("Default__") != std::string::npos) return true;
        found = o;
        return false;
    });
    return found;
}

static UObject* GetObjectProp(UObject* obj, const char* name) {
    if (!obj || !IsValidObject(obj)) return nullptr;
    for (auto& p : GetProperties((UStruct*)GetClass(obj), true)) {
        if (p.Name != name || p.TypeName != "ObjectProperty") continue;
        UObject* v = UE_FIELD(UObject*, obj, p.Offset);
        return (v && IsValidObject(v)) ? v : nullptr;
    }
    return nullptr;
}

// Label a WG_MainMenu_Root_Button_C through its own SetText, so the Blueprint updates every visual
// piece that depends on the caption instead of us poking one TextBlock.
static void SetButtonText(UObject* button, const std::string& text) {
    if (!button) return;
    UFunction* fn = FindFunction(button, "SetText");
    if (!fn) return;
    FString s(text);
    struct { uint8_t Text[16]; } parms{};      // FText is 16 bytes
    Rva<std::remove_pointer_t<Fn_TextFromStr>>(es2rva::FText_FromString)(&parms, &s);
    ProcessEvent(button, fn, &parms);
}

// ---------------------------------------------------------------- the lobby readout
// The menu's VerticalBox has no room to spare (a second row pushed the DLC entry off the panel), and
// UPanelWidget offers no InsertChildAt, so the entry doubles as its own status line.
static std::string LobbyLine() {
    if (g_armHost && coop::CurrentRole() == coop::Role::None) return "MULTIPLAYER  -  HOSTING NEXT GAME";
    switch (coop::CurrentRole()) {
        case coop::Role::Host: {
            int n = players::Count();
            return Format("MULTIPLAYER  -  HOSTING (%d/4)", n < 1 ? 1 : n);
        }
        case coop::Role::Client:
            return "MULTIPLAYER  -  CONNECTED";
        default:
            return "MULTIPLAYER";
    }
}

// ---------------------------------------------------------------- click handling
static void OnMultiplayerClicked() {
    ++g_clicks;
    // Hosting from the menu has to happen before a level is loaded: EnableListenServer only creates a
    // net driver when the world has none, so arming it here means the map the player then loads (via
    // the stock Continue / Load / New Game entries) comes up already listening.
    if (coop::CurrentRole() == coop::Role::None) {
        LOGF("[menu] MULTIPLAYER pressed -> arming host mode for the next map load");
        g_armHost = true;
    } else {
        LOGF("[menu] MULTIPLAYER pressed -> already in a session (%s)",
             coop::CurrentRole() == coop::Role::Host ? "host" : "client");
    }
    g_lastStatus.clear();      // force the readout to refresh
}

static void H_ProcessEvent(UObject* obj, UFunction* fn, void* parms) {
    // Two pointer compares on a hot path; everything else falls straight through.
    if (obj && obj == g_button && fn) {
        const std::string n = GetName((UObject*)fn);
        if (n == "OnBtnClicked") OnMultiplayerClicked();
    }
    o_ProcessEvent(obj, fn, parms);
}

// ---------------------------------------------------------------- construction
static bool BuildButtons() {
    UObject* menu = FindLiveByClassName("WG_MainMenu_New_C");
    if (!menu) return false;
    UObject* box = GetObjectProp(menu, "BoxAllButtons");
    if (!box) { LOGF("[menu] main menu found but BoxAllButtons is missing"); return false; }

    // Take the class from a button that is already in the box: that avoids hardcoding an asset path
    // and keeps working if Rockfish renames or moves the Blueprint.
    UObject* templateButton = GetObjectProp(menu, "ButtonCredits");
    if (!templateButton) templateButton = GetObjectProp(menu, "ButtonOptions");
    if (!templateButton) { LOGF("[menu] no template button to clone"); return false; }
    UClass* buttonClass = GetClass(templateButton);

    APlayerController* pc = GetFirstLocalPlayerController(GetWorld());
    auto create = Rva<std::remove_pointer_t<Fn_CreateWidget>>(es2rva::UWidgetBlueprintLibrary_Create);
    auto addChild = Rva<std::remove_pointer_t<Fn_AddChild>>(es2rva::UPanelWidget_AddChild);
    auto removeChild = Rva<std::remove_pointer_t<Fn_RemoveChild>>(es2rva::UPanelWidget_RemoveChild);

    // Drop a previous entry first; otherwise a rebuild (or a second pass over the same live menu)
    // leaves two MULTIPLAYER rows stacked in the box.
    if (g_injected && IsValidObject(g_injected) && g_injectedBox && IsValidObject(g_injectedBox))
        removeChild(g_injectedBox, g_injected);
    g_injected = g_injectedBox = nullptr;

    UObject* btn = create((UObject*)GetWorld(), &buttonClass, pc);
    if (!btn) { LOGF("[menu] CreateWidget failed"); return false; }
    SetButtonText(btn, LobbyLine());
    addChild(box, btn, nullptr);

    g_menu = menu; g_button = btn; g_statusButton = btn;
    g_injected = btn; g_injectedBox = box;
    g_lastStatus = LobbyLine();
    LOGF("[menu] multiplayer entry added to the main menu (button=%p)", (void*)btn);
    return true;
}

// ---------------------------------------------------------------- tick
void Tick(float dt) {
    if (!g_enabled) return;
    g_accum += dt;
    if (g_accum < 0.5) return;
    g_accum = 0;

    UWorld* w = GetWorld();
    const std::string wn = WorldName(w);
    const bool inMenu = wn.find("MainMenu") != std::string::npos;
    const bool inTransition = wn.empty() || wn == "EntryMap" || wn == "EmptyTransitionMap";

    // The player armed hosting from the menu; do it the moment a real map is up. Reusing the console
    // path rather than duplicating it keeps this on the one code path that is already proven (it also
    // selects the net driver and sets the travel port).
    if (g_armHost && !inMenu && !inTransition && coop::CurrentRole() == coop::Role::None) {
        g_armHost = false;
        LOGF("[menu] map '%s' is up -> starting the listen server", wn.c_str());
        std::string r = console::Dispatch("listen 7777", true);
        LOGF("[menu] %s", r.c_str());
    }

    // Only present in the main menu map; the widget is destroyed with it, so rebuild when it returns.
    if (!inMenu) { g_menu = g_button = g_statusButton = nullptr; return; }

    if (!g_button || !IsValidObject(g_button) || !g_menu || !IsValidObject(g_menu)) {
        g_menu = g_button = g_statusButton = nullptr;
        BuildButtons();
        return;
    }
    std::string line = LobbyLine();
    if (line != g_lastStatus && g_statusButton && IsValidObject(g_statusButton)) {
        g_lastStatus = line;
        SetButtonText(g_statusButton, line);
    }
}

// ---------------------------------------------------------------- console
static void CmdMenu(const console::Args& a, std::string& out) {
    if (a.size() > 1 && a[1] == "rebuild") { g_menu = g_button = g_statusButton = nullptr; out += BuildButtons() ? "rebuilt\n" : "no main menu here\n"; return; }
    if (a.size() > 2 && a[1] == "on")  { g_enabled = a[2] == "1"; out += Format("menu injection %s\n", g_enabled ? "on" : "off"); return; }
    if (a.size() > 1 && a[1] == "click") { OnMultiplayerClicked(); out += "simulated click\n"; return; }
    if (a.size() > 1 && a[1] == "arm") { g_armHost = true; out += "host armed for next map\n"; return; }
    out += Format("world=%s menu=%p button=%p status=%p clicks=%llu status='%s'\n",
                  WorldName(GetWorld()).c_str(), (void*)g_menu, (void*)g_button, (void*)g_statusButton,
                  (unsigned long long)g_clicks, LobbyLine().c_str());
}

void Register() {
    console::Register("menu", "menu [rebuild|click|on 0/1] - main-menu multiplayer entry", CmdMenu);
}

void OnInit() {
    console::RegisterTick("menu", Tick);
    hooks::Install("UObject::ProcessEvent", es2rva::UObject_ProcessEvent, (void*)&H_ProcessEvent, (void**)&o_ProcessEvent);
}
}
