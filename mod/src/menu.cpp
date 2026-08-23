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
#include "steamp2p.h"
#include "players.h"
#include "console.h"
#include "log.h"
#include "hooks.h"
#include "ue.h"
#include <windows.h>
#include <string>
#include <vector>
#include <cstring>

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
static bool g_trace = false;      // log every Blueprint call landing on our widgets
// Creating these widgets makes their Blueprint fire OnBtnClicked once during construction/first focus.
// Taken at face value that armed hosting and threw the Steam overlay up the moment the menu appeared,
// so ignore activations until the menu has settled.
static double g_settle = 0;
// Our own SetText/SetVisibility calls re-enter the button's Blueprint, which fires OnBtnClicked again.
// Without this the toggle flipped twice per press and settled back where it started.
static bool g_selfUpdate = false;
struct SelfUpdate { SelfUpdate() { g_selfUpdate = true; } ~SelfUpdate() { g_selfUpdate = false; } };
static bool Settled() { return g_settle <= 0; }
static uint64_t g_clicks = 0;
static double g_accum = 0;
static std::string g_lastStatus;
// Set when the player picks MULTIPLAYER in the menu: the listen server can only be created once a
// real map is up, so we arm here and fire as soon as one is.
static bool g_armHost = false;
// Steam launches an accepting friend with "+connect <string>" on the command line. Like hosting, the
// join is armed rather than performed immediately: a client's own ship is rebuilt from its UPlayerData,
// so it has to load a save first — connecting straight from the menu would put it in a default ship.
static std::string g_pendingJoin;
static bool g_joinChecked = false;
// Set when the player invites a friend over Steam: the listen server that follows has to come up on
// SteamNetDriver (and with the P2P accept gate open) or the invite cannot connect to it.
static bool g_useSteamTransport = false;
// Join retry state: a friend can accept the invite well before the host has started its game.
static double g_menuNow = 0;
static double g_splashSince = 0;
static double g_nextJoinAttempt = 0;
static int g_joinAttempts = 0;
// Kept across `menu rebuild` (which deliberately clears the cache) so a rebuild replaces our entry
// instead of stacking another copy into the box.
static UObject* g_slots[players::kMaxPlayers] = {};
static UObject* g_slotBox = nullptr;                 // canvas we parented them to
static std::string g_slotText[players::kMaxPlayers];
static void OnSlotClicked(int i);
static UObject* g_injected = nullptr;
static UObject* g_injectedBox = nullptr;
// The class our buttons are instances of. A pointer match in H_ProcessEvent is only trusted for an
// object of this class: UObject memory is recycled, so a stale slot pointer could otherwise fire on
// whatever the allocator put at that address next.
static UClass* g_buttonClass = nullptr;

static void ForgetWidgets() {
    for (auto& s : g_slots) s = nullptr;
    g_slotBox = nullptr;
    g_injected = g_injectedBox = nullptr;
    g_menu = g_button = g_statusButton = nullptr;
}

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
    SelfUpdate guard;
    FString s(text);
    struct { uint8_t Text[16]; } parms{};      // FText is 16 bytes
    Rva<std::remove_pointer_t<Fn_TextFromStr>>(es2rva::FText_FromString)(&parms, &s);
    ProcessEvent(button, fn, &parms);
}

// ---------------------------------------------------------------- the lobby readout
// The menu's VerticalBox has no room to spare (a second row pushed the DLC entry off the panel), and
// UPanelWidget offers no InsertChildAt, so the entry doubles as its own status line.
static bool LobbyVisible();
static std::string LobbyLine() {
    // Just the state — who is in the lobby is what the overview on the right is for. An invitee gets a
    // state of its own: "ON" reads as "I am hosting", which is the opposite of what is about to happen,
    // and without it an accepted invite looked like nothing had happened at all.
    if (!g_pendingJoin.empty()) return "MULTIPLAYER:  JOINING A FRIEND";
    return LobbyVisible() ? "MULTIPLAYER:  ON" : "MULTIPLAYER:  OFF";
}

