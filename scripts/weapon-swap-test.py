#!/usr/bin/env python3
"""Compare what a weapon switch updates on the host versus on the client.

Three things should move together when the player switches weapon:
  EquippedSlotIndex   the weapon component's own state  (does the switch happen at all?)
  CurrentSlotIndex    the equipment bar's highlight     (is the pawn -> HUD event chain alive?)
  WeaponCategory      the crosshair's reticle shape     (is the crosshair leg of that chain alive?)

Reading all three on both machines across the same switch is what separates "the switch never
happened" from "it happened but the HUD ignored it" from "the HUD followed but the crosshair did not".
"""
import subprocess, sys, os, re, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORTS = {'host': '27100', 'client': '27101'}


def con(port, cmd):
    try:
        return subprocess.run(['python3', os.path.join(ROOT, 'scripts/console.py'), port, cmd],
                              capture_output=True, text=True, timeout=120).stdout
    except subprocess.TimeoutExpired:
        return ''


def widgets(port):
    """Resolve the HUD, its crosshair, its equipment bar and the live weapon component."""
    out = con(port, 'find WG_Ingame_HUD_C 10')
    hud = next((m.group(0) for line in out.split('\n')
                if not re.search(r'Default__|GeneratedClass|Package', line)
                for m in [re.match(r'[0-9A-F]{16}', line)] if m), None)
    if not hud:
        return None
    def tail_ptr(text, marker):
        for line in text.split('\n'):
            if marker in line:
                m = re.search(r'([0-9A-F]{16})\s*$', line)
                if m:
                    return m.group(1)
        return None
    cross = tail_ptr(con(port, f'props 0x{hud} Crosshair'), '0x0378 Crosshair ')
    equip = tail_ptr(con(port, f'props 0x{hud} PrimariesAndDevices'), 'PrimariesAndDevices')
    wc = tail_ptr(con(port, f'props 0x{equip} WeaponComponent'), '0x0398') if equip else None
    return dict(hud=hud, cross=cross, equip=equip, wc=wc)


def sample(port, w):
    """The three numbers that should move together."""
    idx = re.search(r'\+0x08D0\s+(-?\d+)', con(port, f'peek 0x{w["wc"]} 8D0 1 d'))
    cur = re.search(r'CurrentSlotIndex\s+\w+\s+=\s+(-?\d+)', con(port, f'props 0x{w["equip"]} CurrentSlotIndex'))
    cat = re.search(r'(cat_\w+)', con(port, f'props 0x{w["cross"]} WeaponCategory'))
    return dict(equipped=idx.group(1) if idx else '?',
                highlight=cur.group(1) if cur else '?',
                reticle=cat.group(1) if cat else '?')


def owner_of(port, w):
    m = re.search(r'=\s+\S+\s+(\S*BP_Ship_Player_C_\d+)\.PrimaryWeapons0',
                  con(port, f'props 0x{w["cross"]} PlayerWeaponComponent'))
    return m.group(1).split('.')[-1] if m else '?'


def main():
    swaps = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 2
    w = {}
    for role, port in PORTS.items():
        w[role] = widgets(port)
        if not w[role] or not w[role]['wc']:
            print(f'!! cannot resolve HUD widgets on the {role} — is it alive and in a level?')
            return 1

    for role, port in PORTS.items():
        pawn = re.search(r'pawn=\S+\s+(\S*BP_Ship_Player_C_\d+)', con(port, 'status'))
        print(f'{role:6} live pawn={pawn.group(1).split(".")[-1] if pawn else "?":<28} '
              f'crosshair bound to={owner_of(port, w[role])}')
    print()

    hdr = f'{"":6} {"equipped":>10} {"highlight":>10} {"reticle":>16}'
    for n in range(swaps + 1):
        print(f'--- {"start" if n == 0 else f"after swap {n}"} ---')
        print(hdr)
        for role, port in PORTS.items():
            s = sample(port, w[role])
            print(f'{role:6} {s["equipped"]:>10} {s["highlight"]:>10} {s["reticle"]:>16}')
        if n == swaps:
            break
        for role, port in PORTS.items():
            con(port, f'call 0x{w[role]["wc"]} NextWeapon false')
        time.sleep(3)
    return 0


if __name__ == '__main__':
    sys.exit(main())
