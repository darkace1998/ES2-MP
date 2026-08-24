# Changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versions follow
[Semantic Versioning](https://semver.org/).

Every release is pinned to one game build: the mod compares the game exe's PE timestamp with the value
baked into its SDK headers and disables itself if they differ, so after a game patch it stays inert until
it is regenerated and rebuilt.

## [Unreleased]

### Fixed
- `shipdata repairjoin 1` on the host hands joining players a repaired hull, for sessions whose saves are
  parked at near-zero hull. Off by default, since it otherwise erases real damage.
- `shipdata rate N` and `shipdata repairjoin` were declared but unreachable — both are wired up now.
- **A rejoining client flew the host's ship.** The flag that stops the host re-substituting a loadout is
  keyed by player id, and a rejoin reuses the id — so after a dock (a disconnect and a reconnect) no
  substitution was armed and the client kept the placeholder built from the host's own player data: the
  host's ship model, the host's stats, a near-empty shield, until being destroyed forced a fresh spawn.
  Every login now re-applies that player's ship. Shields also come up full on join.
- **A joining client inherited the host's hull condition.** The loadout substitution replaced a joiner's
  ship items but not its condition, so its bars read whatever the host's saved ship was — zero if the
  host's ship was wrecked — and the first thing that corrected them was dying, since respawn restores to
  full. The client's own `Health`/`ArmorRatio` travel with its loadout and are now applied.
- **A client could not damage plant enemies, mines or similar props.** They are level actors that do not
  replicate, so each machine loads and simulates its own private copy: the client kept ones the host had
  already destroyed (measured: host 0, client 16 in one location) and never saw damage on the ones that
  did exist. The host now reconciles them on join, mirrors their health, and reports their destruction,
  all keyed by the level path both machines share.

### Verified
- **XP from a client's kill goes to the client, not the host.** The path shipped in an earlier release
  but had never been seen firing, because every kill in the test save awards zero XP by ES2's own design
  (a 4+ player-level delta zeroes the award, the local NPCs carry XP=0, and 20% damage share is
  required). The harness now drives the award directly with those gates neutralised and checks both
  directions.

## [0.2.0] - 2026-08-24

### Added
- **Docking works in co-op.** Docking is a level transition, not an animation, so a docked player used to
  drag everyone else into the station map. Each player now docks alone and a client rejoins the host by
  itself on the way out.
- **Mission rewards now reach the client.** Only the host runs mission logic, so a client used to finish
  a mission and be paid nothing. XP, credits and job score are mirrored and applied to each player's own
  save. Mission *item* rewards are not covered yet.
- **An invited friend joins from the title screen**, with no save to load first.
- `objtop`, `uiwatch` and `tools/dump_stackscan.py` for diagnosing object leaks and minidumps.

### Fixed
- **The host ran out of UObjects and crashed after two client joins.** ES2's player-controller Blueprint
  builds a complete in-game menu (map, inventory, perks) in its BeginPlay with no local-controller check,
  so a listen server built one for every joining client and never freed it: ~500,000 objects per join,
  against UE's 2,162,688 ceiling. The host now suppresses widget creation while spawning a remote
  player's controller. Host object count is flat across repeated joins (687,683 -> 687,723 over three),
  and the host keeps exactly one UI -- its own. This also blocked 3-4 player sessions, where the third
  joiner would have crossed the ceiling.
- **The respawn watchdog fought docking.** It treated any non-ship pawn as death, respawning docked
  players every 5s and leaking a pawn per cycle; only ES2's game-over pawn counts as death now.
- **A rejoining client took a fresh player slot** instead of the one it left, which enough dock cycles
  would walk off the end of the 4-slot registry.
- **Joining is faster.** The ship transfer was paced at one chunk per 50 ms (3.9 s for a typical
  loadout) and the joiner's id waited on a HELLO that is always dropped because it races the host's
  PostLogin. The host now hands the id over unprompted and the blob streams several chunks per tick:
  the mod's share of a join went from ~4.3 s to ~1 s. The rest of the wait is ES2 loading the level.

## [0.1.1] - 2026-08-22

### Fixed
- **Steam invites never armed the join.** Steam appends the `connect` rich-presence string to the
  accepting friend's command line verbatim — it adds no `+connect` of its own. The mod published a bare
  address while the invitee looked for `+connect`, so the invite silently did nothing. The prefix is now
  published, and the invitee accepts either form.
- **A Steam host came up on the wrong transport.** The menu path hosted on `IpNetDriver` while
  advertising a `steam.` address, which cannot connect. Inviting a friend now selects the Steam transport
  before the listen server is created and opens the P2P accept gate. LAN hosting is unaffected — it stays
  on IP, so console `connect <ip>:7777` still works.
- **An invitee had no "joining" state.** The menu showed `MULTIPLAYER: ON`, which reads as "I am
  hosting". It now shows `MULTIPLAYER: JOINING A FRIEND`.
- **The lobby overview drew over the settings screen**, covering the graphics options. It now hides
  while any sub-page (Settings, Load, Save, New Game) is open.

### Changed
- `menu` reports `lobbyVisible`, `frontPage`, whether the slots are shown, the pending join and the
  transport, so the above is diagnosable without screenshots.
- The release workflow runs on tags only, instead of on every branch push.

## [0.1.0] - 2026-08-21

First release. Two-player co-op for Everspace 2, delivered as a `dwmapi.dll` proxy: a host-authoritative
listen server built on the engine's own actor replication.

### Added
- Shared world with per-player ship and equipment — each player flies their own loadout, streamed from
  their own save rather than inheriting the host's.
- Client-authoritative ship movement, with the host dead-reckoning the server-side pawn.
- Host-authoritative combat: a client's fire, aim and target lock are routed to the host, which owns all
  damage and kills.
- Mirrored presentation on clients — NPC fire and aim, hitpoints, deaths, damage numbers, loot drops,
  mission and dialog state.
- Instanced loot, kill and XP attribution to the player who earned it, death and respawn instead of game
  over, and co-op location jumps.
- A MULTIPLAYER entry in the game's own main menu, with a lobby overview and Steam invites.
- Steam P2P transport in addition to direct IP.

### Fixed
- 16 bugs found in a full audit, including a console deadlock that hung the game after a single mistyped
  command, and heap corruption from the `call` console command.
- NPC ships jittered on clients: their proxies are now kinematic and driven from the replicated state
  instead of fighting the local physics simulation (~2.6x smoother).
- NPC explosions were inconsistent — they now play when the ship is actually destroyed rather than when
  its hull reaches zero, and no longer depend on a Blueprint function that only ships have.

[0.2.0]: https://github.com/darkace1998/ES2-MP/releases/tag/v0.2.0
[0.1.1]: https://github.com/darkace1998/ES2-MP/releases/tag/v0.1.1
[0.1.0]: https://github.com/darkace1998/ES2-MP/releases/tag/v0.1.0
