# ES2 co-op research: steam

_Auto-generated from the PDB research workflow (2026-08-19)._

## Summary

## 1. What is compiled in

**Present (full legacy stack):** `OnlineSubsystemSteam` + `SteamShared` + the legacy `ISteamNetworking` P2P socket subsystem. Confirmed classes: `USteamNetDriver`, `USteamNetConnection`, `FSocketSubsystemSteam`, `FSocketSteam`, `FInternetAddrSteam`, `FUniqueNetIdSteam`, `FOnlineSubsystemSteam`, `FOnlineSessionSteam` (incl. lobby create/join), `FOnlineAuthSteam`, `FOnlineAsyncTaskManagerSteam`, `FSteamSharedModule`.

**ABSENT:** the modern `SteamSockets` plugin — `grep -ic 'SteamNetworkingSockets|SteamNetworkingMessages|ISteamNetworkingSockets|SteamSocketsSubsystem|USteamSocketsNetDriver' sdk/symbols.tsv` returns **0**. Transport is the *legacy* `ISteamNetworking::SendP2PPacket/ReadP2PPacket` API only. `OnlineSubsystemEOS`/`SocketSubsystemEOS` are also absent (only `EOSShared`/`FEOSSDKManager` is linked), so Steam P2P is the **only** built-in NAT-traversing transport.

**steam_api64.dll is a DELAY-LOAD import** (verified by parsing the delay-import descriptor table). Imported entries / IAT VAs: `SteamInternal_ContextInit 0x1499D9378`, `SteamInternal_SteamAPI_Init 0x1499D9380`, `SteamInternal_GameServer_Init_V2 0x1499D9388`, `SteamAPI_Shutdown 0x1499D93A0`, `SteamInternal_FindOrCreateUserInterface 0x1499D93B0`, `SteamAPI_GetHSteamUser 0x1499D93B8`, `SteamAPI_RunCallbacks 0x1499D93C0`, `SteamAPI_RestartAppIfNecessary 0x1499D93D0`, `SteamAPI_RegisterCallback 0x1499D93D8`.
The DLL path is built by `FSteamSharedModule::GetSteamModulePath` (RVA `052905B0`) as `FPaths::EngineDir() / "Binaries/ThirdParty/Steamworks" / "Steamv16" / "Win64/"`; the file exists at `/home/darkace/es2game/Engine/Binaries/ThirdParty/Steamworks/Steamv16/Win64/steam_api64.dll` (300392 bytes). So: Steamworks SDK **"Steamv16" (v1.60)**, not v1.16.

### Why STEAM became the default socket subsystem (the known fact, now with the exact code)
`FOnlineSubsystemSteam::Init` (`052B12E4`) calls `CreateSteamSocketSubsystem()` (`052A2170`) at `052B13E8`. That creates/inits the `FSocketSubsystemSteam` singleton (global `FSocketSubsystemSteam::SocketSingleton` @ RVA `09E62260`) and calls `FSocketSubsystemModule::RegisterSocketSubsystem(STEAM_SUBSYSTEM, Subsystem, bMakeDefault)` via vtable slot 10 (`[vt+0x50]`, call at `052A2227`), where **bMakeDefault = `FSocketSubsystemSteam::ShouldOverrideDefaultSubsystem()`** (`052BCA64`):
`return GConfig->GetBool(L"OnlineSubsystemSteam", L"bUseSteamNetworking", bOut, GEngineIni) ? bOut : true;`
i.e. **defaults to TRUE when the key is absent** — that is exactly why plain IpNetDriver hit Steam. Immediately after, `FOnlineSubsystemSteam::bUsingSteamNetworking` (+0xDA) is set to 1 at `052B13ED`.
`-nosteam` makes `FOnlineSubsystemSteamModule::StartupModule` (`052BE060`) return before registering anything (`FParse::Param(FCommandLine::Get(), TEXT("nosteam"))`, string at VA 0x149249018).

## 2. The `steam.<SteamID64>` URL — exact resolution path (all disassembled)

**Client `open steam.<id>`:**
1. `USteamNetDriver::InitConnect` (`052B245C`): `ISocketSubsystem::Get(STEAM_SUBSYSTEM)`, then `ConnectURL.Host` (FURL+0x10) `.StartsWith(L"steam.", 6, IgnoreCase)`.
   - **Match** → `CreateUniqueSocket(FNetworkProtocolTypes::Steam, L"Unreal client (Steam)", FName("SteamClientSocket"))` + `SetSocketAndLocalAddress`, then tail-jmp `UIpNetDriver::InitConnect`.
   - **No match** → `bIsPassthrough = true` (`USteamNetDriver+0x980`) → plain UDP.
2. `USteamNetDriver::GetSocketSubsystem` (`052B0694`) returns `ISocketSubsystem::Get("WINDOWS")` when `bIsPassthrough`, else STEAM. This is a **separate override** from `UIpNetDriver::GetSocketSubsystem` (`0525112C`) — the mod's existing hook on the IpNetDriver one will NOT fire for a SteamNetDriver instance (UNetDriver vtable slot **127** = `[vt+0x3F8]`).
3. `FSocketSubsystemSteam::GetAddressFromString` (`052AAE78`): `RemoveFromStart(L"steam.", 6, IgnoreCase)`; if remainder `IsNumeric()` → CRT `_wtoi64` → `FUniqueNetIdSteam::Create<uint64>` → `FInternetAddrSteam`. **Non-numeric forwards to `ISocketSubsystem::Get(FName("WINDOWS"))->GetAddressFromString(...)`** (vtable +0x58), so an IP still works through the Steam subsystem. `FSocketSubsystemSteam::GetAddressInfo` (`052AB098`, the UE5 resolver entry point) does the same and tags the result `FNetworkProtocolTypes::Steam`.
4. `FInternetAddrSteam::SetIp(const TCHAR*, bool&)` (`052BC050`) accepts `steam.<id>`, `<id>`, or `<id>:<channel>` (splits on `:`, `_wtoi` the channel into +0x18).
5. `USteamNetConnection::InitLocalConnection` (`052B2C08`): `bIsPassthrough = !InURL.Host.StartsWith(L"steam.")`; for Steam it also sets `Resolver(+0x1E68)->[+0x4C] = 1` (address resolution off) and then `FSocketSubsystemSteam::RegisterConnection` (`052B8EF0`).
6. `USteamNetConnection::InitRemoteConnection` (`052B2CC8`) (host): `bIsPassthrough = Driver->bIsPassthrough`; after `UIpConnection::InitRemoteConnection` calls `ISocketSubsystem::Get(STEAM)->RegisterConnection(this)` **only if `UNetConnection::RemoteAddr` (+0x110) is non-null**.

**Host:** `USteamNetDriver::InitListen` (`052B27B4`): if `ListenURL.HasOption(L"bIsLanMatch")` **or** command line has `-forcepassthrough` → `bIsPassthrough = true` (plain UDP); else creates `CreateUniqueSocket(FNetworkProtocolTypes::Steam, L"Unreal server (Steam)", FName("SteamClientSocket"))`, installs it, then still tail-jmps `UIpNetDriver::InitListen`.

**Port semantics:** `FSocketSteam::Bind` (`0529DEE0`) is `SteamChannel(+0x40) = Addr.GetPort(); return true;` — the URL port is the **Steam P2P channel**, not a UDP port. Host and client must match (the mod already passes 7777 to `EnableListenServer`, so `open steam.<id>:7777` is the safe explicit form).

**Send path:** `FSocketSteam::SendTo` (`052BB72C`):
```
if (!SteamNetworkingPtr) fail;
if (Dest.SteamId == LocalSteamId) fail;   // <-- self-send blocked
SteamNetworkingPtr->SendP2PPacket(*Dest.SteamId, Data, Count, this->SteamSendMode(+0x44), Dest.SteamChannel(+0x18));
```
**Consequence: two local Proton instances on ONE Steam account can never connect over Steam P2P** — identical addresses, `SendTo` bails. Steam P2P testing needs two different Steam accounts.

