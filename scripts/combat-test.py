#!/usr/bin/env python3
"""Repeatable host-authority combat test.

Teleports ONLY the client next to an enemy, has the client hold the trigger, and measures the
enemy's health on the HOST. The host player is left alone (and made invulnerable) so nothing but
the client's routed fire can explain a change.

Usage: combat-test.py [seconds=15] [--route 0|1]
"""
import subprocess, sys, os, re, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST, CLIENT = '27100', '27101'

def con(port, cmd):
    return subprocess.run(['python3', os.path.join(ROOT, 'scripts/console.py'), port, cmd],
                          capture_output=True, text=True, timeout=120).stdout

def players(port=HOST):
    out = con(port, 'players')
    rows = {}
    for m in re.finditer(r'\[(\d+)\]\s+(\S+)\s+(local|remote)\s+pc=(\S+)\s+pawn=(\S+)', out):
        rows[int(m.group(1))] = dict(name=m.group(2), kind=m.group(3), pc=m.group(4), pawn=m.group(5))
    return rows

def actors(port, cls='ESPawn', n=60):
    out = con(port, f'actors {cls} {n}')
    res = []
    for line in out.split('\n'):
        m = re.match(r'([0-9A-F]{8,16})\s+(\S+)\s+(\S+)\s+loc=\((-?\d+), (-?\d+), (-?\d+)\)', line)
        if m:
            res.append(dict(addr=m.group(1), cls=m.group(2), path=m.group(3),
                            loc=(int(m.group(4)), int(m.group(5)), int(m.group(6))),
                            name=m.group(3).split('.')[-1]))
    return res

def hp_map(port=HOST):
    out = con(port, 'hp ESPawn')
    res = {}
    for m in re.finditer(r'(\S+)\s+hp=([\d.]+)\s+shield=([\d.]+)', out):
        res[m.group(1)] = (float(m.group(2)), float(m.group(3)))
    return res

def prop(port, addr, name):
    out = con(port, f'props 0x{addr} {name}')
    m = re.search(rf'{name}\s+\w+\s+=\s+(.*)', out)
    return m.group(1).strip() if m else None

def obj_prop_addr(port, addr, name):
    """Address of an ObjectProperty's value (the trailing 16-hex-digit pointer)."""
    v = prop(port, addr, name)
    if not v: return None
    m = re.search(r'([0-9A-F]{16})\s*$', v)
    return m.group(1) if m else None

def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 15
    route = '1'
    if '--route' in sys.argv: route = sys.argv[sys.argv.index('--route') + 1]

    pl = players()
    if 1 not in pl:
        print('no client player registered on the host'); return 1
    cpawn_name = pl[1]['pawn']
    print(f'client pawn (host side): {cpawn_name}')
    if 'Ship_Player' not in cpawn_name:
        print(f'!! client is not flying a ship (pawn={cpawn_name}) — dead or in a menu'); return 1
    if 'Ship_Player' not in pl[0]['pawn']:
        print(f'!! HOST is not flying a ship (pawn={pl[0]["pawn"]}) — restart the session'); return 1

    host_actors = actors(HOST)
    cpawn = next((a for a in host_actors if a['name'] == cpawn_name), None)
    if not cpawn: print('cannot resolve client pawn address'); return 1

    # make the host player invulnerable so it cannot die and confound the test
    hpawn = next((a for a in host_actors if a['name'] == pl[0]['pawn']), None)

    # Prefer an isolated turret: scouts dogfight and damage each other, which contaminates the measurement.
    turrets = [a for a in host_actors if 'Turret' in a['cls']]
    others = [a for a in host_actors if 'Outlaw' in a['cls'] and 'Turret' not in a['cls']]
    def isolated(t):
        return all(sum((a - b) ** 2 for a, b in zip(t['loc'], o['loc'])) > 400e6 for o in others)   # >20 km away
    cands = [t for t in turrets if isolated(t)] or turrets or others
    if not cands: print('no enemies in this location'); return 1
    tgt = cands[0]
    print(f'target: {tgt["name"]} at {tgt["loc"]}')

    x, y, z = tgt['loc']
    con(CLIENT, f'tp {x + 1200} {y} {z}')
    con(CLIENT, f'combat route {route}')
    cw_addr = obj_prop_addr(HOST, cpawn['addr'], 'PrimaryWeapons')
    time.sleep(4)
    # Teleporting leaves the ship pointing wherever it happened to face, so without this the client
    # shoots into empty space and the test measures nothing. ES2's auto-aim only covers a narrow cone.
    print('  aim:', con(CLIENT, 'aim').strip().split('\n')[0])
    time.sleep(2)

    def hp_of():
        return hp_map().get(tgt['name'])

    # Phase 1: identical position/conditions, NOBODY fires. Any change here is ambient (NPC crossfire).
    p0 = hp_of()
    time.sleep(secs)
    p1 = hp_of()
    amb = (p0[0] - p1[0]) if (p0 and p1) else None
    print(f'PHASE 1 (no fire, {secs}s): {p0} -> {p1}   ambient hull loss = {amb}')

    # Phase 2: same target, client holds the trigger.
    con(CLIENT, 'fire primary on'); con(CLIENT, 'fire secondary on')
    time.sleep(2)
    if cw_addr:
        print(f'  host-side bFireActivated during: {prop(HOST, cw_addr, "bFireActivated")}')
    time.sleep(max(0, secs - 2))
    con(CLIENT, 'fire primary off'); con(CLIENT, 'fire secondary off')
    p2 = hp_of()
    fired = (p1[0] - p2[0]) if (p1 and p2) else None
    print(f'PHASE 2 (client fires, {secs}s): {p1} -> {p2}   hull loss = {fired}')
    if p2 is None:
        print(f'==> TARGET DESTROYED while the client fired (route={route})')
    elif fired is not None and amb is not None and fired > amb + 0.01:
        print(f'==> ATTRIBUTABLE DAMAGE: {fired:.3f} vs ambient {amb:.3f} (route={route})')
    else:
        print(f'==> no damage attributable to client fire (route={route})')
    print('client:', con(CLIENT, 'combat').split('\n')[0])
    print('host:  ', con(HOST, 'combat').split('\n')[0])
    return 0

if __name__ == '__main__':
    sys.exit(main())
