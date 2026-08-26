#!/usr/bin/env python3
"""Disassemble a function of ES2-Win64-Shipping.exe by name (substring) or RVA, annotating call targets with symbol names.
Usage: disasm.py <rva|name-substring> [maxbytes=2048]"""
import sys, os, re, bisect, pickle, subprocess
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.expanduser('~/es2game/ES2/Binaries/Win64/ES2-Win64-Shipping.exe')
keys, names = pickle.load(open(os.path.join(ROOT, 'sdk/funcs.pkl'), 'rb'))
def sym(addr):
    rva = addr - 0x140000000
    i = bisect.bisect_right(keys, rva) - 1
    if i < 0: return '?'
    return f'{names[i]}+{rva-keys[i]:x}' if rva - keys[i] else names[i]
arg = sys.argv[1]; maxb = int(sys.argv[2]) if len(sys.argv) > 2 else 2048
# An RVA is 0x-prefixed, or a bare hex number that contains at least one digit (a name such as
# "Facade" or "deface" is made of hex letters only and used to be parsed as an address).
if re.fullmatch(r'0[xX][0-9A-Fa-f]+', arg) or (re.fullmatch(r'[0-9A-Fa-f]{6,}', arg) and re.search(r'\d', arg)):
    rva = int(arg, 16)
else:
    cands = [(k, n) for k, n in zip(keys, names) if arg in n]
    if not cands: sys.exit('no symbol matches ' + arg)
    if len(cands) > 1: print('\n'.join(f'  {k:08X} {n}' for k, n in cands[:20]), file=sys.stderr)
    rva = cands[0][0]
i = bisect.bisect_right(keys, rva) - 1
end = keys[i + 1] if i + 1 < len(keys) else rva + maxb
end = min(end, rva + maxb)
print(f'; {names[i]} rva {rva:#x}..{end:#x}')
out = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', '--x86-asm-syntax=intel', f'--start-address={0x140000000+rva:#x}', f'--stop-address={0x140000000+end:#x}', EXE], capture_output=True, text=True).stdout
for line in out.split('\n'):
    m = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
    if not m: continue
    addr = int(m.group(1), 16); ins = m.group(2)
    # annotate absolute targets
    ins2 = re.sub(r'<[^>]*>', '', ins).strip()
    tgt = re.search(r'(?:call|j\w+)\s+(0x[0-9a-f]+)', ins2)
    ann = ''
    if tgt: ann = '   ; ' + sym(int(tgt.group(1), 16))
    else:
        rip = re.search(r'# (0x[0-9a-f]+)', ins)
        if rip: ann = '   ; ' + sym(int(rip.group(1), 16))
    print(f'{addr-0x140000000:08x}  {ins2:<60}{ann}')