### Does the host need a lobby/session? — NO (verified)
`FOnlineAsyncEventSteamConnectionRequest::Finalize` (`052A6698`) is the `P2PSessionRequest_t` handler:
```
if (Subsystem && Subsystem->bUsingSteamNetworking /*+0xDA*/)
    ((FSocketSubsystemSteam*)ISocketSubsystem::Get(STEAM_SUBSYSTEM))->AcceptP2PConnection(SteamNetworkingPtr, RemoteId);
```
`FSocketSubsystemSteam::AcceptP2PConnection` (`0529C8E4`) checks `RemoteId.IsValid()` and `!IsConnectionPendingRemoval(RemoteId,-1)`, then calls **`SteamNetworkingPtr->AcceptP2PSessionWithUser(CSteamID)`** (ISteamNetworking vtable slot **3** = `[vt+0x18]`, confirmed against the PDB vtable dump) and records the peer in `AcceptedConnections` (+0xC0).
`bUsingSteamNetworking` is set unconditionally in `FOnlineSubsystemSteam::Init` for any non-dedicated run, so **the host auto-accepts P2P sessions from any SteamID with no lobby, no session, and no game-side code.** No `CreateSession` is needed for the transport.
(The one thing not verifiable from the binary is Valve's *server-side* gate on the legacy API — Steam only delivers `P2PSessionRequest_t` between friends / lobby members / same-game-server peers. Treat "be Steam friends" as a prerequisite.)

### RELAY IS OFF BY DEFAULT — the single most important finding for "no port forwarding"
`FSocketSubsystemSteam::FSocketSubsystemSteam()` (`0529741C`) initialises `bAllowP2PPacketRelay` (+0x160) from a zeroed register at `0529753B` → **false**; `P2PConnectionTimeout`(+0x164)=45.0f (`0x42340000`), `P2PDumpInterval`(+0x170)=10.0, `P2PCleanupTimeout`(+0x178)=1.5.
`FSocketSubsystemSteam::Init` (`052B216C`) reads `[OnlineSubsystemSteam] bAllowP2PPacketRelay / P2PConnectionTimeout / P2PCleanupTimeout` from `GEngineIni` and calls `SteamNetworking()->AllowP2PPacketRelay(...)` **and** `SteamGameServerNetworking()->AllowP2PPacketRelay(...)` (ISteamNetworking vtable slot **7** = `[vt+0x38]`; calls at `052B2219` and `052B2244`).
Unless ES2's packaged `DefaultEngine.ini` sets it, **Valve's relay fallback is disabled** and only direct NAT punch-through is attempted. Force it on (see hook plan) or symmetric-NAT/CGNAT users will fail.

## 3. Steam sessions / "join friend" plumbing in ES2 — NONE
`FOnlineSessionSteam` is fully compiled in (`CreateSession(int,FName,Settings)` `052A1A08`, `JoinSession(int,FName,Result)` `052B3E2C`, `FindSessions` `052AA9F8`, `CreateLobbySession` `052A0764`, `JoinLobbySession` `052B3C78`, `JoinedLobby` `052B4118`, `GetResolvedConnectString` `052AFDA8`/`052AFE88`, `GetSteamConnectionString` `052B0744`), **but the ES2 game module never calls any of it**: `grep -iE 'CreateSession|JoinSession|FindSessions|DestroySession' sdk/es2_functions.txt` yields only `UAnalyticsLib::TrackSessionStart/End` (analytics). `UES2PlatformActivityHelper` is the console "Activities" API (StartActivity/EndActivity/ResumeActivity/SetActivityAvailability); `UPlatformSupportLib` is platform detection / saves / store / SteamDeck. **No rich-presence join-game, no invite plumbing — there is no shortcut to reuse.**
Note `FOnlineSessionSteam::GetSteamConnectionString` formats `-SteamConnectIP=%s`, and `FInternetAddrSteam::ToString(bool)` (`052C5944`) emits **`%lld:%d`** or **`%lld`** — the `steam.` prefix is only ever *parsed*, never *emitted*.

**AppID:** the game does **not** configure one. `FOnlineSubsystemSteam::Init` calls `InitSteamworksClient(bRelaunchInSteam=1, SteamAppId=0)` (`dl=1`, `r8d=0` at `052B135C..052B1365`); inside `InitSteamworksClient` (`052B2D50`) the `SteamAPI_RestartAppIfNecessary` call is guarded by `bRelaunchInSteam && SteamAppId != 0`, so it is **skipped** — launching the exe directly under Proton will not bounce through Steam. The AppID is taken from the Steam client environment afterwards: `SteamAppID(+0xDC) = SteamUtils()->GetAppID()` (`[vt+0x48]` at `052B2F28`). (The repo README records appid 1128920.) Two local instances can both `SteamAPI_Init`, but they share one SteamID64 — see the self-send block above.

## 4. Practical recommendation
1. **Keep direct-IP (current IpNetDriver + forced `ISocketSubsystem::Get("Windows")`) as the default/LAN path** — it works and is the only path testable with two local Proton instances.
2. **Add Steam P2P as an opt-in transport** for internet play (recipe below). Requires two *different* Steam accounts, so budget for a two-machine test.
3. If Steam P2P proves flaky, prefer a **userspace relay / overlay VPN** (ZeroTier, Tailscale, Radmin) over port forwarding — zero game changes, the existing direct-IP path just works on the overlay address. Port forwarding is the worst option (CGNAT breaks it entirely).
4. **Proton caveats verifiable from the binary:** steam_api64.dll is *delay-loaded* (so a mod injected early can pre-hook safely); it is a Win64 PE resolved relative to `FPaths::EngineDir()` inside the prefix; `FSteamSharedModule::LoadSteamModules` (`052908C8`) does `PushDllDirectory(GetSteamModulePath())` + `GetDllHandle(...)`. Under Proton this resolves through Steam's `steamclient` bridge and requires the process to inherit `SteamAppId`/`SteamGameId`/`SteamClientLaunch` from the Steam launch. If the launcher script starts the exe outside `%command%`, **`SteamAPI_Init` fails and `IOnlineSubsystem::Get(STEAM)` is null** — `USteamNetDriver::InitListen`/`InitConnect` then silently early-out on the null STEAM subsystem. Verify at runtime with `FSteamSharedModule::IsAvailable` (`0529083C`) / `FOnlineSubsystemSteam::GetAppId` (`052AB3C8`).

### (a) Exact Steam P2P host/join steps
**Host:** ensure Steam env is inherited → let `FOnlineSubsystemSteam::Init` run → (mod) force `bAllowP2PPacketRelay=true` → set `GEngine->NetDriverDefinitions[GameNetDriver].DriverClassName = "/Script/OnlineSubsystemSteam.SteamNetDriver"` (fallback `/Script/OnlineSubsystemUtils.IpNetDriver`) → `UGameInstance::EnableListenServer(true, 7777)` as today. Do **not** put `bIsLanMatch` in the listen URL and do **not** pass `-forcepassthrough`. Nothing else: incoming P2P is auto-accepted by the `P2PSessionRequest_t` handler. Give the host's SteamID64 to the client.
**Client:** same NetDriverDefinition change, then console `open steam.<SteamID64>:7777`.

### (b) Verified list of player-count limits to raise — see the table below.
### (c) Inventory of single-player singleton accessors — see section 6.

## 5. Player-count limits (all values read from the actual constructors)

| Thing | Where | Verified value | Action |
|---|---|---|---|
| `AGameSession::MaxPlayers` | `AGameSession+0x2AC` (int32, `globalconfig`) | **NOT set by the ctor** — `InternalConstructor<AGameSession>` (`04A9F494`) only does `MaxPartySize(+0x2B0) = -1`. MaxPlayers/MaxSpectators come from `[/Script/Engine.GameSession]` in the packaged ini (UE stock 16 / 2). Read the CDO at runtime. | write `GameMode->GameSession(+0x300)->MaxPlayers(+0x2AC) = 4` (or 0 = unlimited) |
| `AGameSession::MaxSpectators` | `+0x2A8` | same | raise if you ever spectate |
| `net.MaxPlayersOverride` cvar | `TConsoleVariableData<int>*` @ RVA `09B2A610` | `AtCapacity`: `Lim = cvar>0 ? cvar : MaxPlayers; if (Lim<=0) return false; return GetNumPlayers() >= Lim;` | easiest runtime knob: `net.MaxPlayersOverride 4` |
| `AGameSession::AtCapacity` | vtable slot **235** (`+0x758`), RVA `04AAC160` | false in `NM_Standalone`; else compares `AGameModeBase::GetNumPlayers()` (`04A37E68`, iterates `UWorld` PlayerControllerList at `+0x218`/`+0x220`, counts local **and remote** PCs) | last-resort hook: force `false` |
| `AGameSession::ApproveLogin` | vtable slot **232** (`+0x740`), RVA `04AABD74` | called from `AGameModeBase::PreLogin` (`04A442C8`) at `04A44332`; parses `?SpectatorOnly=` / `?SplitscreenCount=`; returns `L"Server full."` | |
| `AGameSession::InitOptions` | vtable slot **229** (`+0x728`), RVA `04ABD748`; invoked from `AGameModeBase::InitGame` (`04A3CE28`) at `04A3CF17` right after `SpawnActor(GameSessionClass)` | parses `?MaxPlayers=` → `+0x2AC` and `+0x2C0`, `?MaxSpectators=` → `+0x2A8`/`+0x2C8` | put `?MaxPlayers=4` in the travel URL |
| `UNetConnection` channels | `UNetConnection::InitChannelData` (`04D48B6C`) | `N = cvar(net.MaxChannelSize, data ptr @ RVA 09B2CF10); if (N<=0) { N = DefaultMaxChannelSize(+0x13E8); if (Driver->MaxChannelsOverride(+0x280) > 0) N = that; }` — ctor sets `DefaultMaxChannelSize = 0x7FFF (32767)` at `04D3A045` | **not a limiter**; leave alone |
| `FNetDriverDefinition::MaxChannelsOverride` | sizeof 28, stride `0x1C`, field `+0x18`; copied to `UNetDriver+0x280` in `UE::Private::CreateNetDriver_Local` (`05079CAC`) at `05079E63` | -1 unless the ini sets it | leave -1 |
| **Bandwidth** | `UNetDriver` ctor (`04D5BFC0`): `MaxInternetClientRate(+0xC4) = 0x2710 (10000 B/s)` at `04D5C05D`, `MaxClientRate(+0xC8) = 0x3A98 (15000 B/s)` at `04D5C067` | **the real 4-player limiter** — 10 KB/s per internet client is far too low for a busy space sim | raise both on the driver, plus `UPlayer::ConfiguredInternetSpeed(+0x3C)` / `CurrentNetSpeed(+0x38)` per client |
| ES2-side cap | `grep -iE 'MaxPlayers' sdk/es2_functions.txt` | **none** | nothing to do |

`UNetDriver::ClientConnections` is a TArray @ `+0xF8`, `MappedClientConnections` a TMap @ `+0x108` — no fixed cap.

## 6. THE BIG ONE — inventory of single-player singleton assumptions in ES2

Two root singletons; everything else hangs off them.

### A. `UESGameInstance::InstancePointer` — static global @ RVA `09AC5E88`
Process-wide pointer to the one ES2 GameInstance. **362 RIP-relative references across 304 functions** (`.text` byte-scan; a few may be false positives). Top classes: `UGameplayLib` (51), `UItemLib` (44), `UPlayerData` (26), `AESPlayerController` (23), `UKeyBinder` (14), `UInventoryLib` (13), `UUserFunctionsLib` (11), `UMissionLib` (9), `UInputLib` (7), `UMapLib` (5), `UESGameInstance` (5), `AESGameModeBase` (5), `ULocationLib` (4), `UESGameUserSettings` (4), `AMapEventManager` (4).
Accessor: `UESGameInstance::GetInstance()` (`05D6C270`) — one instruction.

### B. `UESGameInstance::PlayerData` @ `UESGameInstance+0x1C0` — ONE `UPlayerData` (sizeof 8624) per process
`UGameplayLib::GetPlayerData()` (`0127398C`) = `InstancePointer ? InstancePointer->PlayerData : nullptr`. **371 direct call sites in 342 functions.** By class: `UGameplayLib` (84), `UInventoryLib` (44), `UMapLib` (38), `UItemLib` (34), `UMissionLib` (27), `ULocationLib` (9), `AWantedLevelManager` (9), `UInventory` (6), `AMapEventManager` (6), `AESGameModeBase` (5), `ADockableStation` (5), `AChallengeBase` (5), `AActivityBase` (5), `UGameData` (4), `UDeviceComponent` (4), `UArmorComponent` (4), `UHealthComponent` (3), `AMissionTaskBase`/`AMissionBase` (6), `UWeaponComponent` (2), `UShieldComponent` (2), `UFactionComponent` (2)…
**Credits, XP/level, inventory, ships, perks, missions, map/location state, wanted level, job rank all live in this one object.** With >1 player on a host, all of it is the *host's*.

### C. Accessors that return the WRONG player on a host with clients (hard-coded local player index 0)
All unique-RVA, all reflected UFunctions (exec thunk RVAs given so BP paths are covered too):

| Function | RVA | exec thunk | What it really does |
|---|---|---|---|
| `UGameplayLib::GetESPlayerPawn(UObject*)` | `013F0CC0` | `01824964` | `Cast<AESPawn>(GetPlayerController(ctx, **0**)->GetPawnOrSpectator())` |
| `UGameplayLib::GetESPlayerController(UObject*)` | `0150D280` | `0150D170` | `Cast<AESPlayerController>(GetPlayerController(ctx, **0**))` |
| `UGameplayLib::GetESHUD(UObject*)` | `015576B8` | `015575A8` | `Cast<AESHUD>(GetPlayerController(ctx, **0**)->MyHUD)` |
| `UGameplayLib::GetPlayerControllerWithoutContext()` | `013EFE78` | `05B92368` | `InstancePointer ? GetPlayerController(InstancePointer, **0**) : nullptr` |
| `UGameplayLib::GetPlayerData()` | `0127398C` | `018C43BC` | global PlayerData (above) |
| `UGameplayLib::IsPlayerController(UObject*, AController*)` | `012D2898` | — | `GetPlayerController(ctx, **0**) == C` → **false for every remote client PC** |
| `UInventoryLib::GetCurrentShip()` | `0121A1E0` | `01805F74` | global PlayerData → current ship (66 sites / 42 fns) |
| `UInventoryLib::GetInventoryOfCurrentShip()` | `014DE7B4` | `014DE790` | global (57 sites / 43 fns) |
| `UInventoryLib::GetCurrentShipIndex()` | `01954FB8` | — | global |

`UGameplayStatics::GetPlayerController(ctx, Index)` (`013F08F0`) walks `World(+0x1D8)->OwningGameInstance` → `UGameInstance::LocalPlayers` (`+0x38` data / `+0x40` num) → `UPlayer::PlayerController (+0x30)`. **`LocalPlayers` only ever contains locally-controlled players**, so on a listen server index 0 is always the *host* and remote clients are unreachable at any index.

**Safe by comparison:** `UGameplayLib::IsPlayerPawn(AActor*)` (`012A1810`) = `Cast<AESPawn>(A) && (A->IsPlayerControlled() /*APawn vtable slot 254 = [vt+0x7F0]*/ || AESPawn::bIsPlayerPawn /*+0x10C1*/)` — **does** return true for a client's ship on the server. Prefer it whenever the question is only "is this a player".

### D. `AESGameModeBase` caches EXACTLY ONE player (sizeof 1432)
- `ESPlayerPawn` @ **+0x508** (`AESPawn*`)
- `ESPlayerController` @ **+0x510** (`AESPlayerController*`)
- `PlayerData` @ **+0x518** (`UPlayerData*`)

Native write sites:
- `AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation` (`05DEC54C`, **10.5 KB: 0x5DEC54C–0x5DEF078**) — sets `PlayerData(+0x518) = GetPlayerData()` at `05DEC63A` (then `UPlayerData::ReinitGlobalAttributesWithCurrentShip()`); reads `UInventoryLib::GetCurrentShip()` at `05DEEABA` and `05DEEE90` to decide **which ship to spawn**; sets `ESPlayerPawn(+0x508) = <spawned pawn>` at `05DEEE7D`; after `AActor::FinishSpawning` calls `UGameplayLib::GetESPlayerController(this)` at `05DEEF02` → `UpdateDualSenseBoostTriggerFunctionality` and arms `DelayedRestoreAfterRift`.
  → **Every client join re-points the game mode's single player cache at the joining client, spawns the host's current ship for them, and re-inits the host's global PlayerData.**
- `AESGameModeBase::ReinitPlayerPawn(APawn*)` (`05DE7FA4`) — writes `+0x508` at `05DE8080`.
- `AESGameModeBase::StartPlay` (`02B3BF24`) — writes `+0x518` at `02B3C040`.
- `+0x510` has no native writer (BlueprintReadWrite; set from the Blueprint game mode).

Readers that follow the wrong ship once a client joins (all no-arg members): `IsPlayerOutsideOfLocationArea` (`0131133C`, reads `+0x510` then `+0x508`), `IsPlayerOutsideOfJumpInArea`, `IsPlayerOutsideOfInnerLocationBounds`, `CheckIfOutsideOfLevelBounds` (`013114F0`), `CheckForWorldOriginShifting` (`0192A884`), `CheckForInvasions`, `MovePlayerInsideLevelAreaAndLetLookAtCenter`, `PlayerJumpTriggered/Completed`, `DelayedUndockPlayerShip`, `DisplayCurrentObjectives`, `CheckForWorldLevelingEvent`.

### E. Other context-free (therefore single-player) ES2 statics — bulk inventory
`static` functions whose whole parameter list is scalars/void (cannot possibly be per-player):
`UGameplayLib` 84/284, `UItemLib` 58/215, `UInventoryLib` 34/80, `UMissionLib` 26/76, `UMapLib` 22/95, `UPlatformSupportLib` 12/25, `UUserFunctionsLib` 10/23, `UAnalyticsLib` 7/15, `UInputLib` 7/48, `ULocationLib` 7/38, `UVariantLib` 5/9, `UDebugLib` 4/6, `UUiLib` 4/16, `UMathLib` 3/19, `UModifierLib` 3/12, `USerializationLib` 3/3, `UTestLib` 3/4, `UFileCompressionLib` 3/5. **≈295 context-free statics.**
Representative offenders (verified to route through `GetPlayerData()` / `InstancePointer`): `GetPlayerLevel` `05E2E478`, `GetRemainingXPForLevelUp` `05E2F07C`, `GetLevelUpProgress` `05E2D398`, `GetUltimateRatio` `05E323CC` (→ `GetUltimateDeviceOfPlayerPawn` `017F04F4` → `GetPlayerController(InstancePointer, 0)`), `GetCurrentlyDockedToStation` `05E2AF98`, `IsPlayerDockedToAnyStation` `0138210C`, `IsPlayerDockedToHomebase` `05E34C8C`, `HasEnoughCredits` `05E33114`, `HasCurrentPlayerShipAnyDamagedEquipment` `05E33068`, `IsShipInventoryManipulationRestricted` `01914E30`, `GetCurrentJobRank` `05E2ACA4`, `GetMaxLegendaryEquipAmountForCurrentShip` `05E2DA00`, `UInventoryLib::SetCurrentShip(int)` `05E5059C`, `UInventoryLib::ReinitShipAfterPotentialChanges()` `0178F6DC`.
(`UGameplayLib::GetGameData()` `02B3A87C`, `GetCodexIDs`, `GetCommentaryIDs` are read-only static content tables — fine to share.)

### F. Hot paths where this bites first
- `AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation` — spawns the host's current ship for every joiner and clobbers the game-mode caches.
- `AESPlayerController::BeginPlay` (`02B4030C`) — calls `UInventoryLib::ReinitShipAfterPotentialChanges()` (mutates global PlayerData) and touches `InstancePointer` repeatedly; on the server the client's PC has no `GetLocalPlayer()`.
- `UHealthComponent::TakeDamage` and `AWeaponBase::CheckForItemImpactEffects` call `GetESPlayerPawn(...)` → damage/impact bookkeeping always attributed to the host's ship.
- `APickupBase::OnCollect_Implementation` / `InteractPullItem` / `InteractPullAllItems` / `ShowLootScreenIfNeeded` / `RecreatePickupsAfterGroupPull`, `UInteractComponent::AbortInteract`/`Reset`, `UPortableComponent::Attach` — all `GetESPlayerController(...)` → **all loot goes to the host**.
- `UGameplayLib::EnterNextRiftLocation`, `SaveShipState`, `RestoreShipState`, `SpawnNPCPawnForParent`, `GetEnemiesInRangeWithBlacklist`, `GetNumNonHostilesInRange`, `UBTS_DetectNearestEnemyNative::TickNode` (AI target selection) — all `GetESPlayerPawn(ctx)`.

**Design rule:** never let ES2 answer "who is the player?". Keep a per-controller table in the mod and, for any server-side callback servicing a specific client, either (a) temporarily swap `AESGameModeBase+0x508/+0x510/+0x518` and `UESGameInstance+0x1C0` across the call, or (b) hook the six index-0 accessors and resolve via a thread-local "current acting controller" that your RPC/hook entry points set. (b) scales to 4 players; (a) is cheaper for 2.

## Key functions

- **USteamNetDriver::InitConnect** @ `052B245C` unique=True — ISocketSubsystem::Get(STEAM_SUBSYSTEM); if ConnectURL.Host StartsWith(L"steam.",6,IgnoreCase) creates a Steam socket via CreateUniqueSocket(FNetworkProtocolTypes::Steam, L"Unreal client (Steam)", FName("SteamClientSocket")) + SetSocketAndLocalAddress; else sets bIsPassthrough(this+0x980)=1. Always falls through to UIpNetDriver::InitConnect.
  - `public: virtual bool __cdecl USteamNetDriver::InitConnect(class FNetworkNotify*, struct FURL const&, class FString&)`
  - ABI: rcx=this, rdx=FNetworkNotify*, r8=const FURL& (pointer; FURL.Host is FString at +0x10), r9=FString& Error. bool in al. Tail-jumps UIpNetDriver::InitConnect (0x145255928).
- **USteamNetDriver::InitListen** @ `052B27B4` unique=True — If ListenURL has option L"bIsLanMatch" OR command line has -forcepassthrough, sets bIsPassthrough(this+0x980)=1; else creates the Steam listen socket (FNetworkProtocolTypes::Steam, L"Unreal server (Steam)", FName("SteamClientSocket")) and installs it. Then calls Super.
  - `public: virtual bool __cdecl USteamNetDriver::InitListen(class FNetworkNotify*, struct FURL&, bool, class FString&)`
  - ABI: rcx=this, rdx=FNetworkNotify*, r8=FURL& (Op TArray at +0x48 data/+0x50 num), r9b=bReuseAddressAndPort, [rsp+0x28]=FString& Error. Tail-jumps UIpNetDriver::InitListen (0x145255bf4).
- **USteamNetDriver::InitBase** @ `052B225C` unique=True — If bIsPassthrough -> UIpNetDriver::InitBase. Else UNetDriver::InitBase then calls GetSocketSubsystem via vtable [+0x3F8] (slot 127) and validates the socket, writing an error string on failure.
  - `public: virtual bool __cdecl USteamNetDriver::InitBase(bool, class FNetworkNotify*, struct FURL const&, bool, class FString&)`
  - ABI: rcx=this, dl=bInitAsClient, r8=Notify, r9=const FURL&, [rsp+0x28]=bool bReuseAddressAndPort, [rsp+0x30]=FString& Error.
- **USteamNetDriver::GetSocketSubsystem** @ `052B0694` unique=True — Returns ISocketSubsystem::Get(FName("WINDOWS")) when bIsPassthrough(+0x980) is set, else the STEAM subsystem. Distinct from UIpNetDriver::GetSocketSubsystem (0525112C): an existing hook on the IpNetDriver version does NOT affect SteamNetDriver instances.
  - `public: virtual class ISocketSubsystem* __cdecl USteamNetDriver::GetSocketSubsystem(void)`
  - ABI: rcx=this. UNetDriver vtable slot 127 = [vt+0x3F8].
- **FSocketSubsystemSteam::GetAddressFromString** @ `052AAE78` unique=True — FString::RemoveFromStart(L"steam.",6,IgnoreCase); if IsNumeric() -> CRT _wtoi64 -> FUniqueNetIdSteam::Create<uint64> -> FInternetAddrSteam. Otherwise builds FName("WINDOWS") and delegates to ISocketSubsystem::Get(WINDOWS)->GetAddressFromString (vtable +0x58).
  - `public: virtual class TSharedPtr<class FInternetAddr,1> __cdecl FSocketSubsystemSteam::GetAddressFromString(class FString const&)`
  - ABI: MSVC sret: rcx=TSharedPtr<FInternetAddr>* return slot, rdx=this, r8=const FString& (>8 bytes -> by pointer). Returns rcx.
- **FSocketSubsystemSteam::GetAddressInfo** @ `052AB098` unique=True — UE5 resolution entry point used by the NetDriver address resolver. Same steam.-strip + IsNumeric + _wtoi64 + FUniqueNetIdSteam path; wraps the FInternetAddrSteam in FAddressInfoResultData tagged FNetworkProtocolTypes::Steam; non-numeric falls back to the WINDOWS subsystem.
  - `public: virtual struct FAddressInfoResult __cdecl FSocketSubsystemSteam::GetAddressInfo(wchar_t const*, wchar_t const*, enum EAddressInfoFlags, class FName, enum ESocketType)`
  - ABI: sret in rcx; rdx=this; r8=HostName; r9=ServiceName; stack: flags, FName ProtocolType (by value, 8 bytes), ESocketType.
- **FInternetAddrSteam::SetIp** @ `052BC050` unique=True — Accepts 'steam.<id>', '<id>' or '<id>:<channel>'. StartsWith(L"steam.",6) -> RightChop(6); Split on L":" -> left via _wtoi64 into FUniqueNetIdSteam at this+0x8, right via _wtoi into SteamChannel at this+0x18; sets bIsValid=true.
  - `public: virtual void __cdecl FInternetAddrSteam::SetIp(wchar_t const*, bool&)`
  - ABI: rcx=this, rdx=const TCHAR* InAddr, r8=bool& bIsValid (set false on entry).
- **FInternetAddrSteam::ToString** @ `052C5944` unique=True — Printf(L"%lld:%d", SteamID64, SteamChannel) when bAppendPort, else Printf(L"%lld"). Never emits the 'steam.' prefix — the prefix is parse-only.
  - `public: virtual class FString __cdecl FInternetAddrSteam::ToString(bool) const`
  - ABI: sret rcx=FString*, rdx=this, r8b=bAppendPort.
- **FSocketSubsystemSteam::Init** @ `052B216C` unique=True — Reads [OnlineSubsystemSteam] bAllowP2PPacketRelay(+0x160) / P2PConnectionTimeout(+0x164) / P2PCleanupTimeout(+0x178) from GEngineIni, then calls SteamNetworking()->AllowP2PPacketRelay(...) and SteamGameServerNetworking()->AllowP2PPacketRelay(...) via ISteamNetworking vtable slot 7 ([vt+0x38]) at 052B2219 and 052B2244. Ctor default for bAllowP2PPacketRelay is FALSE.
  - `public: virtual bool __cdecl FSocketSubsystemSteam::Init(class FString&)`
  - ABI: rcx=this, rdx=FString& Error. Returns true unconditionally (al=1).
- **FSocketSubsystemSteam::ShouldOverrideDefaultSubsystem** @ `052BCA64` unique=True — GConfig->GetBool(L"OnlineSubsystemSteam", L"bUseSteamNetworking", bOut, GEngineIni) ? bOut : true. This is the bMakeDefault argument to RegisterSocketSubsystem — the reason STEAM hijacks the process default socket subsystem.
  - `public: bool __cdecl FSocketSubsystemSteam::ShouldOverrideDefaultSubsystem(void) const`
  - ABI: rcx=this. bool in al.
- **CreateSteamSocketSubsystem** @ `052A2170` unique=True — Lazily news FSocketSubsystemSteam into the global FSocketSubsystemSteam::SocketSingleton (RVA 09E62260), calls its Init (vtable +8); on success loads FModuleManager module FName("Sockets") and calls FSocketSubsystemModule::RegisterSocketSubsystem(STEAM_SUBSYSTEM, Subsystem, ShouldOverrideDefaultSubsystem()) via vtable slot 10 ([vt+0x50]); returns STEAM_SUBSYSTEM. On failure calls FSocketSubsystemS
  - `class FName __cdecl CreateSteamSocketSubsystem(void)`
  - ABI: sret: rcx=FName* return slot; returns rcx.
- **FSocketSubsystemSteam::AcceptP2PConnection** @ `0529C8E4` unique=True — If RemoteId.IsValid() (FUniqueNetId vtable +0x28) and !IsConnectionPendingRemoval(RemoteId,-1): SteamNetworkingPtr->AcceptP2PSessionWithUser(CSteamID) — ISteamNetworking vtable slot 3 = [vt+0x18] — then emplaces an FSteamP2PConnectionInfo into AcceptedConnections (this+0xC0).
  - `public: bool __cdecl FSocketSubsystemSteam::AcceptP2PConnection(class ISteamNetworking*, class FUniqueNetIdSteam const&)`
  - ABI: rcx=this, rdx=ISteamNetworking*, r8=const FUniqueNetIdSteam& (pointer; CSteamID uint64 at +0x18).
- **FOnlineAsyncEventSteamConnectionRequest::Finalize** @ `052A6698` unique=True — The P2PSessionRequest_t handler. Guard: Subsystem->bUsingSteamNetworking (FOnlineSubsystemSteam+0xDA) must be non-zero; then ISocketSubsystem::Get(STEAM_SUBSYSTEM) and AcceptP2PConnection(SteamNetworkingPtr, RemoteId). This is the ONLY thing needed for the host to accept an incoming P2P peer — no Steam lobby or session involved.
  - `public: virtual void __cdecl FOnlineAsyncEventSteamConnectionRequest::Finalize(void)`
  - ABI: rcx=this. Fields: FOnlineSubsystemSteam* at +0x10, ISteamNetworking* at +0x18, FUniqueNetIdSteam* at +0x20. Runs on the OnlineAsyncTaskManagerSteam thread, not the game thread.
- **FSocketSteam::SendTo** @ `052BB72C` unique=True — Fails if SteamNetworkingPtr(this+0x48) is null OR if Destination.SteamId equals this->LocalSteamId (self-send blocked). Otherwise SendP2PPacket(Dest CSteamID, Data, Count, this->SteamSendMode(+0x44), Dest.SteamChannel(+0x18)). CONSEQUENCE: two instances on the same Steam account cannot connect over Steam P2P.
  - `public: virtual bool __cdecl FSocketSteam::SendTo(unsigned char const*, int, int&, class FInternetAddr const&)`
  - ABI: rcx=this, rdx=Data, r8d=Count, r9=int& BytesSent, [rsp+0x28]=const FInternetAddr& Destination.
- **FSocketSteam::Bind** @ `0529DEE0` unique=True — SteamChannel(this+0x40) = Addr.GetPort() (FInternetAddr vtable +0x38). Proves the URL 'port' is the Steam P2P channel number — host and client must use the same value.
  - `public: virtual bool __cdecl FSocketSteam::Bind(class FInternetAddr const&)`
  - ABI: rcx=this, rdx=const FInternetAddr&. Always returns true.
- **FSocketSteam::RecvFrom** @ `052B8B5C` unique=True — ISteamNetworking IsP2PPacketAvailable/ReadP2PPacket on SteamChannel; fills Source with an FInternetAddrSteam.
  - `public: virtual bool __cdecl FSocketSteam::RecvFrom(unsigned char*, int, int&, class FInternetAddr&, enum ESocketReceiveFlags::Type)`
  - ABI: rcx=this, rdx=Data, r8d=BufferSize, r9=int& BytesRead, [rsp+0x28]=FInternetAddr& Source, [rsp+0x30]=flags.
- **USteamNetConnection::InitLocalConnection** @ `052B2C08` unique=True — bIsPassthrough(this+0x1E70) = !InURL.Host.StartsWith(L"steam."); for Steam it also sets the byte at Resolver(+0x1E68)+0x4C to 1 (address resolution off), then Super, then FSocketSubsystemSteam::RegisterConnection.
  - `public: virtual void __cdecl USteamNetConnection::InitLocalConnection(class UNetDriver*, class FSocket*, struct FURL const&, enum EConnectionState, int, int)`
  - ABI: rcx=this, rdx=UNetDriver*, r8=FSocket*, r9=const FURL&, [rsp+0x28]=EConnectionState, [rsp+0x30]=MaxPacket, [rsp+0x38]=PacketOverhead.
- **USteamNetConnection::InitRemoteConnection** @ `052B2CC8` unique=True — Host side. bIsPassthrough(this+0x1E70) = Driver->bIsPassthrough(+0x980); calls UIpConnection::InitRemoteConnection; then, if not passthrough and UNetConnection::RemoteAddr(+0x110) is non-null, ISocketSubsystem::Get(STEAM)->RegisterConnection(this).
  - `public: virtual void __cdecl USteamNetConnection::InitRemoteConnection(class UNetDriver*, class FSocket*, struct FURL const&, class FInternetAddr const&, enum EConnectionState, int, int)`
  - ABI: rcx=this, rdx=UNetDriver*, r8=FSocket*, r9=const FURL&, [rsp+0x28]=const FInternetAddr&, [rsp+0x30]=state, [rsp+0x38]=MaxPacket, [rsp+0x40]=PacketOverhead.
- **FSocketSubsystemSteam::RegisterConnection** @ `052B8EF0` unique=True — Adds the connection to SteamConnections (this+0xB0, TArray<FWeakObjectPtr>) so the P2P session is torn down when the connection dies.
  - `public: void __cdecl FSocketSubsystemSteam::RegisterConnection(class USteamNetConnection*)`
  - ABI: rcx=this, rdx=USteamNetConnection*.
- **FOnlineSubsystemSteam::Init** @ `052B12E4` unique=True — Reads [OnlineSubsystemSteam] bInitServerOnClient; calls InitSteamworksClient(bRelaunchInSteam=true, SteamAppId=0); optionally InitSteamworksServer; calls CreateSteamSocketSubsystem() then sets bUsingSteamNetworking(+0xDA)=1 at 052B13ED; starts FOnlineAsyncTaskManagerSteam on a thread named 'OnlineAsyncTaskThreadSteam %s'; constructs FOnlineSessionSteam / Identity / Presence / Auth and calls FOnlin
  - `public: virtual bool __cdecl FOnlineSubsystemSteam::Init(void)`
  - ABI: rcx=this. Runs lazily on the first IOnlineSubsystem::Get(STEAM).
- **FOnlineSubsystemSteam::InitSteamworksClient** @ `052B2D50` unique=True — If (bRelaunchInSteam && SteamAppId != 0) calls SteamAPI_RestartAppIfNecessary (delay-import IAT 0x1499D93D0) — SKIPPED here because Init passes SteamAppId=0. Then FSteamSharedModule::ObtainSteamClientInstanceHandle(), sets bSteamworksClientInitialized(+0xD8), validates every SteamXxx() interface is non-null, and finally SteamAppID(+0xDC) = SteamUtils()->GetAppID() (ISteamUtils vtable +0x48, at 052
  - `public: bool __cdecl FOnlineSubsystemSteam::InitSteamworksClient(bool, int)`
  - ABI: rcx=this, dl=bRelaunchInSteam, r8d=SteamAppId.
- **FSteamSharedModule::ObtainSteamClientInstanceHandle** @ `05290BB0` unique=True — Ref-counted SteamAPI_Init wrapper; the handler ctor calls the delay-imported SteamInternal_SteamAPI_Init (IAT 0x1499D9380). Use FSteamSharedModule::IsAvailable (0529083C) to test whether SteamAPI came up under Proton.
  - `public: class TSharedPtr<class FSteamClientInstanceHandler,1> __cdecl FSteamSharedModule::ObtainSteamClientInstanceHandle(void)`
  - ABI: sret rcx=TSharedPtr*, rdx=this.
- **FSteamSharedModule::GetSteamModulePath** @ `052905B0` unique=True — Builds FPaths::EngineDir() / L"Binaries/ThirdParty/Steamworks" / L"Steamv16" / L"Win64/". LoadSteamModules (052908C8) PushDllDirectory's it and GetDllHandle's steam_api64.dll from there.
  - `public: class FString __cdecl FSteamSharedModule::GetSteamModulePath(void) const`
  - ABI: sret rcx=FString*, rdx=this.
- **ISocketSubsystem::Get** @ `0301A940` unique=True — Looks up the named subsystem in FSocketSubsystemModule; NAME_None returns the default — which is STEAM in this build unless bUseSteamNetworking=False.
  - `public: static class ISocketSubsystem* __cdecl ISocketSubsystem::Get(class FName const&)`
  - ABI: rcx=const FName& (by pointer; callers lea a global FName, e.g. STEAM_SUBSYSTEM @ RVA 09B37BD8).
- **UE::Private::CreateNetDriver_Local** @ `05079CAC` unique=True — Scans GEngine->NetDriverDefinitions (GEngine+0x1088 data, +0x1090 num, stride 0x1C) for DefName==NetDriverDefinition; StaticLoadClass(UNetDriver::StaticClass(), nullptr, *Def.DriverClassName.ToString()) with fallback to DriverClassNameFallback; NewObject; sets NetDriverName(+0x200), NetDriverDefinition(+0x278), MaxChannelsOverride(+0x280)=Def+0x18 at 05079E63.
  - `class UNetDriver* __cdecl UE::Private::CreateNetDriver_Local(class UEngine*, struct FWorldContext&, class FName, class FName)`
  - ABI: rcx=UEngine*, rdx=FWorldContext&, r8=FName NetDriverName (by value, 8 bytes), r9=FName NetDriverDefinition (by value).
- **UNetConnection::InitChannelData** @ `04D48B6C` unique=True — MaxChannels = cvar net.MaxChannelSize (TConsoleVariableData<int>* global @ RVA 09B2CF10); if <=0 use this->DefaultMaxChannelSize(+0x13E8) unless Driver(+0x58)->MaxChannelsOverride(+0x280) > 0. UNetConnection ctor (04D39CEC) sets DefaultMaxChannelSize = 0x7FFF (32767) at 04D3A045 — not a 4-player limiter.
  - `protected: void __cdecl UNetConnection::InitChannelData(void)`
  - ABI: rcx=this.
- **AGameSession::AtCapacity** @ `04AAC160` unique=True — Returns false in NM_Standalone. Spectator branch compares the spectator count vs MaxSpectators(+0x2A8). Player branch: Limit = (*net.MaxPlayersOverride cvar data ptr @ RVA 09B2A610) > 0 ? that : MaxPlayers(+0x2AC); if Limit <= 0 returns FALSE (unlimited); else returns GetNumPlayers() >= Limit.
  - `public: virtual bool __cdecl AGameSession::AtCapacity(bool)`
  - ABI: rcx=this, dl=bSpectator. AGameSession vtable slot 235 = [vt+0x758].
- **AGameSession::ApproveLogin** @ `04AABD74` unique=True — Parses ?SpectatorOnly= and ?SplitscreenCount= via UGameplayStatics::GetIntOption, checks AtCapacity, returns L"Server full." or L"Maximum splitscreen players" (empty string = approved).
  - `public: virtual class FString __cdecl AGameSession::ApproveLogin(class FString const&)`
  - ABI: sret rcx=FString* out, rdx=this, r8=const FString& Options. AGameSession vtable slot 232 = [vt+0x740]; invoked from AGameModeBase::PreLogin (04A442C8) at 04A44332.
- **AGameSession::InitOptions** @ `04ABD748` unique=True — If HasOption(L"MaxPlayers") -> MaxPlayers(+0x2AC) = GetIntOption(...) and MaxPlayersOptionOverride(+0x2C0) is set; same for L"MaxSpectators" -> +0x2A8 / +0x2C8. So ?MaxPlayers=4 in the travel URL works.
  - `public: virtual void __cdecl AGameSession::InitOptions(class FString const&)`
  - ABI: rcx=this, rdx=const FString& Options. AGameSession vtable slot 229 = [vt+0x728]; called from AGameModeBase::InitGame (04A3CE28) at 04A3CF17 immediately after SpawnActor(GameSessionClass).
- **AGameModeBase::GetNumPlayers** @ `04A37E68` unique=True — Iterates UWorld's PlayerControllerList (World+0x218 data / +0x220 num) counting non-spectating PCs. On a server this INCLUDES remote clients, so AtCapacity correctly counts joiners.
  - `public: virtual int __cdecl AGameModeBase::GetNumPlayers(void)`
  - ABI: rcx=this; int in eax.
- **AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation** @ `05DEC54C` unique=True — THE co-op choke point. Sets AESGameModeBase::PlayerData(+0x518)=UGameplayLib::GetPlayerData() at 05DEC63A then UPlayerData::ReinitGlobalAttributesWithCurrentShip(); resolves LocationInfo/PlayerStart via AGameModeBase::FindPlayerStart; reads UInventoryLib::GetCurrentShip() at 05DEEABA and 05DEEE90 to pick the ship class/loadout (the HOST's global current ship); spawns + ISavableInterface::StaticRes
  - `public: virtual class APawn* __cdecl AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation(class AController*, struct UE::Math::TTransform<double> const&)`
  - ABI: rcx=this, rdx=AController*, r8=const FTransform& (48 bytes -> BY HIDDEN POINTER). Returns APawn* in rax. Spans 0x05DEC54C..0x05DEF078 (~10.5 KB) — do not truncate when analysing.
- **AESGameModeBase::ReinitPlayerPawn** @ `05DE7FA4` unique=True — Writes ESPlayerPawn(this+0x508) at 05DE8080, then SpawnDefaultPawnAtTransform / AController::UnPossess / Possess / Destroy old pawn, and rewires AESPlayerController dynamic delegates. Second writer of the single-player cache.
  - `public: void __cdecl AESGameModeBase::ReinitPlayerPawn(class APawn*)`
  - ABI: rcx=this, rdx=APawn*.
- **AESGameModeBase::StartPlay** @ `02B3BF24` unique=True — Sets AESGameModeBase::PlayerData(+0x518) = UGameplayLib::GetPlayerData() at 02B3C040 and arms the out-of-bounds / world-origin / objectives timers that later read ESPlayerPawn(+0x508) and ESPlayerController(+0x510).
  - `public: virtual void __cdecl AESGameModeBase::StartPlay(void)`
  - ABI: rcx=this.
- **UGameplayLib::GetESPlayerPawn** @ `013F0CC0` unique=True — Cast<AESPawn>(UGameplayStatics::GetPlayerController(ctx, 0)->GetPawnOrSpectator()). PlayerIndex hard-coded 0 -> always the HOST's ship on a listen server. 42 direct call sites in 27 functions incl. UHealthComponent::TakeDamage, AWeaponBase::CheckForItemImpactEffects, UBTS_DetectNearestEnemyNative::TickNode, UGameplayLib::EnterNextRiftLocation / SaveShipState / RestoreShipState / SpawnNPCPawnForPar
  - `public: static class AESPawn* __cdecl UGameplayLib::GetESPlayerPawn(class UObject const*)`
  - ABI: rcx=const UObject* WorldContextObject; AESPawn* in rax. exec thunk RVA 01824964.
- **UGameplayLib::GetESPlayerController** @ `0150D280` unique=True — Cast<AESPlayerController>(UGameplayStatics::GetPlayerController(ctx, 0)) — hard-coded index 0. 23 direct call sites in 22 functions, notably the whole APickupBase loot pipeline (OnCollect_Implementation, InteractPullItem, InteractPullAllItems, ShowLootScreenIfNeeded, RecreatePickupsAfterGroupPull), UInteractComponent::AbortInteract/Reset, UPortableComponent::Attach. All loot/interaction credit goe
  - `public: static class AESPlayerController* __cdecl UGameplayLib::GetESPlayerController(class UObject const*)`
  - ABI: rcx=const UObject* WorldContextObject. exec thunk RVA 0150D170.
- **UGameplayLib::GetESHUD** @ `015576B8` unique=True — Cast<AESHUD>(UGameplayStatics::GetPlayerController(ctx, 0)->MyHUD) — index 0. 28 call sites / 23 functions.
  - `public: static class AESHUD* __cdecl UGameplayLib::GetESHUD(class UObject const*)`
  - ABI: rcx=const UObject*. exec thunk RVA 015575A8.
- **UGameplayLib::GetPlayerControllerWithoutContext** @ `013EFE78` unique=True — return UESGameInstance::InstancePointer ? UGameplayStatics::GetPlayerController(InstancePointer, 0) : nullptr. 11 call sites / 10 functions.
  - `public: static class APlayerController* __cdecl UGameplayLib::GetPlayerControllerWithoutContext(void)`
  - ABI: No args. exec thunk RVA 05B92368. Body is only 24 bytes (mov/test/je/xor/jmp) — check the MinHook trampoline fits before detouring.
- **UGameplayLib::GetPlayerData** @ `0127398C` unique=True — return UESGameInstance::InstancePointer (RVA 09AC5E88) ? InstancePointer->PlayerData(+0x1C0) : nullptr. THE global player state. 371 direct call sites in 342 functions across UGameplayLib / UInventoryLib / UMapLib / UItemLib / UMissionLib / ULocationLib / AWantedLevelManager / damage+armor components.
  - `public: static class UPlayerData* __cdecl UGameplayLib::GetPlayerData(void)`
  - ABI: No args; UPlayerData* in rax. Body is 28 bytes starting with a 7-byte RIP-relative mov (a 5-byte patch fits). exec thunk RVA 018C43BC.
- **UGameplayStatics::GetPlayerController** @ `013F08F0` unique=True — World = ctx->GetWorld() (UObject vtable +0x198); GameInstance = World+0x1D8; iterates UGameInstance::LocalPlayers (GameInstance+0x38 data / +0x40 num) returning LocalPlayer->PlayerController (UPlayer+0x30) at the index. LocalPlayers only ever holds LOCALLY controlled players, so on a listen server no index can reach a remote client's PC.
  - `public: static class APlayerController* __cdecl UGameplayStatics::GetPlayerController(class UObject const*, int)`
  - ABI: rcx=WorldContextObject, edx=PlayerIndex.
- **UGameplayLib::IsPlayerController** @ `012D2898` unique=True — return UGameplayStatics::GetPlayerController(ctx, 0) == C. Returns FALSE for every remote client controller on the host — prime candidate for a corrective hook.
  - `public: static bool __cdecl UGameplayLib::IsPlayerController(class UObject*, class AController*)`
  - ABI: rcx=WorldContextObject, rdx=AController*.
- **UGameplayLib::IsPlayerPawn** @ `012A1810` unique=True — Cast<AESPawn>(A) && (A->IsPlayerControlled() /*APawn vtable slot 254 = [vt+0x7F0]*/ || AESPawn::bIsPlayerPawn(+0x10C1)). This one IS multiplayer-correct on the server — prefer it over the Get* accessors wherever the question is only 'is this a player ship'.
  - `public: static bool __cdecl UGameplayLib::IsPlayerPawn(class AActor const*)`
  - ABI: rcx=const AActor*.
- **UInventoryLib::GetCurrentShip** @ `0121A1E0` unique=True — Reads the global PlayerData via UESGameInstance::InstancePointer and returns the single 'current ship' record. 66 direct call sites in 42 functions, including twice inside AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation — every joining client is spawned as the host's current ship.
  - `public: static struct FShipData __cdecl UInventoryLib::GetCurrentShip(void)`
  - ABI: sret rcx=FShipData* return slot (large struct -> hidden pointer). exec thunk RVA 01805F74.
- **UInventoryLib::GetInventoryOfCurrentShip** @ `014DE7B4` unique=True — Global PlayerData -> current ship -> UInventory. 57 direct call sites in 43 functions. One shared inventory for all players.
  - `public: static class UInventory* __cdecl UInventoryLib::GetInventoryOfCurrentShip(void)`
  - ABI: No args. exec thunk RVA 014DE790.
- **UESGameInstance::GetInstance** @ `05D6C270` unique=True — Returns the static UESGameInstance::InstancePointer (RVA 09AC5E88).
  - `public: static class UESGameInstance* __cdecl UESGameInstance::GetInstance(void)`
  - ABI: No args; body is 8 bytes (mov rax,[rip+..]; ret) — TOO SHORT for a 5-byte MinHook detour with a safe trampoline. Read the global directly instead.
- **UGameInstance::EnableListenServer** @ `04A2F090` unique=True — Already used by the mod. With a SteamNetDriver GameNetDriver, PortOverride becomes the Steam P2P channel (see FSocketSteam::Bind).
  - `public: virtual bool __cdecl UGameInstance::EnableListenServer(bool, int)`
  - ABI: rcx=this, dl=bEnable, r8d=PortOverride.
- **FInternetAddrSteam::SetPort / GetPort** @ `01960040` unique=False — Trivial accessors for FInternetAddrSteam::SteamChannel at +0x18 (SetPort: mov [rcx+0x18],edx; GetPort: mov eax,[rcx+0x18]). Listed only as a hazard warning.
  - `public: virtual void __cdecl FInternetAddrSteam::SetPort(int)  /  public: virtual int __cdecl FInternetAddrSteam::GetPort(void) const (RVA 0195FBB0)`
  - ABI: SetPort RVA 01960040 is shared by 11 function symbols (TDoubleLinkedList<...>::SetListSize etc). GetPort RVA 0195FBB0 is shared by 87 function symbols. ICF-FOLDED — NEVER HOOK THESE BY RVA.

## Types

### USteamNetDriver (size 2440)
- 0x00C4 MaxInternetClientRate int32 (UNetDriver) — ctor default 10000 (0x2710); per-client bandwidth cap, raise for 4 players
- 0x00C8 MaxClientRate int32 (UNetDriver) — ctor default 15000 (0x3A98)
- 0x00F8 ClientConnections TArray<TObjectPtr<UNetConnection>> (UNetDriver) — no fixed cap
- 0x0200 NetDriverName FName (UNetDriver)
- 0x0278 NetDriverDefinition FName (UNetDriver)
- 0x0280 MaxChannelsOverride int32 (UNetDriver) — copied from FNetDriverDefinition+0x18 in CreateNetDriver_Local at 05079E63
- 0x0918 SocketPrivate TSharedPtr<FSocket> (UIpNetDriver)
- 0x0980 bIsPassthrough bool (USteamNetDriver) — 1 = behave as plain UDP IpNetDriver; drives GetSocketSubsystem / InitBase / InitRemoteConnection

### USteamNetConnection (size 7800)
- 0x0058 Driver UNetDriver* (UNetConnection) — read by InitChannelData
- 0x0110 RemoteAddr TSharedPtr<FInternetAddr> (UNetConnection) — must be non-null for InitRemoteConnection to RegisterConnection
- 0x0160 PlayerId FUniqueNetIdRepl (UNetConnection)
- 0x13E8 DefaultMaxChannelSize int32 (UNetConnection) — ctor sets 0x7FFF (32767) at 04D3A045
- 0x13F0 Channels TArray<TObjectPtr<UChannel>> (UNetConnection)
- 0x1DE0 Socket FSocket* (UIpConnection)
- 0x1E68 Resolver TPimplPtr<FNetConnectionAddressResolution> (UIpConnection) — its byte at +0x4C is set to 1 for Steam connections
- 0x1E70 bIsPassthrough bool (USteamNetConnection)

### FSocketSubsystemSteam (size 392)
- 0x00A0 SteamSockets TArray<FSocketSteam*>
- 0x00B0 SteamConnections TArray<FWeakObjectPtr> — filled by RegisterConnection
- 0x00C0 AcceptedConnections TMap<TSharedRef<FUniqueNetId const>, FSteamP2PConnectionInfo> — filled by AcceptP2PConnection
- 0x0110 DeadConnections TMap<FInternetAddrSteam,double>
- 0x0160 bAllowP2PPacketRelay bool — CTOR DEFAULT FALSE (written at 0529753B); overridden from [OnlineSubsystemSteam] bAllowP2PPacketRelay in Init; passed to ISteamNetworking::AllowP2PPacketRelay (vtable slot 7)
- 0x0164 P2PConnectionTimeout float — ctor 45.0f (0x42340000)
- 0x0168 P2PDumpCounter double
- 0x0170 P2PDumpInterval double — ctor 10.0
- 0x0178 P2PCleanupTimeout double — ctor 1.5
- 0x0180 LastSocketError int32
- STATIC: FSocketSubsystemSteam::SocketSingleton @ RVA 09E62260

### FInternetAddrSteam (size 32)
- 0x0008 SteamId TSharedRef<FUniqueNetIdSteam const> — the CSteamID uint64 lives at FUniqueNetIdSteam+0x18
- 0x0018 SteamChannel int32 — this is what FURL's 'port' becomes

### FSocketSteam (size 80)
- 0x0028 SocketSubsystem FSocketSubsystemSteam*
- 0x0030 LocalSteamId TSharedRef<FUniqueNetIdSteam const> — compared against the destination in SendTo (self-send blocked)
- 0x0040 SteamChannel int32 — set by Bind() from Addr.GetPort()
- 0x0044 SteamSendMode EP2PSend — 4th arg to SendP2PPacket
- 0x0048 SteamNetworkingPtr ISteamNetworking* — a live interface pointer a hook can reuse to call AllowP2PPacketRelay

### FOnlineSubsystemSteam (size 624)
- 0x0088 SubsystemName FName
- 0x00D8 bSteamworksClientInitialized bool
- 0x00D9 bSteamworksGameServerInitialized bool
- 0x00DA bUsingSteamNetworking bool — GATES the P2PSessionRequest auto-accept in FOnlineAsyncEventSteamConnectionRequest::Finalize; set to 1 unconditionally after CreateSteamSocketSubsystem
- 0x00DC SteamAppID uint32 — = SteamUtils()->GetAppID(); read back via GetAppId (052AB3C8)
- 0x00F8 SessionInterface TSharedPtr<FOnlineSessionSteam>
- 0x0200 OnlineAsyncTaskThreadRunnable FOnlineAsyncTaskManagerSteam*
- 0x0210 SteamAPIClientHandle TSharedPtr<FSteamClientInstanceHandler>

### FNetDriverDefinition (size 28)
- 0x0000 DefName FName — match key ('GameNetDriver')
- 0x0008 DriverClassName FName — set to '/Script/OnlineSubsystemSteam.SteamNetDriver' for Steam P2P (that package string is present in the exe)
- 0x0010 DriverClassNameFallback FName — set to '/Script/OnlineSubsystemUtils.IpNetDriver' so a Steam failure degrades to UDP
- 0x0018 MaxChannelsOverride int32 — -1 by default; copied to UNetDriver+0x280
- ARRAY: GEngine (RVA 09DA37B0) +0x1088 = data ptr, +0x1090 = Num, element stride 0x1C

### AGameSession (size 720)
- 0x02A8 MaxSpectators int32 (globalconfig; NOT set by the ctor)
- 0x02AC MaxPlayers int32 (globalconfig; NOT set by the ctor) — write 4 here, or 0 to make AtCapacity always return false
- 0x02B0 MaxPartySize int32 — the ONLY field the ctor touches (or dword,-1 at 04A9F4AA)
- 0x02B4 MaxSplitscreensPerConnection uint8
- 0x02C0 MaxPlayersOptionOverride TOptional<int32> — written by InitOptions but NOT consulted by AtCapacity in this build
- 0x02C8 MaxSpectatorsOptionOverride TOptional<int32>
- vtable: [229] InitOptions (+0x728), [232] ApproveLogin (+0x740), [234] PostLogin (+0x750), [235] AtCapacity (+0x758)

### AESGameModeBase (size 1432)
- 0x0300 GameSession TObjectPtr<AGameSession> (AGameModeBase) — where to poke MaxPlayers
- 0x0350 NumPlayers int32 (AGameMode)
- 0x035C NumTravellingPlayers int32 (AGameMode)
- 0x0410 LocationInfo ALocationInfo*
- 0x0421 bEnableOutOfBoundsCheck bool — the out-of-bounds timer reads +0x508/+0x510
- 0x0508 ESPlayerPawn AESPawn* — SINGLE-SLOT player cache; written by SpawnDefaultPawnAtTransform_Implementation (05DEEE7D) and ReinitPlayerPawn (05DE8080)
- 0x0510 ESPlayerController AESPlayerController* — SINGLE-SLOT; no native writer found (set from Blueprint); read by IsPlayerOutsideOfLocationArea (0131133C) etc.
- 0x0518 PlayerData UPlayerData* — SINGLE-SLOT; written by SpawnDefaultPawnAtTransform_Implementation (05DEC63A) and StartPlay (02B3C040)

### UESGameInstance (size None)
- 0x0030 WorldContext FWorldContext* (UGameInstance)
- 0x0038 LocalPlayers TArray<TObjectPtr<ULocalPlayer>> (UGameInstance) — only locally controlled players; the reason index-0 lookups can never see a remote client
- 0x01C0 PlayerData UPlayerData* — the process-global player save state (UPlayerData sizeof 8624, derives from USaveGame)
- STATIC: UESGameInstance::InstancePointer @ RVA 09AC5E88 — 362 references across 304 functions

### UPlayer (size 72)
- 0x0030 PlayerController TObjectPtr<APlayerController> — what UGameplayStatics::GetPlayerController returns
- 0x0038 CurrentNetSpeed int32 — clamped by UNetDriver::MaxClientRate/MaxInternetClientRate
- 0x003C ConfiguredInternetSpeed int32 (config)
- 0x0040 ConfiguredLanSpeed int32 (config)

### AESPawn (size None)
- 0x10C0 bIgnoreLevelingData bool
- 0x10C1 bIsPlayerPawn bool — second half of UGameplayLib::IsPlayerPawn's test
- APawn vtable slot 254 (+0x7F0) = IsPlayerControlled — the multiplayer-correct half of the same test

### Steam globals (data, not types) (size None)
- 09B37BD8 FName const STEAM_SUBSYSTEM
- 09B38348 FLazyName const FNetworkProtocolTypes::Steam
- 09E62260 FSocketSubsystemSteam* FSocketSubsystemSteam::SocketSingleton
- 09AC5E88 UESGameInstance* UESGameInstance::InstancePointer
- 09DA37B0 UEngine* GEngine
- 09C26AC0 FConfigCacheIni* GConfig
- 09AD1158 FString GEngineIni
- 09B2CF10 TConsoleVariableData<int>* for net.MaxChannelSize
- 09B2A610 TConsoleVariableData<int>* for net.MaxPlayersOverride
- 099A1A78 SteamNetworking()::s_CallbackCounterAndContext
- 099A1A60 SteamGameServerNetworking()::s_CallbackCounterAndContext
- 099A19A0 SteamUtils()::s_CallbackCounterAndContext
- 099D9378 delay-import IAT slot: SteamInternal_ContextInit
- 099D9380 delay-import IAT slot: SteamInternal_SteamAPI_Init
- 099D93C0 delay-import IAT slot: SteamAPI_RunCallbacks
- 099D93D0 delay-import IAT slot: SteamAPI_RestartAppIfNecessary


## Hook plan

- **GEngine->NetDriverDefinitions (data edit, no detour) — GEngine @ RVA 09DA37B0, array at +0x1088 / +0x1090, stride 0x1C** @ `09DA37B0` [both] — Select USteamNetDriver as the GameNetDriver when the user opts into Steam P2P transport.
  - behaviour: Find the entry whose DefName(+0x00) == FName("GameNetDriver"); set DriverClassName(+0x08) = FName("/Script/OnlineSubsystemSteam.SteamNetDriver") and DriverClassNameFallback(+0x10) = FName("/Script/OnlineSubsystemUtils.IpNetDriver"). Do this BEFORE UGameInstance::EnableListenServer on the host and BEFORE UEngine::Browse on the client. Keep the existing IpNetDriver value as the default; only flip it in Steam mode.
  - risk: The array also holds DemoNetDriver/BeaconNetDriver — match DefName exactly. If OnlineSubsystemSteam failed to init, StaticLoadClass still succeeds but InitListen/InitConnect early-out on a null STEAM subsystem and you get a silent no-op listen. Verify FSteamSharedModule::IsAvailable (0529083C) first and fall back to IpNetDriver.
- **UIpNetDriver::GetSocketSubsystem (existing mod hook) — make it conditional** @ `0525112C` [both] — Stop the current 'force Windows subsystem' hook from fighting the Steam path.
  - behaviour: Keep returning ISocketSubsystem::Get("Windows") only while the driver is a UIpNetDriver in direct-IP mode. Do NOT extend the hook to USteamNetDriver::GetSocketSubsystem (052B0694): that override already returns WINDOWS when bIsPassthrough(+0x980) and STEAM otherwise, which is exactly the desired behaviour.
  - risk: Because USteamNetDriver overrides UNetDriver vtable slot 127, a USteamNetDriver instance never enters 0525112C — the mod must not assume this hook fires in Steam mode.
- **FSocketSubsystemSteam::Init** @ `052B216C` [host] — Turn ON Valve's P2P packet relay so connections survive symmetric NAT / CGNAT without port forwarding. The default is OFF (ctor writes false at 0529753B).
  - behaviour: Detour; call the original; then set *(bool*)(this+0x160) = true and re-invoke AllowP2PPacketRelay(true) on the live interface — get ISteamNetworking* via the delay-imported SteamInternal_ContextInit (IAT VA 0x1499D9378) applied to the SteamNetworking() context block (RVA 099A1A78), then call vtable slot 7 ([vt+0x38]) with the bool = 1. Optionally raise P2PConnectionTimeout(+0x164). Cleaner alternative: write [OnlineSubsystemSteam] bAllowP2PPacketRelay=True into GConfig/GEngineIni before the first IOnlineSubsystem::Get(STEAM).
  - risk: Calling AllowP2PPacketRelay before SteamAPI_Init completes dereferences null — only do it in the POST hook of Init, which runs after ObtainSteamClientInstanceHandle. Relay adds latency (Valve relays are not low-latency), so expose it as a cvar rather than forcing it always.
- **FSocketSubsystemSteam::ShouldOverrideDefaultSubsystem** @ `052BCA64` [both] — Cleaner alternative to the current 'force Windows' hack: stop STEAM becoming the process DEFAULT socket subsystem while keeping it registered under the name STEAM.
  - behaviour: Detour to return false. ISocketSubsystem::Get(NAME_None) then yields the Windows subsystem again so plain IpNetDriver binds normally, while USteamNetDriver can still fetch ISocketSubsystem::Get(STEAM_SUBSYSTEM) explicitly.
  - risk: Must be hooked before FOnlineSubsystemSteam::Init runs — it is read exactly once, inside CreateSteamSocketSubsystem (052A2170). Hooking it later has no effect. Note bUsingSteamNetworking(+0xDA) is set independently, so P2P auto-accept still works.
- **FOnlineAsyncEventSteamConnectionRequest::Finalize** @ `052A6698` [host] — Observability plus optional allow-listing of incoming Steam P2P peers.
  - behaviour: Observe: log the remote CSteamID (this+0x18 = ISteamNetworking*, this+0x20 = FUniqueNetIdSteam*, uint64 at +0x18 of that). Optionally return early for SteamIDs not on the party allow-list instead of the current accept-anyone behaviour.
  - risk: Runs on the OnlineAsyncTaskManagerSteam thread, NOT the game thread — logging and state must be thread-safe. Blocking here drops the peer with no user-visible error.
- **FSocketSteam::SendTo** @ `052BB72C` [both] — Diagnose the 'nothing happens' failure mode of Steam P2P.
  - behaviour: Observe only (dev builds): log when it returns false, distinguishing 'SteamNetworkingPtr null' from 'destination == LocalSteamId' (the self-send guard) from 'SendP2PPacket returned false'. Fastest way to prove the two-instances-one-account problem in a local test.
  - risk: Extremely hot path (per packet). Rate-limit or gate behind a cvar; never allocate or take locks in the hook.
- **AGameSession MaxPlayers (data edit) — AGameModeBase::GameSession at +0x300, MaxPlayers at +0x2AC, MaxSpectators at +0x2A8. RVA shown is AtCapacity, the last-resort detour.** @ `04AAC160` [host] — Raise the player cap from the shipped value to 4.
  - behaviour: After AGameModeBase::InitGame has spawned the GameSession (or inside the existing PostLogin/RestartPlayer hook), write GameSession->MaxPlayers(+0x2AC) = 4 and MaxSpectators(+0x2A8) = 4. Runtime equivalent: exec 'net.MaxPlayersOverride 4' (AtCapacity prefers the cvar when > 0). Belt and braces: append ?MaxPlayers=4 to the travel URL so AGameSession::InitOptions (04ABD748) picks it up. Only detour AtCapacity to return false if none of that is workable.
  - risk: MaxPlayers is a globalconfig property; AtCapacity reads the live instance, so writing the CDO alone is not enough once the session exists. Setting MaxPlayers=0 makes AtCapacity always return false (verified) — unlimited, but then nothing rejects a 5th joiner.
- **UNetDriver bandwidth fields (data edit) — MaxInternetClientRate +0xC4, MaxClientRate +0xC8; UPlayer::ConfiguredInternetSpeed +0x3C / CurrentNetSpeed +0x38. RVA shown is the UNetDriver ctor where the defaults are written (04D5C05D / 04D5C067).** @ `04D5BFC0` [both] — Stop 4 players starving the replication channel. Ctor defaults are 10000 and 15000 bytes/sec per client.
  - behaviour: After the net driver exists, write MaxInternetClientRate/MaxClientRate to something like 100000; on each client set UPlayer::ConfiguredInternetSpeed(+0x3C) and CurrentNetSpeed(+0x38) to match (the client's requested NetSpeed is clamped by the server's Max*ClientRate). Patch the data, not the code.
  - risk: Over-raising causes packet loss / MTU fragmentation, especially on the Steam relay path which has its own send-rate limits. Leave net.MaxChannelSize alone (32767 default is already ample).
- **AESGameModeBase::SpawnDefaultPawnAtTransform_Implementation** @ `05DEC54C` [host] — Stop each client join from hijacking the game mode's single-player caches and from spawning the HOST's current ship for the joiner.
  - behaviour: Extend the existing hook. BEFORE calling the original: if the incoming AController is not the host's, swap UESGameInstance::InstancePointer->PlayerData(+0x1C0) to that client's UPlayerData so UGameplayLib::GetPlayerData() and UInventoryLib::GetCurrentShip() resolve to the right ship. AFTER the original returns: restore PlayerData and restore AESGameModeBase::ESPlayerPawn(+0x508), ESPlayerController(+0x510) and PlayerData(+0x518) to the host's objects, keeping the client's pawn only in the mod's per-controller table.
  - risk: The function is ~10.5 KB (0x5DEC54C..0x5DEF078) and calls GetCurrentShip twice plus ISavableInterface::StaticRestoreState — the PlayerData swap must cover the whole call and any early return must restore it. Getting the restore wrong leaves the host's out-of-bounds / world-origin-shifting / invasion logic tracking the client's ship.
- **UGameplayLib::GetESPlayerPawn** @ `013F0CC0` [host] — Return the correct player for server-side ES2 logic that services a specific client (damage, AI targeting, rift/ship-state, NPC spawning).
  - behaviour: Detour: if the mod has a 'current acting controller' set (thread-local, assigned at each RPC/hook entry that services a client) return that controller's AESPawn; otherwise call the original. Covers Blueprint callers too, since exec thunk 01824964 forwards here.
  - risk: 42 direct call sites incl. UHealthComponent::TakeDamage and UBTS_DetectNearestEnemyNative::TickNode (AI target selection). Returning a client pawn where the host pawn was expected (HUD-adjacent code) misbehaves — default to the original whenever no acting controller is set.
- **UGameplayLib::GetESPlayerController** @ `0150D280` [host] — Make loot and interaction credit the player who actually did it — the entire APickupBase pipeline and UInteractComponent go through this.
  - behaviour: Same 'current acting controller' redirect as GetESPlayerPawn. Set the acting controller from APickupBase::OnCollect_Implementation / InteractPullItem / InteractPullAllItems and UInteractComponent entry hooks, where the instigating actor is available.
  - risk: Also used by UES2PlatformActivityHelper and UESGameInstance::StopAndClearAllForceFeedbackEffects, which genuinely want the LOCAL player — keep the redirect opt-in per call site rather than always-on.
- **UGameplayLib::GetPlayerData** @ `0127398C` [host] — Route the 371 call sites that read global player state to the right player's save.
  - behaviour: Detour: return the acting controller's UPlayerData when one is set, else the original global. Body is 28 bytes beginning with a 7-byte RIP-relative mov so a 5-byte MinHook patch fits — verify the trampoline. One hook covers Blueprint too (exec thunk 018C43BC forwards here).
  - risk: Highest blast radius in the mod: 342 distinct functions. Introduce it behind a flag and start by redirecting only a small allow-list of call sites (inventory / XP / credits) rather than globally. UPlayerData is a USaveGame of 8624 bytes — a second instance must be created or loaded per client (see UESGameInstance::CreateNewPlayerData 05DE206C), never aliased.
- **UGameplayLib::IsPlayerController** @ `012D2898` [host] — It currently returns false for every remote client's controller (compares against local index 0).
  - behaviour: Detour: return true if the passed AController is any APlayerController in the mod's player table (or simply Cast<APlayerController>(C) != nullptr on the server); otherwise defer to the original.
  - risk: Some ES2 code may rely on this meaning 'is the LOCAL player' for UI gating — audit its callers before flipping it globally.
- **UGameplayLib::GetPlayerControllerWithoutContext** @ `013EFE78` [host] — Completes the index-0 accessor family (11 call sites).
  - behaviour: Same acting-controller redirect, falling back to the original global lookup.
  - risk: Body is only 24 bytes (mov / test / je / xor / jmp). Confirm MinHook's 5-byte patch does not clobber the je target; if it does, hook the exec thunk (05B92368) and the individual callers instead.
- **UInventoryLib::GetCurrentShip and GetInventoryOfCurrentShip** @ `0121A1E0` [host] — Per-player ship and inventory instead of one shared global (66 + 57 call sites).
  - behaviour: Prefer fixing UGameplayLib::GetPlayerData — both of these read the global PlayerData via UESGameInstance::InstancePointer, so they become correct for free. Hook them individually only if you need finer control.
  - risk: GetCurrentShip returns FShipData by hidden sret pointer (rcx = return slot, rdx would be 'this' if it had one — it is a static, so rcx is the sret slot). A direct detour must honour that ABI and return rcx in rax. Fixing GetPlayerData avoids the issue entirely.

## Open questions

- Valve-side gate on the legacy ISteamNetworking API: I could not verify from the binary whether P2PSessionRequest_t is delivered between arbitrary SteamIDs or only between friends / lobby members / same-game-server peers. Plan for 'must be Steam friends' and test with two real accounts. If it fails, the fallback is for the host to create a Steam lobby via FOnlineSessionSteam::CreateLobbySession (052A0764) and the client to JoinLobbySession (052B3C78) purely to satisfy Valve's gate — the UE transport itself needs neither.
- The packaged DefaultEngine.ini / DefaultGame.ini live inside the .pak (no .ini files exist on disk under ~/es2game). So the runtime values of [OnlineSubsystemSteam] bUseSteamNetworking and bAllowP2PPacketRelay, [/Script/Engine.GameSession] MaxPlayers / MaxSpectators, [/Script/Engine.NetConnection] DefaultMaxChannelSize, and the shipped NetDriverDefinitions array are unknown to me. Read them live via GConfig (RVA 09C26AC0) + GEngineIni (RVA 09AD1158), the AGameSession CDO at +0x2AC/+0x2A8, and GEngine+0x1088.
- Default value of the net.MaxPlayersOverride cvar: I located its TConsoleVariableData<int>* global (RVA 09B2A610) and proved AtCapacity's use of it, but the registration site had no usable symbol so I did not read the literal default. Query 'net.MaxPlayersOverride' in-game before relying on it.
- AESGameModeBase::ESPlayerController (+0x510) has no native writer in the ES2 module — it is presumably assigned from the Blueprint game mode. Confirm at runtime (dump the property after a client joins) so the save/restore in the spawn hook covers whatever actually writes it.
- My .text byte-scan for call/rip-relative references gives counts that may include a small number of false positives, and it attributes a call site to the nearest preceding function symbol. One case initially looked like a false positive (GetESPlayerController inside SpawnDefaultPawnAtTransform) but turned out real at 0x5DEEF02 once I disassembled the full 10.5 KB function. Verify individual call sites with tools/disasm.py (and a large maxbytes) before acting on any specific number.
- Whether ES2's UPlayerData can safely exist more than once per process — it is a USaveGame of 8624 bytes with heavy global coupling, and UESGameInstance::CreateNewPlayerData (05DE206C) exists. Needs a live experiment before committing to the per-player-PlayerData design.
- Under Proton, whether the mod's launcher passes the Steam launch environment (SteamAppId / SteamGameId / SteamClientLaunch) through to the game process. If not, SteamAPI_Init fails and every Steam path degrades silently. Check FSteamSharedModule::IsAvailable (0529083C) and FOnlineSubsystemSteam::GetAppId (052AB3C8) at runtime before enabling Steam transport.
- I did not measure whether the Steam P2P MTU/packet-size behaviour matches what UIpNetDriver assumes for MaxPacket — worth checking UNetConnection::MaxPacket after a Steam connection is established, since SendP2PPacket has different size limits than a raw UDP datagram.
