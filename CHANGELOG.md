# Changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versions follow
[Semantic Versioning](https://semver.org/).

Every release is pinned to one game build: the mod compares the game exe's PE timestamp with the value
baked into its SDK headers and disables itself if they differ, so after a game patch it stays inert until
it is regenerated and rebuilt.

## [0.2.3] - 2026-08-26

An audit release. A full read-through of `mod/src`, `scripts/` and `tools/` turned up 46 defects, fixed
in one pass; none came from a player report, and most sit where two players never go — a slot being
reused, a third player, a session that ends and another that starts. It builds clean, but the live
two-instance harness has **not** been run on it yet: treat it as a candidate until `verify.py` is green.
Everyone in a session must run the same version — the `PT` and `HP` payloads and the ghost-query answer
changed shape.

### Fixed
- **Three or more players: a client's ship was frozen for every other client.** A client's registry only
  ever held itself, so the host's position relay for a third player was parsed and dropped — and the host
  switches movement replication off for client-driven ships, so nothing else moved them either. The relay
  now names the pawn by NetGUID and clients track the other players' ships from it.
- **The ghost reconcile could be cancelled before it ran.** The per-destroy report and the answer to the
  client's query shared an op; any placed actor the host destroyed in the seconds before the first query
  latched "answered", and every ghost from the save state stayed on the client for the whole map.
- **Blueprint ran inside the net receive path** — retiring ghosts through `K2_DestroyActor` and spawning
  mirrored pickups, the exact pattern that crashed the client before. Both are queued to the tick now.
- **A dead NPC's aim and lock haunted whatever reused its weapon component's address**, and a recycled NPC
  inherited a previous occupant's "death announced" flag, so it exploded for nobody. Component and actor
  keys are dropped when the pawn is destroyed, and a hull back above zero resets the latch.
- **A target dying triggered whole-object-array scans per weapon component per frame** until the next aim
  update; failed lookups are now remembered for a few frames.
- **The loadout respawn had no failure path.** A `RestartPlayer` that produced no pawn left the
  controller pawn-less (the state the HUD tick faults on) and marked the player applied; a scheduled
  respawn that could not run was silently dropped; a failed import still counted as applied. It now
  re-possesses the placeholder, retries, and only marks applied when the spawner actually took the ship.
- **A newcomer landing in a freed slot spawned in the previous occupant's ship.** The stash is keyed on
  who is on the connection, not on the slot.
- **Menu transport and presence.** MULTIPLAYER OFF never went back to `netdriver ip`, so after one Steam
  session every later LAN host silently came up on SteamNetDriver; the Steam connect presence was only
  ever cleared by that OFF click (friends kept seeing "Join Game" after the session ended, and even when
  `listen` had failed); the plain LAN toggle advertised a `steam.<id>` address an IP server cannot honour.
- **World-origin shifting stayed off for single-player** for the rest of the process after any session.
- **Stale roster names blocked lobby slots**: a player who left kept "occupying" their slot on clients.
- **The mission snapshot to a joiner was one reliable bunch per cached task, all in one frame** (UE drops
  the connection past 256), and the cache survived loading a different save. It is paced now and reset
  with the save.
- **Per-player state survived a slot being reused**: death timers (an immediate respawn during the
  loadout swap), aim/lock/auto-aim, queued damage numbers.
- **Mission XP completed by a client's kill was paid twice to the killer and not at all to the host.**
- **A client could not leave a session** — ReturnToMainMenu was blocked for clients too.
- Stack sizes of mirrored drops were never applied; `guard on` blocked every mirrored pickup; `world xp 0`
  did nothing; `IsFlying` walked the whole reflection chain per player per frame; the host-speed guard
  and the injected menu widgets kept raw pointers across map changes; the reticle setter wrote parms
  without the `CPF_Parm` filter; loadout chunks could be cut inside a UTF-8 sequence or advanced past a
  drop; the reconnect path could `open` a host from the main menu (the ES2 crash the docking path already
  avoids); partner hull/shield was never shown on a client; NPC HP mirroring restarted from the head every
  tick and starved mines and props; an NPC that opened fire before it was replicated stayed "already
  firing" forever.

### Changed
- `god [0|1] [playerId]` — on the host, a player id protects that player's server-side ship.
- NetGUID helpers (`NetGuidOf`, `ActorFromNetGuid`) live in `ue.h`; `combat` and `coop` share them.
- Steam offsets and the three `UShipMovementComponent` speed offsets come from the generator again
  (they had been lost to a duplicate key); `gen_sdk.py` now fails on duplicate keys and missing members.

### Tooling
- `verify.py` could pass green with the mission check silently missing from the tally, matched
  `progress=3` inside `progress=30`, capped actor lists below where the player pawns sit, and crashed
  instead of failing on an unreadable `world`; `combat-test.py` never actually made anyone invulnerable
  (now gods the host, the client's server-side ship and the client); `loadout-test.py` lost players with
  spaces in their names; `coop-session.sh` timeouts are wall-clock (a "240 s" wait could run 44 minutes);
  `crash.sh` mis-symbolised offsets below the first symbol; `kill.sh` no longer kills tooling that merely
  names the exe.

## [0.2.2] - 2026-08-25

Mining, and a ship-speed bug that has been there since multiplayer first worked — it only became visible
once 0.2.1 stopped clients flying the host's ship.

### Fixed
- **A second player made the first one's ship slow.** ES2 treats the player ship as a singleton, so
  whichever pawn initialised second took its movement stats and left the other at the class default:
  reversing, strafing and hovering at ~59% speed while forward flight stayed normal. It reads as the
  *other* player permanently cruising, when in fact your own ship is the degraded one. Both machines now
  hold their own ship's values.
- **A client could not mine ore or crystals — it could not even see them.** Resource nodes are spawned at
  runtime by the location generator, and UE does not run gameplay spawns on a client, so a client's sky
  was simply empty of ore (measured: host 4 nodes, client 0). The host now shares them, after which UE
  handles their appearance and destruction on every machine.
- **Mined resources never reached the client.** Resources drop by item ID, and that was the one
  pickup-spawn path the loot mirroring never hooked, so a mined resource existed only on the host. Each
  player now gets their own instanced pickup — nobody can take another player's drop.

## [0.2.1] - 2026-08-24

Follow-up to 0.2.0, almost all of it from one play session's reports. Two of these made docking — the
headline feature of 0.2.0 — worse than useless in practice, so this is worth taking if you have 0.2.0.

### Fixed
- **A rejoining client flew the host's ship.** The flag that stops the host re-substituting a loadout is
  keyed by player id, and a rejoin reuses the id — so after a dock (which is a disconnect and a
  reconnect) no substitution was armed, and the client kept the placeholder the host had built from its
  own player data: the host's ship model, the host's stats, a near-empty shield. Being destroyed was the
  only way out. Every login now re-applies that player's ship.
- **A client could not damage plant enemies, mines or loot props.** These are level actors that do not
  replicate, so each machine loads and simulates its own private copy. A client kept ones the host had
  already destroyed — measured at host 0 / client 16 in one location, plus five emptied loot containers —
  and never saw damage on the ones that did exist. The host now reconciles them on join, mirrors their
  health, and reports their destruction, keyed by the level path both machines share. Covers plant
  enemies, proximity mines and item containers.
- **A joining client inherited the host's hull condition.** The substitution replaced a joiner's ship
  items but not its condition, so its bars read whatever the host's *saved* ship was — near-zero if that
  save was wrecked — and the first thing that corrected them was dying. The client's own `Health` and
  `ArmorRatio` travel with its loadout and are applied now, and its shield comes up full.
- `shipdata rate N` and `shipdata repairjoin` were declared but unreachable, so neither switch did
  anything. Both are wired up.

### Added
- `shipdata repair [id|all]` (host) restores every player's hull, armour and shield. ES2's hull never
  regenerates, so a save parked at low hull otherwise starts every session one hit from death.
- `shipdata repairjoin 0/1` (host) hands every joining player a repaired hull. Off by default, since it
  otherwise erases real damage.

### Verified
- **XP from a client's kill goes to the client, not the host.** Shipped earlier but never actually seen
  firing, because every kill in the test save awards zero XP by ES2's own design: a 4+ player-level delta
  zeroes the award, the local NPCs carry `XP=0`, and 20% damage share is required. The harness now drives
  the award directly with those gates neutralised and checks both directions.

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

[0.2.2]: https://github.com/darkace1998/ES2-MP/releases/tag/v0.2.2
[0.2.1]: https://github.com/darkace1998/ES2-MP/releases/tag/v0.2.1
[0.2.0]: https://github.com/darkace1998/ES2-MP/releases/tag/v0.2.0
[0.1.1]: https://github.com/darkace1998/ES2-MP/releases/tag/v0.1.1
[0.1.0]: https://github.com/darkace1998/ES2-MP/releases/tag/v0.1.0
