#!/usr/bin/env python3
"""Recover a callstack from a UE minidump when the crash XML only carries one frame.

UE's <CallStack> element is often a single ntdll frame for a LowLevelFatalError, but the dump still
holds the crashing thread's stack. This walks that stack and reports every value that points into
ES2-Win64-Shipping.exe or the mod, symbolized. It is a scan, not an unwind, so it shows stale frames
too -- read it as "these functions are on the stack", newest first.

Usage: tools/dump_stackscan.py <UEMinidump.dmp> [--all-threads] [--max N]
"""
import sys, struct, bisect, pickle, os, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]

def parse(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'MDMP', 'not a minidump'
    nstreams, dirrva = u32(d, 8), u32(d, 12)
    streams = {}
    for i in range(nstreams):
        o = dirrva + i * 12
        streams[u32(d, o)] = (u32(d, o + 4), u32(d, o + 8))   # type -> (size, rva)
    return d, streams

def modules(d, streams):
    out = []
    if 4 not in streams: return out
    _, rva = streams[4]
    n = u32(d, rva)
    for i in range(n):
        o = rva + 4 + i * 108
        base, size, name_rva = u64(d, o), u32(d, o + 8), u32(d, o + 20)
        ln = u32(d, name_rva)
        name = d[name_rva + 4: name_rva + 4 + ln].decode('utf-16-le', 'replace')
        out.append((base, size, name.split('\\')[-1]))
    return out

def mem_ranges(d, streams):
    """[(start, size, file_rva)] from both memory list flavours."""
    r = []
    if 5 in streams:
        _, rva = streams[5]
        n = u32(d, rva)
        for i in range(n):
            o = rva + 4 + i * 16
            r.append((u64(d, o), u32(d, o + 8), u32(d, o + 12)))
    if 9 in streams:
        _, rva = streams[9]
        n, base = u64(d, rva), u64(d, rva + 8)
        off = base
        for i in range(n):
            o = rva + 16 + i * 16
            start, size = u64(d, o), u64(d, o + 8)
            r.append((start, size, off))
            off += size
    return r

def read(d, ranges, addr, size):
    for start, sz, rva in ranges:
        if start <= addr and addr + size <= start + sz:
            o = rva + (addr - start)
            return d[o:o + size]
    return None

def threads(d, streams):
    out = []
    if 3 not in streams: return out
    _, rva = streams[3]
    n = u32(d, rva)
    for i in range(n):
        o = rva + 4 + i * 48
        # MINIDUMP_THREAD: tid 0, suspend 4, prio 8/12, Teb 16, Stack{start 24, size 32, rva 36},
        # ThreadContext{size 40, rva 44}. Reading Teb as the stack base yields a bogus, empty range.
        out.append(dict(tid=u32(d, o), stack_start=u64(d, o + 24), stack_size=u32(d, o + 32),
                        stack_rva=u32(d, o + 36), ctx_rva=u32(d, o + 44)))
    return out

def exception(d, streams):
    if 6 not in streams: return None
    _, rva = streams[6]
    return dict(tid=u32(d, rva), code=u32(d, rva + 8), addr=u64(d, rva + 24), ctx_rva=u32(d, rva + 164))

def main():
    path = sys.argv[1]
    all_threads = '--all-threads' in sys.argv
    maxn = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 60
    d, streams = parse(path)
    mods = modules(d, streams)
    ranges = mem_ranges(d, streams)
    exc = exception(d, streams)

    es2 = next((m for m in mods if m[2].lower().startswith('es2-win64')), None)
    mod = next((m for m in mods if m[2].lower() == 'dwmapi.dll'), None)
    print(f"modules: {len(mods)}   ES2 base=0x{es2[0]:x} size=0x{es2[1]:x}" if es2 else "ES2 module not found")
    if mod: print(f"mod dwmapi base=0x{mod[0]:x} size=0x{mod[1]:x}")

    keys, names = pickle.load(open(os.path.join(ROOT, 'sdk/funcs.pkl'), 'rb'))
    def sym_es2(rva):
        i = bisect.bisect_right(keys, rva) - 1
        return f"{names[i][:130]} +{rva - keys[i]:x}" if i >= 0 else '?'

    modcache = {}
    def sym_mod(off):
        if off not in modcache:
            r = subprocess.run(['python3', os.path.join(ROOT, 'tools/symbolize_dll.py'),
                                os.path.join(ROOT, 'mod/build/dwmapi.pdb'), hex(off)],
                               capture_output=True, text=True).stdout.strip()
            modcache[off] = r or '?'
        return modcache[off]

    if exc:
        print(f"\nexception code=0x{exc['code']:x} at 0x{exc['addr']:x}", end='')
        if es2 and es2[0] <= exc['addr'] < es2[0] + es2[1]:
            print(f"  -> ES2 +{exc['addr']-es2[0]:x}  {sym_es2(exc['addr']-es2[0])}")
        else: print()

    tl = threads(d, streams)
    picked = tl if all_threads else [t for t in tl if exc and t['tid'] == exc['tid']] or tl[:1]
    for t in picked:
        rsp = rip = None
        ctx_rva = exc['ctx_rva'] if (exc and t['tid'] == exc['tid'] and exc['ctx_rva']) else t['ctx_rva']
        if ctx_rva:
            c = d[ctx_rva: ctx_rva + 0x100]
            if len(c) >= 0x100:
                rsp, rip = u64(c, 0x98), u64(c, 0xF8)
        print(f"\n=== thread {t['tid']}  stack 0x{t['stack_start']:x} +0x{t['stack_size']:x}"
              + (f"  rsp=0x{rsp:x} rip=0x{rip:x}" if rsp else ""))
        blob = read(d, ranges, t['stack_start'], t['stack_size'])
        if blob is None and t.get('stack_rva'):
            blob = d[t['stack_rva']: t['stack_rva'] + t['stack_size']]
        if not blob:
            print("  (stack memory not in dump)"); continue
        start = t['stack_start']
        if rsp and start <= rsp < start + len(blob):
            off0 = rsp - start
        else:
            off0 = 0
        shown = 0
        for off in range(off0, len(blob) - 8, 8):
            v = struct.unpack_from('<Q', blob, off)[0]
            if es2 and es2[0] <= v < es2[0] + es2[1]:
                print(f"  +{off-off0:#07x}  ES2 +{v-es2[0]:<9x} {sym_es2(v - es2[0])}")
                shown += 1
            elif mod and mod[0] <= v < mod[0] + mod[1]:
                print(f"  +{off-off0:#07x}  MOD +{v-mod[0]:<9x} {sym_mod(v - mod[0])}")
                shown += 1
            if shown >= maxn:
                print(f"  ... (stopped at {maxn} hits; --max N for more)"); break

main()
