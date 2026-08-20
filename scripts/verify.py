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

    st = con(HOST, 'shipdata stash')
    check('host holds the client\'s ship loadout', 'player 1:' in st and 'applied=1' in st, st.strip().split('\n')[0])

    # fire routing: client presses the trigger, host-side weapon component must react
    cp = re.search(r'\[1\].*pawn=(\S+)', pl)
    fired = False
    if cp:
        acts = con(HOST, 'actors ESPawn 60')
        addr = None
        for line in acts.split('\n'):
            m = re.match(r'([0-9A-F]{8,16})\s+\S+\s+(\S+)', line)
            if m and m.group(2).endswith(cp.group(1)): addr = m.group(1); break
        if addr:
            pw = re.search(r'([0-9A-F]{16})\s*$', con(HOST, f'props 0x{addr} PrimaryWeapons'), re.M)
            if pw:
                w = pw.group(1)
                before = 'true' in con(HOST, f'props 0x{w} bFireActivated')
                con(CLIENT, 'fire primary on')
                time.sleep(1.5)
                during = 'true' in con(HOST, f'props 0x{w} bFireActivated')
                con(CLIENT, 'fire primary off')
                time.sleep(1)
                after = 'true' in con(HOST, f'props 0x{w} bFireActivated')
                fired = (not before) and during and (not after)
    check('client fire drives the host-side weapon', fired)

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
