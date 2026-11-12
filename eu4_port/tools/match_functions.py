"""Match the functions of two builds of eu4.exe (Steam and GOG 1.37.5).

Both builds come from the same source and differ by shifted addresses, so a function is
identified by its instructions with every address removed: branch targets and RIP-relative
displacements are replaced by placeholders. Function bounds come from .pdata.

Usage: python -I match_functions.py STEAM_EXE GOG_EXE OUT_JSON

The output holds the matched function pairs and a map of data addresses (globals, vtables,
strings) built from RIP-relative references of matched instructions. map_rvas.py uses it.
"""
import hashlib
import json
import re
import struct
import sys
from collections import Counter, defaultdict

import capstone

BRANCH = re.compile(r'^(call|jmp|j[a-z]+|loop[a-z]*)$')
HEX = re.compile(r'0x[0-9a-f]+')
RIP = re.compile(r'rip ([+-]) (0x[0-9a-f]+)')


class Image:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        o = struct.unpack_from('<I', raw, 0x3c)[0]
        ns = struct.unpack_from('<H', raw, o + 6)[0]
        oh = o + 24
        so = oh + struct.unpack_from('<H', raw, o + 20)[0]
        self.size = struct.unpack_from('<I', raw, oh + 56)[0]
        self.sections = {}
        img = bytearray(self.size)
        for i in range(ns):
            name = raw[so + i * 40:so + i * 40 + 8].rstrip(b'\0').decode()
            vs, va, rs, rp = struct.unpack_from('<IIII', raw, so + i * 40 + 8)
            n = min(vs or rs, rs) if rs else 0
            img[va:va + n] = raw[rp:rp + n]
            self.sections[name] = (va, max(vs, rs))
        self.img = bytes(img)
        pva, psize = self.sections['.pdata']
        funcs = set()
        for k in range(0, psize - 11, 12):
            b, e, u = struct.unpack_from('<III', self.img, pva + k)
            if b == 0:
                break
            if u & 1:      # chained unwind info: part of the parent function, keep the range
                pass
            funcs.add((b, e))
        self.funcs = sorted(funcs)


def normalize(image, begin, end, md):
    """Return (digest, list of (offset, size, rip_target or None, branch_target or None))."""
    h = hashlib.blake2b(digest_size=12)
    insns = []
    code = image.img[begin:end]
    for ins in md.disasm_lite(code, begin):
        addr, size, mn, ops = ins
        rip_t = br_t = None
        m = RIP.search(ops)
        if m:
            d = int(m.group(2), 16)
            rip_t = addr + size + (d if m.group(1) == '+' else -d)
            ops = RIP.sub('rip+X', ops)
        if BRANCH.match(mn) and ops.startswith('0x'):
            br_t = int(ops, 16)
            ops = 'T'
        h.update((mn + ' ' + ops + ';').encode())
        insns.append((addr - begin, size, rip_t, br_t))
    return h.hexdigest(), insns


def main():
    steam, gog, out = sys.argv[1:4]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    imgs = [Image(steam), Image(gog)]
    sigs = []
    for im in imgs:
        s = []
        for b, e in im.funcs:
            d, ins = normalize(im, b, e, md)
            s.append((b, e, d, ins))
        sigs.append(s)
        print('functions', len(s), file=sys.stderr)

    # 1) unique digests on both sides are anchors
    cnt = [Counter(x[2] for x in s) for s in sigs]
    by = [defaultdict(list) for _ in sigs]
    for i, s in enumerate(sigs):
        for k, x in enumerate(s):
            by[i][x[2]].append(k)
    pairs = {}
    for d, ks in by[0].items():
        if cnt[0][d] == 1 and cnt[1].get(d) == 1:
            pairs[ks[0]] = by[1][d][0]
    print('unique anchors', len(pairs), file=sys.stderr)

    # 2) fill the gaps between anchors in address order: equal digests in the same order
    anchors = sorted(pairs.items())
    filled = 0
    for (a0, b0), (a1, b1) in zip(anchors, anchors[1:]):
        ga = list(range(a0 + 1, a1))
        gb = list(range(b0 + 1, b1))
        if not ga or not gb:
            continue
        j = 0
        for i in ga:
            while j < len(gb) and sigs[1][gb[j]][2] != sigs[0][i][2]:
                j += 1
            if j < len(gb):
                pairs[i] = gb[j]
                j += 1
                filled += 1
    print('ordered fill', filled, file=sys.stderr)

    # 3) branch targets of matched functions match more functions (callees)
    fstart = [{x[0]: k for k, x in enumerate(s)} for s in sigs]
    data = defaultdict(Counter)
    changed = True
    while changed:
        changed = False
        for ka, kb in list(pairs.items()):
            ia, ib = sigs[0][ka][3], sigs[1][kb][3]
            for (oa, sa, ra, ba), (ob, sb, rb, bb) in zip(ia, ib):
                if ba is not None and bb is not None:
                    fa, fb = fstart[0].get(ba), fstart[1].get(bb)
                    if fa is not None and fb is not None and fa not in pairs and \
                            sigs[0][fa][2] == sigs[1][fb][2]:
                        pairs[fa] = fb
                        changed = True
    for ka, kb in pairs.items():
        for (oa, sa, ra, ba), (ob, sb, rb, bb) in zip(sigs[0][ka][3], sigs[1][kb][3]):
            if ra is not None and rb is not None:
                data[ra][rb] += 1
    data_map = {}
    for a, c in data.items():
        (b, n), = c.most_common(1)
        if n * 2 > sum(c.values()):
            data_map[a] = b
    print('functions matched', len(pairs), 'of', len(sigs[0]), '; data addresses', len(data_map),
          file=sys.stderr)
    json.dump({
        'steam_size': imgs[0].size, 'gog_size': imgs[1].size,
        'functions': [[sigs[0][a][0], sigs[0][a][1], sigs[1][b][0], sigs[1][b][1]]
                      for a, b in sorted(pairs.items())],
        'unmatched': [[x[0], x[1]] for k, x in enumerate(sigs[0]) if k not in pairs],
        'data': sorted([a, b] for a, b in data_map.items()),
    }, open(out, 'w'))


if __name__ == '__main__':
    main()
