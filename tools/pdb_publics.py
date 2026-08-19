#!/usr/bin/env python3
"""Parse `llvm-pdbutil dump --publics` + `--section-headers` output into a TSV of
   rva<TAB>kind<TAB>mangled<TAB>demangled.  Demangling is done in bulk via llvm-undname.
Usage: pdb_publics.py <sections.txt> <publics.txt> <out.tsv>
"""
import re, subprocess, sys
sec_txt, pub_txt, out = sys.argv[1:4]
# --- sections: index -> virtual address
secs = {}
cur = None
for line in open(sec_txt, errors='replace'):
    m = re.match(r'\s*SECTION HEADER #(\d+)', line)
    if m: cur = int(m.group(1)); continue
    m = re.match(r'\s*([0-9A-Fa-f]+) virtual address', line)
    if m and cur is not None: secs[cur] = int(m.group(1), 16)
print('sections', secs, file=sys.stderr)
# --- publics
recs = []   # (mangled, kind, sec, off)
name = None
for line in open(pub_txt, errors='replace'):
    m = re.search(r'S_PUB32 \[size = \d+\] `(.*)`\s*$', line)
    if m: name = m.group(1); continue
    m = re.search(r'flags = (\S+), addr = (\d+):(\d+)', line)
    if m and name is not None:
        recs.append((name, m.group(1), int(m.group(2)), int(m.group(3))))
        name = None
print('records', len(recs), file=sys.stderr)
# --- demangle in bulk: llvm-undname echoes `mangled\ndemangled\n\n` per input (blank demangled on failure)
names = '\n'.join(r[0] for r in recs) + '\n'
p = subprocess.run(['llvm-undname'], input=names.encode(), capture_output=True)
txt = p.stdout.decode(errors='replace')
dem = {}
for block in txt.split('\n\n'):
    parts = block.split('\n')
    if len(parts) >= 2 and parts[0]:
        dem[parts[0]] = parts[1]
with open(out, 'w') as f:
    for (mang, kind, sec, off) in recs:
        base = secs.get(sec)
        if base is None: continue
        rva = base + off
        d = dem.get(mang, '')
        if d == mang: d = ''
        f.write(f'{rva:08X}\t{kind}\t{mang}\t{d}\n')
print('wrote', out, file=sys.stderr)
