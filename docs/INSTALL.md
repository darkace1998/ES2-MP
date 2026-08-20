# ES2 Co-op — Installation

A co-op multiplayer mod for **EVERSPACE 2**. It ships as a single DLL that the game loads at startup;
nothing else is modified and no game file is replaced.

## Before you start

| Requirement | Detail |
|---|---|
| Game | EVERSPACE 2, Steam build **18856734** (v1.4.48188) |
| Who needs it | **Every player.** The host and each client all install the same DLL. |
| Same version | All players must be on the same game build (see *Version guard* below). |
| Steam | Required for invites and for the Steam P2P transport. Direct IP works without it. |
| Network | Direct IP uses **UDP 7777** (host must be reachable / port-forwarded). Steam invites do not need port forwarding. |

The mod is pinned to one game build. It checks the executable's build stamp
(`0xFBD525DD`) at startup and **disables itself** rather than run against a binary it does not know.
That is deliberate — every address it uses was taken from that exact build, so running against another
one would crash rather than misbehave. If Steam updates the game, the mod goes quiet until it is
rebuilt (`docs/PLAN.md` covers regenerating the SDK).

## Install

The mod is a **proxy DLL**: it is named `dwmapi.dll`, a Windows library the game already loads, so the
game pulls it in on its own. It forwards every real call straight through to the system library.

### Linux (Steam Play / Proton)

1. Copy `dwmapi.dll` into the game's binary folder:

   ```
   ~/.local/share/Steam/steamapps/common/EVERSPACE™ 2/ES2/Binaries/Win64/
   ```

   If your Steam library is elsewhere, look for the folder containing `ES2-Win64-Shipping.exe`.

2. Tell Proton to prefer our copy over the built-in one. In Steam: **right-click EVERSPACE 2 →
   Properties → Launch Options**, and set:

   ```
   WINEDLLOVERRIDES="dwmapi=n,b" %command%
   ```

   **Without this the mod will not load** — Wine resolves its own built-in `dwmapi` first and never
   looks at the file you just copied. This is the single most common installation mistake.

3. Launch the game normally from Steam.

### Windows

1. Copy `dwmapi.dll` next to `ES2-Win64-Shipping.exe`:

   ```
   ...\steamapps\common\EVERSPACE™ 2\ES2\Binaries\Win64\
   ```

2. Launch the game. No launch options are needed — Windows searches the executable's own folder before
   the system directory, so it finds the mod automatically.

## Check that it loaded

The mod writes a log the moment it starts:

```
ES2/Binaries/Win64/ES2Coop/logs/es2coop-<pid>.log
```

A healthy start looks like this:

```
=== ES2Coop log start (pid 320) ===
exe base 0000000140000000, build timestamp matches PDB
console listening on 127.0.0.1:27100
```

The quickest visual check: reach the main menu and look for a **MULTIPLAYER** entry directly under
**NEW GAME**. If it is there, the mod is running.

If the log says `exe timestamp ... != expected`, the game was updated and the mod has disabled itself.
If there is no log at all, the DLL is not being loaded — on Linux that is almost always the missing
`WINEDLLOVERRIDES` launch option.

## Playing co-op

The mod is **off until you switch it on**, and with it off the game behaves exactly as it does
unmodded.

### Hosting

1. In the main menu, select **MULTIPLAYER** so it reads `MULTIPLAYER: ON`.
   A lobby panel appears in the top-right with your Steam name in slot 1.
2. Click any **+ INVITE FRIEND** slot. Steam's overlay opens with your friends list, and the invite
   carries a connect string, so whoever accepts is sent straight to your session.
3. Start the game as you normally would — **Continue**, **Load** or **New Game**. The map comes up
   already hosting.

Hosting cannot be switched on mid-map, which is why it is armed in the menu and starts with the level.
Turning MULTIPLAYER back **OFF** also stops advertising "Join Game" to your friends.

### Joining

Accept the Steam invite. Your game launches, the menu shows `MULTIPLAYER: JOINING A FRIEND`, and you
then **load your own save** — you fly *your* ship with *your* loadout, which is read from your save, so
you need one loaded before the connection completes. The join happens automatically once you are in.

### LAN / direct IP (no Steam)

Every player still needs the mod. The host toggles MULTIPLAYER on and loads a game as above. Joining
players load their save, then connect through the mod's console:

```
python3 scripts/console.py 27100 "connect <host-ip>:7777"
```

The console listens on `127.0.0.1:27100` (set `ES2COOP_CONSOLE_PORT` to change it). It is also the
place to inspect a running session — `status`, `players`, `coop`, `net`.

## Uninstall

Delete `dwmapi.dll` (and `dwmapi.pdb`, if present) from `ES2/Binaries/Win64/`, and remove the
`WINEDLLOVERRIDES` launch option on Linux. Nothing else was changed; your saves are untouched.

## Known limitations

- **Up to 4 players.** Two is the tested configuration.
- **Save handling.** Each player keeps their own ship, loadout and progression from their own save.
  World and mission progress follows the host.
- **Joining a friend who is already in-game** works when your game is launched by the invite. Clicking
  Join while your game is already running is not wired up yet.
- **Aim is not perfectly synced.** Other players' weapon fire is mirrored as trigger state, so it reads
  as plausible rather than frame-exact.
- The mod is pinned to one game build and will disable itself after a game update until rebuilt.

## Troubleshooting

| Symptom | Cause |
|---|---|
| No `ES2Coop/logs` folder at all | DLL not loaded. On Linux, check the `WINEDLLOVERRIDES` launch option; confirm the file sits beside `ES2-Win64-Shipping.exe`. |
| Log says `exe timestamp ... != expected` | Game updated. The mod disabled itself on purpose; it needs rebuilding against the new build. |
| No MULTIPLAYER entry in the menu | Mod loaded but the menu was not found — check the log for `[menu]` lines. |
| Steam overlay does not open | Steam overlay disabled for the game, or the game was not launched through Steam. |
| Friend cannot connect over IP | UDP 7777 not reachable. Use a Steam invite instead — it needs no port forwarding. |
