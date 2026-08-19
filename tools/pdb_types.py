#!/usr/bin/env python3
"""Index and query the `llvm-pdbutil dump --types` text dump of ES2-Win64-Shipping.pdb.

Subcommands:
  index  <types.txt>                       build <types.txt>.idx (record offsets) + <types.txt>.names.pkl
  layout <types.txt> <ClassName> [--all]   print full class layout (with inherited members, offsets, types)
  enum   <types.txt> <EnumName>            print enum values
  find   <types.txt> <regex>               list definition names matching regex
  vtable <types.txt> <ClassName>           print virtual functions with vtable index (from LF_ONEMETHOD/LF_METHOD 'vftable offset')
  gen    <types.txt> <spec.json> <out.h>   generate C++ header with offsets/sizes for (class, member) pairs

The dump is ~2.7 GB; records are sequential type indices starting at 0x1000.
"""
import sys, re, os, pickle, json
from array import array

HDR_RE = re.compile(rb'^\s+(0x[0-9A-F]+) \| (LF_[A-Z0-9_]+)(?: \[size = \d+\])?(?: `(.*)`)?\s*$')
FIRST_TI = 0x1000

def idx_paths(types_txt):
    return types_txt + '.idx', types_txt + '.names.pkl'

def build_index(types_txt):
    offs = array('Q')
    names = {}          # name -> list of type indices (definitions only, non-forward)
    pending = None      # (ti, kind, name, offset)
    size = os.path.getsize(types_txt)
    with open(types_txt, 'rb') as f:
        pos = 0
        n = 0
        expect = FIRST_TI
        for line in f:
            m = HDR_RE.match(line)
            if m:
                ti = int(m.group(1), 16)
                if ti != expect:
                    # llvm-pdbutil prints records in order; tolerate gaps by padding
                    while expect < ti:
                        offs.append(0); expect += 1
                offs.append(pos); expect = ti + 1
                kind = m.group(2).decode()
                nm = m.group(3)
                pending = (ti, kind, nm.decode(errors='replace') if nm is not None else None)
                n += 1
                if n % 500000 == 0:
                    print(f'  {n} records, {pos*100//size}%', file=sys.stderr)
            elif pending is not None and pending[2] is not None and line.lstrip().startswith(b'options:'):
                if b'forward ref' not in line:
                    names.setdefault(pending[2], []).append(pending[0])
                pending = None
            pos += len(line)
    ip, npk = idx_paths(types_txt)
    with open(ip, 'wb') as f: offs.tofile(f)
    with open(npk, 'wb') as f: pickle.dump(names, f, protocol=4)
    print(f'indexed {len(offs)} records, {len(names)} definition names', file=sys.stderr)

