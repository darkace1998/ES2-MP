#!/usr/bin/env python3
"""End-to-end co-op verification against a live host+client session.

Assumes `scripts/coop-session.sh --kill` has already brought both instances up
(host console 27100, client console 27101). Prints PASS/FAIL per check.

Usage: verify.py [--travel]      (--travel also exercises a co-op location jump, ~2 min)
"""
import subprocess, sys, os, re, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST, CLIENT = '27100', '27101'
results = []

def con(port, cmd, t=120):
    try:
        return subprocess.run(['python3', os.path.join(ROOT, 'scripts/console.py'), port, cmd],
                              capture_output=True, text=True, timeout=t).stdout
    except Exception as e:
        return f'<console error: {e}>'

def check(name, ok, detail=''):
    results.append((name, ok, detail))
    print(f'{"PASS" if ok else "FAIL"}  {name}' + (f'   [{detail}]' if detail else ''))
    return ok

def main():
    do_travel = '--travel' in sys.argv

    hs, cs = con(HOST, 'status'), con(CLIENT, 'status')
    check('host is a listen server', 'netmode=ListenServer' in hs, hs.split('\n')[1][:70] if '\n' in hs else '')
    check('client is a net client', 'netmode=Client' in cs, cs.split('\n')[1][:70] if '\n' in cs else '')
    hw = re.search(r'world=(\S+)', hs); cw = re.search(r'world=(\S+)', cs)
    check('both in the same world', bool(hw and cw and hw.group(1) == cw.group(1)),
          f'{hw.group(1) if hw else "?"} vs {cw.group(1) if cw else "?"}')

    pl = con(HOST, 'players')
    n = re.search(r'count=(\d+)', pl)
    check('host registry has 2 players', bool(n and int(n.group(1)) == 2), pl.strip().split('\n')[0])
    check('client knows its player id', 'localId=1' in con(CLIENT, 'players'))

    coop = con(HOST, 'coop')
    rx = re.search(r'rx=(\d+)', coop)
    check('client ship transforms reaching the host', bool(rx and int(rx.group(1)) > 0), f'rx={rx.group(1) if rx else 0}')
    check('client owns its own movement (no rubberbanding)', 'repMove=0' in coop,
          [l.strip() for l in coop.split('\n') if 'p1' in l][:1])

    ti = con(HOST, 'travelinfo')
    check('world origin shifting disabled', 'worldOriginShiftingStack=0' in ti and 'bUseWorldOriginShifting=0' in ti)

    # The client streams its ~25 KB ship loadout to the host in ~54 reliable-RPC chunks; on a fresh
    # connect that can take up to a minute to arrive and apply, so poll rather than sample once.
    st = con(HOST, 'shipdata stash')
    for _ in range(30):
        if 'player 1:' in st and 'applied=1' in st: break
        time.sleep(3); st = con(HOST, 'shipdata stash')
    check('host holds the client\'s ship loadout', 'player 1:' in st and 'applied=1' in st, st.strip().split('\n')[0])
    # Applying the loadout replaces the client's pawn; let that settle before anything reads a pawn
    # or one of its components, or the reads land on the actor that is about to be destroyed.
    time.sleep(6)

    # Fire routing. Sampling the host-side weapon's bFireActivated turned out to be a poor probe: it
    # is only true while the weapon is mid-cycle, and it reads off a pawn the loadout swap or a
    # respawn may have just replaced. The robust signal is the host's own counter — combat::fireApplied
    # increments exactly when the host presses the trigger on that player's server-side controller,
    # which is precisely what "the client's fire drives the host" means.
    # combat-test.py is the deeper end-to-end check (it destroys a real enemy).
    def host_applied():
        m = re.search(r'fireApplied=(\d+)', con(HOST, 'combat'))
        return int(m.group(1)) if m else -1
    a0 = host_applied()
    con(CLIENT, 'fire primary on')
    time.sleep(1.5)
    con(CLIENT, 'fire primary off')
    time.sleep(1)
    a1 = host_applied()
    check('client fire drives the host-side weapon', a1 > a0, f'host fireApplied {a0} -> {a1}')

    # --- shared world state ---
    ws_h, ws_c = con(HOST, 'world'), con(CLIENT, 'world')
    check('mission/dialog sync enabled on both sides', 'missions=1' in ws_h and 'missions=1' in ws_c)

    # drive a real mission change on the host and watch the client's own PlayerData record follow
    tasks = con(HOST, 'missions')
    m = re.search(r'^\s+(\S+)\s+state=\d+\s+stage=\d+\s+progress=(\d+)', tasks, re.M)
    mission_ok = False
    if m:
        tid = m.group(1)
        acts = con(HOST, 'actors MissionTaskBase 20')
        addr = None
        for line in acts.split('\n'):
            mm = re.match(r'([0-9A-F]{8,16})\s', line)
            if mm and tid in con(HOST, f'props 0x{mm.group(1)} MissionTaskID'):
                addr = mm.group(1); break
        if addr:
            want = 3 + (int(m.group(2)) % 5)
            con(HOST, f'call 0x{addr} SetProgress {want}')
            time.sleep(2)
            rec = con(CLIENT, f'missions {tid}')
            mission_ok = f'progress={want}' in rec
            check('mission progress reaches the client\'s own PlayerData', mission_ok, f'{tid} -> progress={want}')
    if not mission_ok and not m:
        check('mission progress reaches the client\'s own PlayerData', False, 'no mission task found to drive')

    # loot: host drops one, the client should materialise its own copy
    before = con(CLIENT, 'loot')
    con(HOST, 'loot test coil_gun 12')
    time.sleep(4)
    after = con(CLIENT, 'loot')
    def applied(t):
        mm = re.search(r'applied=(\d+)', t); return int(mm.group(1)) if mm else -1
    check('loot drop mirrored to the client', applied(after) > applied(before), f'{applied(before)} -> {applied(after)}')

    # --- the client's own ship must be fully built locally (weapons + equipment) ---
    cw = con(CLIENT, 'shipdata weapons')
    prim = re.search(r'PrimaryWeapons: (\d+) slot', cw)
    sec = re.search(r'SecondaryWeapons: (\d+) slot', cw)
    empties = cw.count('(empty)')
    check("client's own ship is armed locally",
          bool(prim and int(prim.group(1)) > 0) and bool(sec and int(sec.group(1)) > 0) and empties == 0,
          f"primary={prim.group(1) if prim else '?'} secondary={sec.group(1) if sec else '?'} empty={empties}")
    lo = con(CLIENT, 'shipdata local')
    # The fill now happens at PreInitializeComponents (early enough for the device/consumable
    # components, which build their slots in InitializeComponents, before PostInit). The PostInit
    # hook remains only as a backstop, so either counter being non-zero means the ship arrived in time.
    filled = any(f'{k}=' in lo and f'{k}=0' not in lo for k in ('prePreInit', 'prePostInit'))
    check('client ship data filled before its components initialise', filled,
          [l for l in lo.split('\n') if 'prePreInit' in l][:1])

    # ES2's ingame HUD widget lives under the GameInstance and caches the pawn at construction, so it
    # survives the loadout pawn swap and then reads a destroyed actor — that is what froze weapon
    # swap, the drive charge and the equipment slots on a client. HudRebindTick must keep it current.
    cst = con(CLIENT, 'status')
    live = re.search(r'^pawn=\S+ (\S+) \((\w+)\)', cst, re.M)
    hud_addr = None
    for line in con(CLIENT, 'find WG_Ingame_HUD_C 8').split('\n'):
        if 'BP_GameInstance' in line:
            m = re.match(r'([0-9A-F]{12,16})', line.strip())
            if m: hud_addr = m.group(1); break
    bound = None
    if hud_addr:
        m = re.search(r'PlayerPawn\s+\S+\s+=\s+\S+\s+(\S+)\s+([0-9A-F]{12,16})',
                      con(CLIENT, f'props 0x{hud_addr} PlayerPawn'))
        if m: bound = m.group(2)
    check("client HUD is bound to the pawn it is actually flying",
          bool(live and bound) and bound.lstrip('0').lower() == live.group(2).lstrip('0').lower(),
          f'hud={bound} live={live.group(2) if live else "?"}')

    # Aim + target lock must reach the host, which is what actually fires for a client. The host's copy
    # of the client's weapon component has no player behind it, so without the sync its FocusLocation is
    # NaN and it has no lock at all. Compare by NetGUID — the actor is a different UObject per machine.
    con(CLIENT, 'lock')
    time.sleep(3)
    def aim_of(port):
        out = con(port, 'aiminfo')
        blk = re.search(r'^\s+p1\s.*?\n\s+focus=\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\).*?\n\s+lock:(.*)$',
                        out, re.M | re.S)
        if not blk: return None, None
        focus = (blk.group(1), blk.group(2), blk.group(3))
        g = re.search(r'primary=\S*?\(guid (\d+)\)', blk.group(4))
        return focus, (g.group(1) if g else None)
    hf, hg = aim_of(HOST)
    cf, cg = aim_of(CLIENT)
    # The host does NOT simply copy the reported point when it knows which actor the player is on: a
    # client's copy of a moving NPC trails the host's by ~600 uu, so firing at the client's world point
    # misses. It aims at its own copy of the target instead. So either the points match (aiming at empty
    # space) or the retarget path is actively running.
    def retargets():
        m = re.search(r'retargeted=(\d+)', con(HOST, 'combat'))
        return int(m.group(1)) if m else 0
    r0 = retargets()
    time.sleep(2)
    r1 = retargets()
    check("client's aim reaches the host",
          (bool(hf and cf) and hf == cf) or r1 > r0,
          f'points {"match" if hf == cf else "differ"}, retargeted {r0}->{r1}')
    # Assert only when the client actually holds a lock — in the suite's start position there may be no
    # target in range, and there is nothing to propagate then. It still fails if the client has a lock
    # and the host does not, which is the regression worth catching.
    # Only meaningful at a realistic lock range. The `lock` console command force-locks the nearest
    # target whatever the distance, and ES2 on the host then quite correctly drops a lock on something
    # 100 km away — that is the game behaving, not a sync failure.
    def dist_to_lock(guid):
        st = con(CLIENT, 'status')
        m = re.search(r'pawnLoc=\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)', st)
        if not m: return None
        me = tuple(float(m.group(i)) for i in (1, 2, 3))
        row = re.search(rf'guid={guid}\s+\S+\s+pos=\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)',
                        con(CLIENT, 'npcpos 60'))
        if not row: return None
        t = tuple(float(row.group(i)) for i in (1, 2, 3))
        return sum((a - b) ** 2 for a, b in zip(me, t)) ** 0.5
    d = dist_to_lock(cg) if (cg and cg != '0') else None
    if cg and cg != '0' and d is not None and d < 10000:
        check("client's target lock reaches the host", hg == cg,
              f'host guid={hg} client guid={cg} at {d:.0f} u')
    else:
        print(f'INFO  target lock: client guid={cg} host guid={hg}'
              + (f' — target {d:.0f} u away, beyond a realistic lock' if d else ' — nothing in lock range here'))

    # A weapon swap must move UWeaponComponent::EquippedSlotIndex on the client...
    def equipped():
        m = re.search(r'PrimaryWeapons: \d+ slot\(s\), equipped=(\d+)', con(CLIENT, 'shipdata weapons'))
        return m.group(1) if m else None
    # ...and the reticle must follow it. Those are two separate legs: the switch itself always worked on
    # the client, but ES2 only calls the crosshair's SetWeaponCategory on the host, so the client used to
    # keep the previous weapon's reticle. Sampling both catches a regression in either one.
    def reticle():
        out = con(CLIENT, 'reticle')          # one sample: the two values must agree at the same instant
        cur = re.search(r'cur=(cat_\w+)', out)
        want = re.search(r'GetSubCategoryID ok=1 -> (cat_\w+)', out)
        return (cur.group(1) if cur else None), (want.group(1) if want else None)
    # A swap is briefly refused right after a respawn (ES2 blocks the next-weapon action until the
    # ship is ready again), so retry rather than sample once.
    before = after = None
    for _ in range(4):
        before = equipped()
        con(CLIENT, 'input nextprimary')
        time.sleep(3)
        after = equipped()
        if before and after and before != after: break
    check('client weapon swap changes the equipped slot', bool(before and after and before != after),
          f'{before} -> {after}')
    shown, want = reticle()
    check("client's reticle matches the equipped weapon", bool(want and shown == want),
          f'crosshair shows {shown}, equipped weapon is {want}')

    # --- client must not simulate damage, and must see NPCs shooting ---
    cb = con(CLIENT, 'combat')
    check('client-side damage suppressed / NPC fire mirroring on', 'npcFireMirror=1' in cb)

    # Damage feedback. ES2 draws the floating number and the hitmarker from
    # UGameplayLib::DamageDealtByPlayerOrPlayerFriend, which only fires for the LOCAL player -- so on the
    # host it stays silent for a client's routed fire and the client, whose own damage path is blocked,
    # had nothing to draw. The host now forwards each hit and the client draws it with ES2's own API.
    # Every forwarded event must arrive: a gap means the coalescing or the deferred drain is dropping.
    def counter(port, name):
        m = re.search(rf'{name}=(\d+)', con(port, 'combat'))
        return int(m.group(1)) if m else -1
    # The client has to actually be ON something: firing from wherever it happens to sit hits nothing
    # and the check measures no damage rather than a broken path.
    # Prefer a TURRET: it does not move, so the client keeps hitting it. A scout dogfights away and the
    # check ends up measuring "landed no hits" instead of the path being broken (same reason
    # combat-test.py defaults to a turret).
    def pick(pred):
        for line in con(HOST, 'actors ESPawn 80').split('\n'):
            if pred(line) and 'loc=(' in line:
                mm = re.search(r'loc=\((-?\d+), (-?\d+), (-?\d+)\)', line)
                if mm: return tuple(int(mm.group(i)) for i in (1, 2, 3))
        return None
    tgt = pick(lambda l: 'Turret' in l) or pick(lambda l: 'BP_Outlaw' in l)
    sent0, shown0 = counter(HOST, 'dmgSent'), counter(CLIENT, 'dmgShown')
    if tgt:
        con(CLIENT, 'god 1')
        con(CLIENT, f'tp {tgt[0] + 900} {tgt[1]} {tgt[2]}')
        time.sleep(7)
        con(CLIENT, 'aim')
        time.sleep(2)
        con(CLIENT, 'fire primary on')
        for _ in range(4):
            time.sleep(2); con(CLIENT, 'aim')      # re-aim: a drifting ship stops landing hits
        con(CLIENT, 'fire primary off')
        time.sleep(2)
    sent1, shown1 = counter(HOST, 'dmgSent'), counter(CLIENT, 'dmgShown')
    if sent1 > sent0:
        check("client sees damage numbers for its own hits",
              (shown1 - shown0) == (sent1 - sent0),
              f'host forwarded {sent1 - sent0}, client drew {shown1 - shown0}')
    else:
        print('INFO  damage numbers: the client landed no hits this run — not exercised')

    # The destruction explosion is a host-local actor (BP_Explosion_Base_C has RemoteRole ROLE_None), so
    # it never replicates -- the client has to spawn its own or the enemy just blinks out.
    ds = counter(HOST, 'deathsSent')
    dp = counter(CLIENT, 'deathsPlayed')
    if ds > 0:
        check('client plays the destruction explosion', dp == ds, f'host reported {ds} deaths, client played {dp}')
    else:
        print('INFO  destruction explosion: nothing died during this run — not exercised')

    rs = con(HOST, 'respawn')
    check('co-op death handling armed', 'respawn=1' in rs and 'vetoGameOverPawn=1' in rs, rs.strip().split('\n')[0][:90])
    at = con(HOST, 'attribution')
    check('kill attribution armed', 'attribution=1' in at)

    mp = con(HOST, 'maxplayers')
    m = re.search(r'MaxPlayers=(\d+)', mp)
    check('host capacity >= 4 players', bool(m and int(m.group(1)) >= 4), mp.strip())

    if do_travel:
        dest = 'S01L01' if 'S01L01' not in (hw.group(1) if hw else '') else 'S01ML01'
        print(f'--- co-op jump to {dest} (this takes ~90s)')
        con(HOST, f'goto {dest}')
        time.sleep(100)
        hs2, cs2 = con(HOST, 'status'), con(CLIENT, 'status')
        hw2 = re.search(r'world=(\S+)', hs2); cw2 = re.search(r'world=(\S+)', cs2)
        check('host travelled and is hosting again', bool(hw2 and hw2.group(1) == dest) and 'ListenServer' in hs2,
              hw2.group(1) if hw2 else '?')
        check('client followed to the new location', bool(cw2 and cw2.group(1) == dest) and 'netmode=Client' in cs2,
              cw2.group(1) if cw2 else '?')

    print()
    passed = sum(1 for _, ok, _ in results if ok)
    print(f'{passed}/{len(results)} checks passed')
    return 0 if passed == len(results) else 1

if __name__ == '__main__':
    sys.exit(main())
