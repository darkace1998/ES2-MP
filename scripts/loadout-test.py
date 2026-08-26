#!/usr/bin/env python3
"""Provenance test: does the host really build the client's pawn from the CLIENT's ship data?

Starts a host, starts a client, tells the client to rewrite its outgoing ship blob (coil_gun ->
scatter_gun) and to send it, then compares the equipped weapons of both players' pawns ON THE HOST.
If the client's pawn shows the rewritten weapon and the host's does not, the loadout came from the client.
"""
import subprocess, sys, os, time, re
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def con(port, cmd, t=120):
    return subprocess.run(['python3', os.path.join(ROOT, 'scripts/console.py'), str(port), cmd],
                          capture_output=True, text=True, timeout=t).stdout

def main():
    frm = sys.argv[1] if len(sys.argv) > 1 else 'coil_gun'
    to  = sys.argv[2] if len(sys.argv) > 2 else 'scatter_gun'
    print(f'== client will rewrite {frm} -> {to} in its outgoing ship blob')
    print(con(27100, 'god 1').strip())      # keep the host alive so it does not die mid-test
    print(con(27101, 'god 1').strip())
    print(con(27101, f'shipdata tweak {frm} {to}').strip())
    print(con(27101, 'shipdata send').strip())
    time.sleep(10)
    # the host applies a client's loadout once per session; force it to re-apply the tweaked blob
    print(con(27100, 'shipdata apply 1').strip())
    time.sleep(8)
    pl = con(27100, 'players')
    print(pl)
    # The name column is a Steam persona (spaces and brackets are routine), so never \S+ for it.
    m = {int(a): b for a, b in re.findall(r'\[(\d+)\]\s+.*?\s+(?:local|remote)\s+pc=\S+\s+pawn=(\S+)', pl)}
    acts = con(27100, 'actors ESPawn 60')
    addr = {}
    for line in acts.split('\n'):
        mm = re.match(r'([0-9A-F]{8,16})\s+\S+\s+(\S+)', line)
        if mm: addr[mm.group(2).split('.')[-1]] = mm.group(1)
    for pid in sorted(m):
        pawn = m[pid]
        a = addr.get(pawn)
        label = 'HOST  ' if pid == 0 else 'CLIENT'
        print(f'--- {label} player {pid}: {pawn}')
        if a: print(con(27100, f'shipdata weapons 0x{a}'))
        else: print('   (pawn address not found)')

if __name__ == '__main__':
    main()