class TypeDB:
    def __init__(self, types_txt):
        self.path = types_txt
        ip, npk = idx_paths(types_txt)
        if not os.path.exists(ip):
            print('building index...', file=sys.stderr); build_index(types_txt)
        self.offs = array('Q')
        with open(ip, 'rb') as f: self.offs.frombytes(f.read())
        with open(npk, 'rb') as f: self.names = pickle.load(f)
        self.f = open(types_txt, 'rb')
        self.cache = {}

    def raw(self, ti):
        """Return list of lines (str) of record ti."""
        if ti in self.cache: return self.cache[ti]
        i = ti - FIRST_TI
        if i < 0 or i >= len(self.offs) or self.offs[i] == 0:
            return None
        self.f.seek(self.offs[i])
        lines = []
        first = True
        for line in self.f:
            if not first and HDR_RE.match(line): break
            first = False
            lines.append(line.decode(errors='replace').rstrip('\n'))
        self.cache[ti] = lines
        return lines

    def rec(self, ti):
        """Parse record into dict."""
        lines = self.raw(ti)
        if not lines: return None
        m = HDR_RE.match(lines[0].encode())
        kind = m.group(2).decode(); name = m.group(3).decode(errors='replace') if m.group(3) is not None else None
        r = {'ti': ti, 'kind': kind, 'name': name, 'lines': lines}
        body = '\n'.join(lines[1:])
        if kind in ('LF_CLASS', 'LF_STRUCTURE', 'LF_INTERFACE', 'LF_UNION'):
            mm = re.search(r'field list: (0x[0-9A-F]+|<no type>)', body)
            r['fieldlist'] = int(mm.group(1), 16) if mm and mm.group(1).startswith('0x') else None
            mm = re.search(r'sizeof (\d+)', body); r['size'] = int(mm.group(1)) if mm else 0
            mm = re.search(r'forward ref \(-> (0x[0-9A-F]+)\)', body)
            r['fwd'] = int(mm.group(1), 16) if mm else None
            r['is_fwd'] = 'forward ref' in body
            mm = re.search(r'vtable: (0x[0-9A-F]+|<no type>)', body)
            r['vtshape'] = int(mm.group(1), 16) if mm and mm.group(1).startswith('0x') else None
        elif kind == 'LF_ENUM':
            mm = re.search(r'field list: (0x[0-9A-F]+)', body); r['fieldlist'] = int(mm.group(1), 16) if mm else None
            mm = re.search(r'underlying type: (0x[0-9A-F]+)(?: \(([^)]*)\))?', body)
            r['underlying'] = (int(mm.group(1), 16), mm.group(2)) if mm else None
            r['is_fwd'] = 'forward ref' in body
            mm = re.search(r'forward ref \(-> (0x[0-9A-F]+)\)', body); r['fwd'] = int(mm.group(1), 16) if mm else None
        elif kind == 'LF_POINTER':
            mm = re.search(r'referent = (0x[0-9A-F]+)(?: \(([^)]*)\))?, mode = ([a-z ]+?), opts = ([^,]*), kind = (\w+)', body)
            r['referent'] = int(mm.group(1), 16); r['referent_name'] = mm.group(2); r['mode'] = mm.group(3); r['opts'] = mm.group(4)
        elif kind == 'LF_MODIFIER':
            mm = re.search(r'referent = (0x[0-9A-F]+)(?: \(([^)]*)\))?, modifiers = (.*)', body)
            r['referent'] = int(mm.group(1), 16); r['referent_name'] = mm.group(2); r['mods'] = mm.group(3)
        elif kind == 'LF_ARRAY':
            mm = re.search(r'size: (\d+), index type: (0x[0-9A-F]+)(?: \([^)]*\))?, element type: (0x[0-9A-F]+)(?: \(([^)]*)\))?', body)
            r['size'] = int(mm.group(1)); r['elem'] = int(mm.group(3), 16); r['elem_name'] = mm.group(4)
        elif kind == 'LF_BITFIELD':
            mm = re.search(r'type = (0x[0-9A-F]+)(?: \(([^)]*)\))?, bit offset = (\d+), # bits = (\d+)', body)
            r['base'] = int(mm.group(1), 16); r['base_name'] = mm.group(2); r['bitoff'] = int(mm.group(3)); r['bits'] = int(mm.group(4))
        elif kind in ('LF_PROCEDURE', 'LF_MFUNCTION'):
            mm = re.search(r'return type = (0x[0-9A-F]+)(?: \(([^)]*)\))?, # args = (\d+), param list = (0x[0-9A-F]+)', body)
            r['ret'] = int(mm.group(1), 16); r['ret_name'] = mm.group(2); r['nargs'] = int(mm.group(3)); r['args'] = int(mm.group(4), 16)
            if kind == 'LF_MFUNCTION':
                mm = re.search(r'class type = (0x[0-9A-F]+), this type = (0x[0-9A-F]+)', body)
                r['cls'] = int(mm.group(1), 16)
        elif kind == 'LF_ARGLIST':
            r['argtis'] = [int(x, 16) for x in re.findall(r'(0x[0-9A-F]+)', body)]
        elif kind == 'LF_FIELDLIST':
            r['fields'] = self._parse_fieldlist(lines[1:])
        return r

    def _parse_fieldlist(self, lines):
        fields = []
        i = 0
        while i < len(lines):
            ln = lines[i].strip()
            nxt = lines[i+1].strip() if i + 1 < len(lines) else ''
            if ln.startswith('- LF_MEMBER'):
                m = re.match(r'- LF_MEMBER \[name = `(.*)`, Type = (0x[0-9A-F]+)(?: \(([^)]*)\))?, offset = (\d+), attrs = (.*)\]$', ln)
                if m: fields.append({'k': 'member', 'name': m.group(1), 'type': int(m.group(2), 16), 'type_name': m.group(3), 'offset': int(m.group(4)), 'attrs': m.group(5)})
            elif ln.startswith('- LF_BCLASS'):
                m = re.match(r'type = (0x[0-9A-F]+), offset = (\d+), attrs = (.*)', nxt)
                if m: fields.append({'k': 'base', 'type': int(m.group(1), 16), 'offset': int(m.group(2))}); i += 1
            elif ln.startswith('- LF_VBCLASS') or ln.startswith('- LF_IVBCLASS'):
                m = re.match(r'base = (0x[0-9A-F]+), vbptr = (0x[0-9A-F]+), vbptr offset = (\d+), vtable index = (\d+)', nxt)
                if m: fields.append({'k': 'vbase', 'type': int(m.group(1), 16), 'vbptr_offset': int(m.group(3)), 'vtable_index': int(m.group(4))}); i += 2
            elif ln.startswith('- LF_VFUNCTAB'):
                m = re.match(r'- LF_VFUNCTAB type = (0x[0-9A-F]+)', ln)
                if m: fields.append({'k': 'vfunctab', 'type': int(m.group(1), 16)})
            elif ln.startswith('- LF_ONEMETHOD'):
                m = re.match(r'- LF_ONEMETHOD \[name = `(.*)`\]$', ln)
                m2 = re.match(r'type = (0x[0-9A-F]+), vftable offset = (-?\d+), attrs = (.*)', nxt)
                if m and m2:
                    fields.append({'k': 'method', 'name': m.group(1), 'type': int(m2.group(1), 16), 'vft': int(m2.group(2)), 'attrs': m2.group(3)}); i += 1
            elif ln.startswith('- LF_METHOD'):
                m = re.match(r'- LF_METHOD \[name = `(.*)`, # overloads = (\d+), overload list = (0x[0-9A-F]+)\]', ln)
                if m: fields.append({'k': 'methodlist', 'name': m.group(1), 'list': int(m.group(3), 16)})
            elif ln.startswith('- LF_STMEMBER'):
                m = re.match(r'- LF_STMEMBER \[name = `(.*)`, type = (0x[0-9A-F]+)(?: \([^)]*\))?, attrs = (.*)\]', ln)
                if m: fields.append({'k': 'static', 'name': m.group(1), 'type': int(m.group(2), 16)})
            elif ln.startswith('- LF_NESTTYPE'):
                m = re.match(r'- LF_NESTTYPE \[name = `(.*)`, parent = (0x[0-9A-F]+)', ln)
                if m: fields.append({'k': 'nested', 'name': m.group(1), 'type': int(m.group(2), 16)})
            elif ln.startswith('- LF_ENUMERATE'):
                m = re.match(r'- LF_ENUMERATE \[(.*) = (-?\d+)\]', ln)
                if m: fields.append({'k': 'enum', 'name': m.group(1), 'value': int(m.group(2))})
            elif ln.startswith('- LF_INDEX'):
                m = re.match(r'- LF_INDEX continuation = (0x[0-9A-F]+)', ln)
                if m:
                    cont = self.rec(int(m.group(1), 16))
                    if cont and cont.get('fields'): fields.extend(cont['fields'])
            i += 1
        return fields

    # ---------- helpers ----------
    def resolve_def(self, name):
        """Definition type index for a class/struct/enum name (prefer largest sizeof)."""
        tis = self.names.get(name)
        if not tis: return None
        best = None
        for ti in tis:
            r = self.rec(ti)
            if r is None: continue
            if best is None or r.get('size', 0) > best.get('size', 0): best = r
        return best

    def deref_fwd(self, ti):
        r = self.rec(ti)
        if r and r.get('is_fwd'):
            if r.get('fwd'): return self.rec(r['fwd'])
            d = self.resolve_def(r['name']) if r.get('name') else None
            return d or r
        return r

    def type_name(self, ti, name_hint=None, depth=0):
        if ti < FIRST_TI:
            return name_hint or BUILTIN.get(ti, f'T{ti:#x}')
        r = self.rec(ti)
        if r is None: return f'T{ti:#x}'
        k = r['kind']
        if k in ('LF_CLASS', 'LF_STRUCTURE', 'LF_UNION', 'LF_ENUM', 'LF_INTERFACE'):
            return r['name']
        if k == 'LF_POINTER':
            inner = self.type_name(r['referent'], r.get('referent_name'), depth+1)
            return inner + (' &' if 'ref' in r['mode'] else ' *')
        if k == 'LF_MODIFIER':
            inner = self.type_name(r['referent'], r.get('referent_name'), depth+1)
            return (r['mods'] + ' ' + inner) if r['mods'] and r['mods'] != 'None' else inner
        if k == 'LF_ARRAY':
            inner = self.type_name(r['elem'], r.get('elem_name'), depth+1)
            esz = self.sizeof(r['elem'])
            cnt = r['size'] // esz if esz else '?'
            return f'{inner}[{cnt}]'
        if k == 'LF_BITFIELD':
            return f"{self.type_name(r['base'], r.get('base_name'), depth+1)} : {r['bits']} @bit{r['bitoff']}"
        if k in ('LF_PROCEDURE', 'LF_MFUNCTION'):
            args = self.rec(r['args'])
            al = ', '.join(self.type_name(a) for a in (args or {}).get('argtis', []))
            return f"{self.type_name(r['ret'], r.get('ret_name'))} ({al})"
        return k

    def sizeof(self, ti):
        if ti < FIRST_TI: return BUILTIN_SIZE.get(ti, 0)
        r = self.deref_fwd(ti)
        if r is None: return 0
        k = r['kind']
        if k in ('LF_CLASS', 'LF_STRUCTURE', 'LF_UNION', 'LF_INTERFACE'): return r.get('size', 0)
        if k == 'LF_ENUM': return self.sizeof(r['underlying'][0]) if r.get('underlying') else 4
        if k == 'LF_POINTER': return 8
        if k == 'LF_MODIFIER': return self.sizeof(r['referent'])
        if k == 'LF_ARRAY': return r['size']
        if k == 'LF_BITFIELD': return self.sizeof(r['base'])
        return 0

    def layout(self, name_or_ti, base_off=0, out=None, seen=None, own_only=False):
        """Collect (offset, size, typename, name, owner) for all data members incl. bases."""
        if out is None: out = []
        r = self.resolve_def(name_or_ti) if isinstance(name_or_ti, str) else self.deref_fwd(name_or_ti)
        if r is None: return out
        fl = self.rec(r['fieldlist']) if r.get('fieldlist') else None
        if not fl: return out
        for fld in fl['fields']:
            if fld['k'] == 'base' and not own_only:
                b = self.deref_fwd(fld['type'])
                self.layout(b['ti'], base_off + fld['offset'], out, seen)
            elif fld['k'] == 'vfunctab':
                out.append((base_off, 8, 'void**', '__vftable', r['name']))
            elif fld['k'] == 'member':
                out.append((base_off + fld['offset'], self.sizeof(fld['type']), self.type_name(fld['type'], fld.get('type_name')), fld['name'], r['name']))
        return out

    def bases(self, name):
        r = self.resolve_def(name)
        if r is None: return []
        fl = self.rec(r['fieldlist']) if r.get('fieldlist') else None
        res = []
        for fld in (fl or {}).get('fields', []):
            if fld['k'] == 'base':
                b = self.deref_fwd(fld['type']); res.append((b['name'], fld['offset']))
        return res

    def vtable(self, name):
        """Return list of (vft_index, method name, signature) for virtuals declared in this class (not inherited)."""
        r = self.resolve_def(name)
        if r is None: return []
        fl = self.rec(r['fieldlist']) if r.get('fieldlist') else None
        res = []
        for fld in (fl or {}).get('fields', []):
            if fld['k'] == 'method' and fld['vft'] >= 0:
                res.append((fld['vft'] // 8, fld['name'], self.type_name(fld['type'])))
            elif fld['k'] == 'methodlist':
                ml = self.raw(fld['list'])
                for ln in (ml or [])[1:]:
                    m = re.search(r'type = (0x[0-9A-F]+), vftable offset = (-?\d+)', ln)
                    if m and int(m.group(2)) >= 0:
                        res.append((int(m.group(2)) // 8, fld['name'], self.type_name(int(m.group(1), 16))))
        return sorted(res)

BUILTIN = {0x0003: 'void', 0x0010: 'int8', 0x0011: 'int16', 0x0012: 'int32', 0x0013: 'int64', 0x0020: 'uint8', 0x0021: 'uint16', 0x0022: 'uint32', 0x0023: 'uint64',
           0x0030: 'bool', 0x0040: 'float', 0x0041: 'double', 0x0070: 'char', 0x0071: 'wchar_t', 0x0074: 'int', 0x0075: 'unsigned', 0x0076: 'int64', 0x0077: 'uint64',
           0x0068: 'int8', 0x0069: 'uint8', 0x0072: 'short', 0x0073: 'unsigned short', 0x007a: 'char16_t', 0x007b: 'char32_t', 0x0600: 'void*', 0x0603: 'void*',
           0x0620: 'char*', 0x0670: 'char*', 0x0674: 'int*', 0x0675: 'unsigned*', 0x0603: 'void *'}
BUILTIN_SIZE = {0x0003: 0, 0x0010: 1, 0x0011: 2, 0x0012: 4, 0x0013: 8, 0x0020: 1, 0x0021: 2, 0x0022: 4, 0x0023: 8, 0x0030: 1, 0x0040: 4, 0x0041: 8, 0x0070: 1, 0x0071: 2,
                0x0074: 4, 0x0075: 4, 0x0076: 8, 0x0077: 8, 0x0068: 1, 0x0069: 1, 0x0072: 2, 0x0073: 2, 0x007a: 2, 0x007b: 4}
for k in list(BUILTIN):
    if k >= 0x0600: BUILTIN_SIZE[k] = 8

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    cmd, types_txt = sys.argv[1], sys.argv[2]
    if cmd == 'index':
        build_index(types_txt); return
    db = TypeDB(types_txt)
    if cmd == 'layout':
        name = sys.argv[3]
        r = db.resolve_def(name)
        if r is None: print('not found'); return
        print(f"// {r['kind']} {name} sizeof={r['size']} bases={db.bases(name)}")
        for off, sz, tn, nm, owner in db.layout(name):
            print(f'  0x{off:04X} (+{sz:4}) {tn:50s} {nm}   // {owner}')
    elif cmd == 'enum':
        r = db.resolve_def(sys.argv[3])
        fl = db.rec(r['fieldlist'])
        for f in fl['fields']:
            if f['k'] == 'enum': print(f"  {f['name']} = {f['value']}")
    elif cmd == 'find':
        rx = re.compile(sys.argv[3])
        for n in sorted(db.names):
            if rx.search(n): print(n)
    elif cmd == 'vtable':
        for idx, nm, sig in db.vtable(sys.argv[3]): print(f'  [{idx:3}] {nm}  {sig}')
    elif cmd == 'gen':
        spec = json.load(open(sys.argv[3]))
        gen_header(db, spec, sys.argv[4])

def gen_header(db, spec, out):
    """spec: {"classes": {"UObject": ["ClassPrivate", "NamePrivate", ...], ...}, "sizes": ["UObject", ...]}"""
    lines = ['// AUTO-GENERATED by tools/pdb_types.py from ES2-Win64-Shipping.pdb — do not edit', '#pragma once', '#include <cstdint>', 'namespace es2sdk {']
    for cls, members in spec.get('classes', {}).items():
        lay = db.layout(cls)
        r = db.resolve_def(cls)
        if r is None:
            print(f'WARNING: class {cls} not found', file=sys.stderr); continue
        lines.append(f'  namespace {cls} {{')
        lines.append(f'    constexpr uint32_t __size = {r["size"]};')
        bymember = {}
        for off, sz, tn, nm, owner in lay:
            bymember.setdefault(nm, (off, sz, tn, owner))
        for m in members:
            if m not in bymember:
                print(f'WARNING: {cls}::{m} not found', file=sys.stderr); lines.append(f'    // MISSING {m}'); continue
            off, sz, tn, owner = bymember[m]
            lines.append(f'    constexpr uint32_t {m} = 0x{off:X}; // {tn} (size {sz}) from {owner}')
        lines.append('  }')
    lines.append('}')
    open(out, 'w').write('\n'.join(lines) + '\n')
    print('wrote', out, file=sys.stderr)

if __name__ == '__main__':
    main()