// ---------------------------------------------------------------- click handling
// The entry is a toggle: OFF leaves the game exactly as stock, ON means the next map the player loads
// comes up hosting. Hosting cannot simply be switched on here — EnableListenServer only creates a net
// driver when the world has none — so ON arms it and it fires when a map is actually up.
static void OnMultiplayerClicked() {
    ++g_clicks;
    if (coop::CurrentRole() != coop::Role::None) {
        LOGF("[menu] MULTIPLAYER pressed -> already in a session (%s)",
             coop::CurrentRole() == coop::Role::Host ? "host" : "client");
        return;
    }
    if (g_armHost || !g_pendingJoin.empty()) {
        g_armHost = false;
        g_pendingJoin.clear();
        g_useSteamTransport = false;             // else a later LAN host would silently come up on Steam
        steamp2p::SetConnectPresence("");        // stop advertising "Join Game" to friends
        LOGF("[menu] multiplayer OFF");
    } else {
        g_armHost = true;
        steamp2p::SetConnectPresence(steamp2p::ConnectString());
        LOGF("[menu] multiplayer ON -> the next map loaded will host");
    }
    g_lastStatus.clear();      // force the readout to refresh
}

static void H_ProcessEvent(UObject* obj, UFunction* fn, void* parms) {
    // Two pointer compares on a hot path; everything else falls straight through. On a match, confirm
    // the object really is one of our buttons before acting (the pointer may have been recycled).
    if (obj && fn && (obj == g_button || obj == g_slots[0] || obj == g_slots[1] || obj == g_slots[2] || obj == g_slots[3])
        && g_buttonClass && GetClass(obj) == g_buttonClass) {
        const std::string n = GetName((UObject*)fn);
        if (g_trace) LOGF("[menu] trace %s <- %s", obj == g_button ? "entry" : "slot", n.c_str());
        if (n == "OnBtnClicked" && Settled() && !g_selfUpdate) {
            if (obj == g_button) OnMultiplayerClicked();
            else for (int i = 0; i < players::kMaxPlayers; ++i) if (obj == g_slots[i]) { OnSlotClicked(i); break; }
        }
    }
    o_ProcessEvent(obj, fn, parms);
}

// Put our entry directly under NEW GAME. UPanelWidget has no InsertChildAt, so the only way to place
// a child at an index is to detach everything below the anchor and re-attach it in the wanted order.
// Re-adding creates fresh UVerticalBoxSlots with default layout, which would visibly change the menu's
// spacing, so each slot's settings block is copied back over the new one.
//
// ONLY the reflected settings [Size .. VerticalAlignment]. The class ends with a private
// `SVerticalBox::FSlot* Slot` (+0x58, PDB), the slot's live Slate linkage: the snapshots are taken
// before RemoveChild frees that FSlot, so copying to the end of the object stamped a dangling pointer
// into every re-added slot (and our own slot aliased the Continue button's). BuildSlot has already
// pushed the defaults into Slate by the time we run, so the restored values are re-applied through the
// slot's own setters, which is also the only way they reach the screen.
static void CopySlotSettings(UObject* dst, const uint8_t* src) {
    if (!dst || !src) return;
    constexpr uint32_t from = es2off::UVerticalBoxSlot::Size;
    constexpr uint32_t to = es2off::UVerticalBoxSlot::VerticalAlignment + 1;
    static_assert(to + 8 <= es2off::UVerticalBoxSlot::__size, "UVerticalBoxSlot layout changed: settings block must end before the FSlot pointer");
    memcpy(reinterpret_cast<uint8_t*>(dst) + from, src + from, to - from);
    auto apply = [&](const char* setter, uint32_t off, size_t n) {
        UFunction* fn = FindFunction(dst, setter);
        if (!fn) return;
        size_t sz = UE_FIELD(uint16_t, fn, es2off::UFunction::ParmsSize);
        if (sz < n) return;                       // not the signature we expect — leave it alone
        std::vector<uint8_t> parms(sz + 16, 0);
        memcpy(parms.data(), src + off, n);
        ProcessEvent(dst, fn, parms.data());
    };
    apply("SetSize", es2off::UVerticalBoxSlot::Size, 8);                         // FSlateChildSize
    apply("SetPadding", es2off::UVerticalBoxSlot::Padding, 16);                  // FMargin
    apply("SetHorizontalAlignment", es2off::UVerticalBoxSlot::HorizontalAlignment, 1);
    apply("SetVerticalAlignment", es2off::UVerticalBoxSlot::VerticalAlignment, 1);
}

