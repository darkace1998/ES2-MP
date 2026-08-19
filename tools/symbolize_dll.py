#!/usr/bin/env python3
"""Symbolize offsets (relative to image base) in our mod DLL using its PDB publics.
Usage: symbolize_dll.py <dll.pdb> <hexoff> [hexoff...]"""
import re, subprocess, bisect, sys
pdb = sys.argv[1]
txt = subprocess.run(['llvm-pdbutil', 'dump', '--publics', pdb], capture_output=True, text=True).stdout
sec = subprocess.run(['llvm-pdbutil', 'dump', '--section-headers', pdb], capture_output=True, text=True).stdout
secs = {}
cur = None
for line in sec.split('\n'):
    m = re.match(r'\s*SECTION HEADER #(\d+)', line)
    if m: cur = int(m.group(1))
    m = re.match(r'\s*([0-9A-Fa-f]+) virtual address', line)
    if m and cur: secs[cur] = int(m.group(1), 16)
syms = []
name = None
for line in txt.split('\n'):
    m = re.search(r'S_PUB32 \[size = \d+\] `(.*)`', line)
    if m: name = m.group(1); continue
    m = re.search(r'addr = (\d+):(\d+)', line)
    if m and name:
        syms.append((secs.get(int(m.group(1)), 0) + int(m.group(2)), name)); name = None
syms.sort(); keys = [s[0] for s in syms]
for a in sys.argv[2:]:
    off = int(a, 16)
    i = bisect.bisect_right(keys, off) - 1
    if i < 0: print(hex(off), '?'); continue
    nm = syms[i][1]
    d = subprocess.run(['llvm-undname', nm], capture_output=True, text=True).stdout.strip().split('\n')
    dem = d[-1] if d else nm
    print(f'{off:#x} {dem} +{off - syms[i][0]:x}')
