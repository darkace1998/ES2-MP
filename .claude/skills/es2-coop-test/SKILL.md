---
name: es2-coop-test
description: Run and interpret the Everspace 2 co-op regression tests — full session bring-up, the 13-check end-to-end verifier, the loadout provenance test and the combat authority test. Use after changing mod code to confirm co-op still works, or to diagnose which co-op feature regressed. Triggers: "verify coop", "run the coop tests", "did I break multiplayer", "test the loadout", "test combat routing".
---

# ES2 co-op regression testing

Every check runs against two live game instances. A full cycle is ~4 minutes.

## Full cycle
```
scripts/coop-session.sh --kill        # host(27100) + client(27101): load save, listen, connect  (~3 min)
python3 scripts/verify.py             # 16 checks, ~2 min
python3 scripts/verify.py --travel    # 18 checks incl. a co-op location jump, ~4 min
scripts/coop-session.sh --kill --steam # same, but the host uses Steam P2P transport
```
`verify.py` prints PASS/FAIL per check and exits non-zero if any failed. What it covers:
roles (listen server / client), same world, player registry, client→host transform flow,
movement authority (`repMove=0`, i.e. no rubberbanding), world origin shifting disabled,
the host holding the client's ship loadout, the client's trigger driving the host-side weapon
component, mission progress reaching the client's own PlayerData, loot drops being mirrored,
death handling and kill attribution being armed, player capacity, and (with `--travel`) that a
jump takes both players along.

## Targeted tests
- **Loadout provenance**: `python3 scripts/loadout-test.py coil_gun scatter_gun` — has the client
  rewrite its outgoing ship blob, forces the host to re-apply it, then prints both players' equipped
  weapons *as the host sees them*. The client's pawn must show the rewritten weapon and the host's
  must not.
- **Steam transport**: `python3 scripts/console.py 27100 steam` self-checks the whole Steam path on
  one machine — subsystem, SteamAPI init, socket subsystem, your own SteamID64 (the join address),
  the resolved net driver and whether it is real P2P or silently degraded to UDP. Note the transport
  must be selected BEFORE the first `listen`.
- **Combat authority**: `python3 scripts/combat-test.py 15` — parks the client next to an isolated
  turret, measures a no-fire window and then a firing window, and reports damage attributable to the
  client's routed fire. Beware ambient NPC crossfire: always compare against the no-fire phase.

## When something fails
1. `python3 scripts/logof.py 27100` / `27101` gives the mod log for each instance; grep for
   `[coop] [net] [loadout] [travel] [combat] [authority]`.
2. `scripts/crash.sh` symbolizes the newest crash (it reads the Proton prefix's
   `AppData/Local/ES2/Saved/Crashes`, resolving ES2 frames via the PDB and mod frames via the
   mod's own PDB).
3. Re-run a single check by hand with `python3 scripts/console.py <port> <command>` (see `es2-console`).

## Cautions
- Only two instances fit in this machine's RAM (~3 GB each) — do not launch a third.
- The host can die while idle near enemies and invalidate a test; `god 1` on the host prevents that.
- Tests assume the default save; `--save NAME` on `coop-session.sh` changes it.