static void InsertAfter(UObject* box, UObject* widget, const char* anchorName) {
    auto addChild = Rva<std::remove_pointer_t<Fn_AddChild>>(es2rva::UPanelWidget_AddChild);
    auto removeChild = Rva<std::remove_pointer_t<Fn_RemoveChild>>(es2rva::UPanelWidget_RemoveChild);
    struct RawArray { UObject** Data; int32_t Num; int32_t Max; };
    RawArray& slots = UE_FIELD(RawArray, box, es2off::UPanelWidget::Slots);

    int anchor = -1;
    std::vector<UObject*> tail;
    std::vector<std::vector<uint8_t>> tailSettings;
    std::vector<uint8_t> templateSettings;
    for (int i = 0; i < slots.Num; ++i) {
        UObject* slot = slots.Data[i];
        if (!slot || !IsValidObject(slot)) continue;
        UObject* content = UE_FIELD(UObject*, slot, es2off::UPanelSlot::Content);
        if (templateSettings.empty())
            templateSettings.assign(reinterpret_cast<uint8_t*>(slot), reinterpret_cast<uint8_t*>(slot) + es2off::UVerticalBoxSlot::__size);
        if (anchor < 0) {
            if (content && GetName(content) == anchorName) anchor = i;
            continue;
        }
        tail.push_back(content);
        tailSettings.emplace_back(reinterpret_cast<uint8_t*>(slot), reinterpret_cast<uint8_t*>(slot) + es2off::UVerticalBoxSlot::__size);
    }
    if (anchor < 0) { addChild(box, widget, nullptr); return; }   // anchor gone: fall back to the end

    for (auto it = tail.rbegin(); it != tail.rend(); ++it) if (*it) removeChild(box, *it);
    CopySlotSettings((UObject*)addChild(box, widget, nullptr), templateSettings.data());
    for (size_t i = 0; i < tail.size(); ++i) {
        if (!tail[i]) continue;
        CopySlotSettings((UObject*)addChild(box, tail[i], nullptr), tailSettings[i].data());
    }
}

// ---------------------------------------------------------------- lobby overview
//
// Four slots pinned to the top-right of the menu's root CanvasPanel. Each is another
// WG_MainMenu_Root_Button_C so it matches the rest of the menu and is clickable for free; an empty
// slot opens Steam's invite dialog carrying our connect string, so the friend who accepts is dropped
// straight into this session and no separate "join" entry is needed.

// UCanvasPanelSlot's setters are BlueprintCallable, so they can be driven by reflection instead of
// more hardcoded RVAs. Each takes a single struct of doubles (UE5 FVector2D/FAnchors are double).
static void CallDoubles(UObject* obj, const char* fname, const double* v, int n) {
    if (!obj) return;
    UFunction* fn = FindFunction(obj, fname);
    if (!fn) return;
    int sz = UE_FIELD(uint16_t, fn, es2off::UFunction::ParmsSize);
    std::vector<uint8_t> parms((size_t)sz + 32, 0);
    double* d = reinterpret_cast<double*>(parms.data());
    for (int i = 0; i < n; ++i) d[i] = v[i];
    ProcessEvent(obj, fn, parms.data());
}

// The main menu's own vertical pitch, in UMG design units (its buttons sit 80 apart).
static constexpr double kMenuRowPitch = 80.0;

static void CallBool(UObject* obj, const char* fname, bool v) {
    if (!obj) return;
    UFunction* fn = FindFunction(obj, fname);
    if (!fn) return;
    uint8_t p = v ? 1 : 0;
    ProcessEvent(obj, fn, &p);
}

static bool LobbyVisible() { return g_armHost || !g_pendingJoin.empty() || coop::CurrentRole() != coop::Role::None; }

// Split a command line on whitespace, honouring quotes — so nothing can match inside the quoted exe
// path (which on a normal install contains "SteamLibrary\steamapps\...").
static std::vector<std::string> TokenizeCmdline(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (char c : s) {
        if (c == '"') { q = !q; continue; }
        if (!q && (c == ' ' || c == '\t')) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } continue; }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// "steam.<id>:<port>", or a bare "<ip>:<port>".
