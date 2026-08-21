# Changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versions follow
[Semantic Versioning](https://semver.org/).

Every release is pinned to one game build: the mod compares the game exe's PE timestamp with the value
baked into its SDK headers and disables itself if they differ, so after a game patch it stays inert until
it is regenerated and rebuilt.

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

[0.1.1]: https://github.com/darkace1998/ES2-MP/releases/tag/v0.1.1
[0.1.0]: https://github.com/darkace1998/ES2-MP/releases/tag/v0.1.0
