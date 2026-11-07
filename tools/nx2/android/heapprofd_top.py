#!/usr/bin/env python3
"""Top unreleased native allocations from a heapprofd trace, without trace_processor.

Usage: heapprofd_top.py <trace.pftrace> [pid] [count] [unstripped libyuzu-android.so]
"""
import collections, subprocess, sys
def varint(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1; r |= (c & 0x7f) << s; s += 7
        if c < 0x80: return r, i
def fields(b):
    i = 0; n = len(b)
    while i < n:
        k, i = varint(b, i); f, t = k >> 3, k & 7
        if t == 0: v, i = varint(b, i)
        elif t == 1: v = int.from_bytes(b[i:i+8], 'little'); i += 8
        elif t == 5: v = int.from_bytes(b[i:i+4], 'little'); i += 4
        elif t == 2: l, i = varint(b, i); v = b[i:i+l]; i += l
        else: raise ValueError(t)
        yield f, t, v
def packed(v, t):
    if t == 0: return [v]
    out = []; i = 0
    while i < len(v): x, i = varint(v, i); out.append(x)
    return out

def rd(v):
    return dict((a, c) for a, b_, c in fields(v))

def main():
    data = open(sys.argv[1], 'rb').read()
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    lib = sys.argv[4] if len(sys.argv) > 4 else None
    strings, maps, frames, stacks = {}, {}, {}, {}
    dumps = collections.defaultdict(dict)
    for f, t, v in fields(data):
        if f != 1:
            continue
        for pf, pt, pv in fields(v):
            if pf == 12:  # interned data
                for f2, t2, v2 in fields(pv):
                    if f2 in (5, 16, 17):
                        d = rd(v2); strings[d.get(1)] = d.get(2, b'').decode(errors='replace')
                    elif f2 == 6:
                        d = rd(v2); frames[d.get(1)] = (d.get(2), d.get(3), d.get(4, 0))
                    elif f2 in (7, 19):
                        ids, d = [], {}
                        for a, b_, c in fields(v2):
                            if (f2 == 7 and a == 2) or (f2 == 19 and a == 7):
                                ids += packed(c, b_)
                            else:
                                d[a] = c
                        (stacks if f2 == 7 else maps)[d.get(1)] = ids
            elif pf == 37:  # profile packet
                for f2, t2, v2 in fields(pv):
                    if f2 != 5:
                        continue
                    pid = ts = None; samples = []
                    for a, b_, c in fields(v2):
                        if a == 1: pid = c
                        elif a == 9: ts = c
                        elif a == 2: samples.append(rd(c))
                    for s in samples:
                        dumps[(pid, ts)][s.get(1)] = (s.get(2, 0), s.get(3, 0))
    keys = sorted(k for k in dumps if want in (0, k[0]))
    for k in keys:
        print('dump pid %d ts %d: %d MB unreleased' % (k[0], k[1], sum(a - b for a, b in dumps[k].values()) >> 20))
    if not keys:
        return
    cur = dumps[keys[-1]]
    for cs, (a, fr) in sorted(cur.items(), key=lambda kv: -(kv[1][0] - kv[1][1]))[:count]:
        print('=== %d MB' % ((a - fr) >> 20))
        shown = 0
        for fid in stacks.get(cs, []):
            fn, mid, rel = frames[fid]
            name = '/'.join(strings.get(p, '?') for p in maps.get(mid, [])).split('/')[-1]
            if name.startswith('libc.so') or 'heapprofd' in name:
                continue
            sym = strings.get(fn, '')
            if lib and name == 'libyuzu-android.so':
                sym = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', lib, hex(rel)],
                                     capture_output=True, text=True).stdout.split('\n')[0]
            print('  %s %#x %s' % (name, rel, sym[:110]))
            shown += 1
            if shown >= 10:
                break

main()