static bool LooksLikeConnectAddr(const std::string& t) {
    if (t.rfind("steam.", 0) == 0) return t.find(':') != std::string::npos;
    size_t c = t.rfind(':');
    if (c == std::string::npos || c == 0 || c + 1 >= t.size()) return false;
    if (t.find('.') == std::string::npos || t.find('.') > c) return false;
    for (size_t i = 0; i < c; ++i) if (!isdigit((unsigned char)t[i]) && t[i] != '.') return false;
    for (size_t i = c + 1; i < t.size(); ++i) if (!isdigit((unsigned char)t[i])) return false;
    return true;
}

// Find the address a Steam invite launched us with.
//
// Steam appends the host's connect string to our command line VERBATIM — it does NOT add a "+connect"
// of its own. This used to look only for "+connect ", so when the host published a bare address the
// invitee started with `... ES2 steam.<id>:7777`, nothing matched, and the join was never armed with no
// hint in the log as to why. The host now publishes the full argument, and this accepts either form:
// a "+connect <addr>" pair, or a lone token that looks like a connect address (which also covers an
// invite sent by an older build).
static void CheckInviteCommandLine() {
    if (g_joinChecked) return;
    g_joinChecked = true;
    const std::string s8 = es2coop::WideToUtf8(GetCommandLineW());
    const std::vector<std::string> toks = TokenizeCmdline(s8);
    std::string addr;
    for (size_t i = 1; i + 1 < toks.size(); ++i)
        if (toks[i] == "+connect") { addr = toks[i + 1]; break; }
    if (addr.empty())                                     // start at 1: never look at the exe path
        for (size_t i = 1; i < toks.size(); ++i)
            if (LooksLikeConnectAddr(toks[i])) { addr = toks[i]; break; }
    if (addr.empty()) return;
    g_pendingJoin = addr;
    g_lastStatus.clear();                                 // make the entry redraw as "JOINING A FRIEND"
    LOGF("[menu] launched from a Steam invite -> will join '%s' once a game is loaded", addr.c_str());
}

static std::string SlotLabel(int i) {
    if (!LobbyVisible()) return {};
    std::string name = coop::RosterName(i);
    if (name.empty() && i == players::LocalId()) name = coop::LocalPlayerName();
    if (!name.empty()) return Format("%d.  %s", i + 1, name.c_str());
    return Format("%d.  + INVITE FRIEND", i + 1);
}

static void SetSlotVisible(UObject* w, bool on) {
    if (!w) return;
    // ESlateVisibility: 0 Visible, 1 Collapsed, 2 Hidden, 3 HitTestInvisible, 4 SelfHitTestInvisible
    UFunction* fn = FindFunction(w, "SetVisibility");
    if (!fn) return;
    SelfUpdate guard;
    uint8_t v = on ? 0 : 1;
    ProcessEvent(w, fn, &v);
}

static void BuildLobbySlots(UObject* canvas, UClass* buttonClass, APlayerController* pc) {
    auto create = Rva<std::remove_pointer_t<Fn_CreateWidget>>(es2rva::UWidgetBlueprintLibrary_Create);
    auto addChild = Rva<std::remove_pointer_t<Fn_AddChild>>(es2rva::UPanelWidget_AddChild);
    for (int i = 0; i < players::kMaxPlayers; ++i) {
        UObject* w = create((UObject*)GetWorld(), &buttonClass, pc);
        if (!w) continue;
        UObject* slot = (UObject*)addChild(canvas, w, nullptr);
        if (slot) {
            const double anchors[4] = {1.0, 0.0, 1.0, 0.0};   // pin to the top-right corner
            const double align[2]   = {1.0, 0.0};             // and grow leftwards/down from it
            // Use the main menu's own row pitch. An auto-sized WG_MainMenu_Root_Button_C is ~57 units
            // tall, so anything tighter overlapped — visibly so once a row was highlighted and drew its
            // focus border. 80 is what the menu itself spaces its buttons by, which also makes the two
            // lists read as one design.
            const double pos[2]     = {-60.0, 150.0 + i * kMenuRowPitch};
            CallDoubles(slot, "SetAnchors", anchors, 4);
            CallDoubles(slot, "SetAlignment", align, 2);
            CallDoubles(slot, "SetPosition", pos, 2);
            // Let the button size itself. SetSize on a point-anchored slot writes Offsets.Right/Bottom,
            // which did not take here and left every slot zero-sized (present in the tree, invisible on
            // screen); auto-size uses the widget's own desired size and matches the menu's metrics.
            CallBool(slot, "SetAutoSize", true);
        }
        g_slots[i] = w;
        g_slotText[i].clear();
        SetSlotVisible(w, LobbyVisible());
    }
    g_slotBox = canvas;
}

// The lobby slots hang off the menu's ROOT canvas and are added last, so they have the highest Z-order
// there and draw over whatever sub-page the menu puts up — opening Settings left the overview sitting on
// top of the graphics options. They have to follow the front page.
//
// ES2 answers this itself: WG_MainMenu_New_C::IsInRootMenu(). Using the game's own predicate covers every
// sub-page (Settings, Load, Save, New Game), not just the one that was reported. The obvious-looking
// alternatives do not work: the sub-pages all live in one WidgetSwitcher whose ActiveWidgetIndex is
// already the Options page while the front page is showing, and the `Visibility` UPROPERTY is the
// designer value (everything reads SelfHitTestInvisible at runtime, front page or not).
static bool MenuIsOnFrontPage() {
    if (!g_menu || !IsValidObject(g_menu)) return false;
    UFunction* fn = FindFunction(g_menu, "IsInRootMenu");
    if (!fn) return true;            // unknown build: behave as before rather than hide the overview forever
    std::vector<uint8_t> parms((size_t)UE_FIELD(uint16_t, fn, es2off::UFunction::ParmsSize) + 16, 0);
    ProcessEvent(g_menu, fn, parms.data());
    for (auto& p : GetProperties((UStruct*)fn, false))
        if ((p.Flags & 0x400 /*CPF_ReturnParm*/) && p.TypeName == "BoolProperty")
            return PropValueToString((UObject*)parms.data(), p) == "true";
    return true;
}

static void UpdateLobbySlots() {
    const bool vis = LobbyVisible() && MenuIsOnFrontPage();
    for (int i = 0; i < players::kMaxPlayers; ++i) {
        UObject* w = g_slots[i];
        if (!w || !IsValidObject(w)) continue;
        SetSlotVisible(w, vis);
        if (!vis) continue;
        std::string label = SlotLabel(i);
        if (label == g_slotText[i]) continue;
        g_slotText[i] = label;
        SetButtonText(w, label);
    }
}

static void OnSlotClicked(int i) {
    if (!coop::RosterName(i).empty()) return;           // an occupied slot is just a readout
    // An invitee is about to JOIN someone else's session: arming hosting here too made the next map
    // run `connect` and `listen` back to back, and advertised this machine's own address to friends.
    if (!g_pendingJoin.empty()) { LOGF("[menu] lobby slot %d clicked while joining %s — invites are the host's job", i + 1, g_pendingJoin.c_str()); return; }
    // Hosting has to be armed for the connect string to mean anything to the friend who accepts.
    if (coop::CurrentRole() == coop::Role::None) g_armHost = true;
    // Inviting over Steam is the one moment we know the join will arrive as Steam P2P, so this is where
    // the transport gets chosen. It cannot be done at listen time by guessing: EnableListenServer only
    // creates a net driver when the world has none, so the choice has to be made BEFORE the map loads.
    // Deliberately not tied to the MULTIPLAYER toggle itself — that also covers LAN hosting, where
    // joiners come in by IP through the console and a SteamNetDriver would lock them out.
    g_useSteamTransport = true;
    std::string cs = steamp2p::ConnectString();
    steamp2p::SetConnectPresence(cs);
    LOGF("[menu] lobby slot %d clicked -> Steam invite (%s)", i + 1, cs.empty() ? "no steam id" : cs.c_str());
    steamp2p::OpenInviteOverlay(cs);
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
    // leaves two MULTIPLAYER rows stacked in the box. Same for the four lobby slots, which used to be
    // left behind in the canvas under the fresh ones.
    if (g_injected && IsValidObject(g_injected) && g_injectedBox && IsValidObject(g_injectedBox))
        removeChild(g_injectedBox, g_injected);
    g_injected = g_injectedBox = nullptr;
    if (g_slotBox && IsValidObject(g_slotBox))
        for (UObject* s : g_slots) if (s && IsValidObject(s)) removeChild(g_slotBox, s);
    for (auto& sp : g_slots) sp = nullptr;
    g_slotBox = nullptr;
    g_buttonClass = buttonClass;

    UObject* btn = create((UObject*)GetWorld(), &buttonClass, pc);
    if (!btn) { LOGF("[menu] CreateWidget failed"); return false; }
    SetButtonText(btn, LobbyLine());
    InsertAfter(box, btn, "ButtonNewGame");

    g_menu = menu; g_button = btn; g_statusButton = btn;
    g_injected = btn; g_injectedBox = box;
    g_settle = 1.5;

    // the lobby overview hangs off the menu's root canvas, not the button box, so it can be pinned
    UObject* wt = GetObjectProp(menu, "WidgetTree");
    UObject* canvas = wt ? GetObjectProp(wt, "RootWidget") : nullptr;
    if (canvas) BuildLobbySlots(canvas, buttonClass, pc);
    else LOGF("[menu] no root canvas — lobby overview skipped");
    g_lastStatus = LobbyLine();
    LOGF("[menu] multiplayer entry added to the main menu (button=%p)", (void*)btn);
    return true;
}

// ---------------------------------------------------------------- tick
void Tick(float dt) {
    if (!g_enabled) return;
    if (g_settle > 0) g_settle -= dt;
    CheckInviteCommandLine();
    g_accum += dt;
    if (g_accum < 0.5) return;
    g_accum = 0;

    g_menuNow += 0.5;                 // this body runs at 2 Hz (see the accumulator above)

    UWorld* w = GetWorld();
    const std::string wn = WorldName(w);
    const bool inMenu = wn.find("MainMenu") != std::string::npos;
    const bool inTransition = wn.empty() || wn == "EntryMap" || wn == "EmptyTransitionMap";
    // The "PRESS ANY KEY" splash. A friend whose game was launched by an invite is sitting right here,
    // so this is where the join belongs: no keypress, no save to load, no menu to navigate.
    const bool atSplash = (wn == "EntryMap");
    if (atSplash && g_splashSince == 0) g_splashSince = g_menuNow;
    if (!atSplash) g_splashSince = 0;

    // An armed join fires from the splash as well as from a loaded map.
    //
    // It used to wait for a gameplay map because "a client's own ship is rebuilt from its UPlayerData,
    // so it has to load a save first". That premise is wrong: ES2 has already populated UPlayerData by
    // this point (for the Continue button), and a client that never loaded a save joins with its real
    // ship — verified live, the host received the 16 KB loadout blob and applied it.
    //
    // NOT from the main menu, though: a client travel out of Map_MainMenu crashes the game with a null
    // dereference. That is ES2's own doing, not ours — it reproduces with this whole module disabled
    // (`menu on 0`). So an invitee who has already pressed past the splash still has to start a game,
    // and the log says so rather than silently doing nothing.
    const bool canJoinHere = (atSplash && g_menuNow - g_splashSince >= 6.0)   // let the engine settle first
                             || (!inMenu && !inTransition);
    if (!g_pendingJoin.empty() && coop::CurrentRole() == coop::Role::None && canJoinHere) {
        if (g_menuNow >= g_nextJoinAttempt) {
            // Retry rather than fire once: the friend may well accept before the host has started its
            // game, and there is nothing to connect to until the host's map is up and listening.
            ++g_joinAttempts;
            g_nextJoinAttempt = g_menuNow + 10.0;
            g_armHost = false;        // joining and hosting are exclusive
            LOGF("[menu] joining %s from '%s' (attempt %d)", g_pendingJoin.c_str(), wn.c_str(), g_joinAttempts);
            LOGF("[menu] %s", console::Dispatch("connect " + g_pendingJoin, true).c_str());
            if (g_joinAttempts >= 18) {   // ~3 minutes
                LOGF("[menu] giving up on the invite to %s — is the host in a game yet?", g_pendingJoin.c_str());
                g_pendingJoin.clear();
                g_lastStatus.clear();
            }
        }
    } else if (!g_pendingJoin.empty() && inMenu && coop::CurrentRole() == coop::Role::None) {
        static bool told = false;
        if (!told) { told = true; LOGF("[menu] invite to %s is armed, but joining from the main menu crashes ES2 — start or load a game and it will connect", g_pendingJoin.c_str()); }
    }
    // Only a client IN THE HOST'S MAP counts as joined. A failed `open` (host not listening yet) still
    // flips the world to a client net mode for a moment before bouncing back to the splash, and taking
    // that at face value cleared the pending join and stopped the retries after one attempt.
    if (!g_pendingJoin.empty() && coop::CurrentRole() == coop::Role::Client && !inMenu && !inTransition) {
        LOGF("[menu] joined %s after %d attempt(s)", g_pendingJoin.c_str(), g_joinAttempts);
        g_pendingJoin.clear();
        g_joinAttempts = 0;
        g_lastStatus.clear();
    }
    if (g_armHost && !inMenu && !inTransition && coop::CurrentRole() == coop::Role::None) {
        g_armHost = false;
        LOGF("[menu] map '%s' is up -> starting the listen server", wn.c_str());
        // Must precede the listen: EnableListenServer only creates a net driver when the world has none,
        // so a transport chosen afterwards is silently ignored. This also opens the Steam P2P accept
        // gate, which ES2 otherwise leaves shut and which drops incoming peers without a word.
        if (g_useSteamTransport) LOGF("[menu] %s", console::Dispatch("netdriver steam", true).c_str());
        std::string r = console::Dispatch("listen 7777", true);
        LOGF("[menu] %s", r.c_str());
        std::string cs = steamp2p::ConnectString();
        steamp2p::SetConnectPresence(cs);
        // The menu flow listens on whatever `netdriver` selected (IP by default) while the invite
        // carries a steam.<id> address. Those only meet if the Steam transport was selected before
        // this listen; say so rather than let an invite fail silently.
        if (!cs.empty() && GetObjectClassName((UObject*)GetNetDriver(GetWorld())) != "SteamNetDriver")
            LOGF("[menu] WARNING: advertising %s to Steam friends but hosting on %s — Steam invites cannot connect to an IP listen server (run 'netdriver steam' before the map loads, or have friends 'connect <ip>:7777')",
                 cs.c_str(), GetObjectClassName((UObject*)GetNetDriver(GetWorld())).c_str());
    }

    // Only present in the main menu map; the widget is destroyed with it, so rebuild when it returns.
    // Once the menu object itself is gone, every widget pointer we hold is dead — forget them all, or
    // a recycled address could make an unrelated button read as one of our lobby slots.
    if (!inMenu) {
        g_menu = g_button = g_statusButton = nullptr;
        // The slots/entry are kept across a rebuild on purpose; once they are dead objects, drop them.
        bool dead = (g_injected && !IsValidObject(g_injected));
        for (UObject* s : g_slots) if (s && !IsValidObject(s)) dead = true;
        if (dead) ForgetWidgets();
        return;
    }

    if (!g_button || !IsValidObject(g_button) || !g_menu || !IsValidObject(g_menu)) {
        g_menu = g_button = g_statusButton = nullptr;
        BuildButtons();
        return;
    }
    UpdateLobbySlots();
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
    if (a.size() > 1 && a[1] == "trace") { g_trace = a.size() > 2 ? a[2] == "1" : true; out += Format("trace %d\n", (int)g_trace); return; }
    if (a.size() > 1 && a[1] == "arm") { g_armHost = true; out += "host armed for next map\n"; return; }
    out += Format("world=%s menu=%p button=%p status=%p clicks=%llu status='%s'\n",
                  WorldName(GetWorld()).c_str(), (void*)g_menu, (void*)g_button, (void*)g_statusButton,
                  (unsigned long long)g_clicks, LobbyLine().c_str());
    // frontPage is what gates the lobby overview: the slots sit on the menu's root canvas above every
    // sub-page, so they have to disappear while one is open.
    out += Format("lobbyVisible=%d frontPage=%d (slots shown=%d) pendingJoin=%s steamTransport=%d\n",
                  (int)LobbyVisible(), (int)MenuIsOnFrontPage(), (int)(LobbyVisible() && MenuIsOnFrontPage()),
                  g_pendingJoin.empty() ? "-" : g_pendingJoin.c_str(), (int)g_useSteamTransport);
}

void Register() {
    console::Register("menu", "menu [rebuild|click|on 0/1] - main-menu multiplayer entry", CmdMenu);
}

void OnInit() {
    console::RegisterTick("menu", Tick);
    hooks::Install("UObject::ProcessEvent", es2rva::UObject_ProcessEvent, (void*)&H_ProcessEvent, (void**)&o_ProcessEvent);
}
}
