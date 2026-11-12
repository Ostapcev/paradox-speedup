"""Translate Steam eu4.exe 1.37.5 addresses to the GOG build.

Usage:
    python -I map_rvas.py STEAM_EXE GOG_EXE FUNCTIONS_JSON [RVA ...]
    python -I map_rvas.py STEAM_EXE GOG_EXE FUNCTIONS_JSON --scan ROOT OUT_JSON

An address inside a matched function maps to the same instruction of the GOG function
(plus the same offset inside that instruction). A data address maps through the RIP-relative
references collected by match_functions.py, or relative to the nearest mapped data address in
the same section when the distance to both neighbours agrees.

--scan reads every hex constant in the plugin sources under ROOT (eu4_*_patch, eu4_ai_serial),
keeps those that look like addresses in eu4.exe and writes OUT_JSON:
{steam_rva: {"gog": rva or null, "how": ..., "uses": ["file:line", ...]}}.
"""
import bisect
import difflib
import glob
import json
import os
import re
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from match_functions import BRANCH, RIP, Image  # noqa: E402

BASE = 0x140000000


class Mapper:
    def __init__(self, steam, gog, functions):
        self.a, self.b = Image(steam), Image(gog)
        j = json.load(open(functions))
        self.funcs = j['functions']
        self.fstarts = [f[0] for f in self.funcs]
        self.data = j['data']
        self.dstarts = [d[0] for d in self.data]
        self.all_starts = [f[0] for f in self.a.funcs]
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

    def section(self, im, rva):
        for name, (va, size) in im.sections.items():
            if va <= rva < va + size:
                return name
        return None

    def tokens(self, im, b, e):
        out = []
        for addr, size, mn, ops in self.md.disasm_lite(im.img[b:e], b):
            ops = RIP.sub('rip+X', ops)
            if BRANCH.match(mn) and ops.startswith('0x'):
                ops = 'T'
            out.append(((addr, size), mn + ' ' + ops))
        return out

    def fuzzy(self, rva):
        """Function changed between builds: find it between matched neighbours, align insns."""
        k = bisect.bisect_right(self.all_starts, rva) - 1
        sb, se = self.a.funcs[k]
        if not sb <= rva < se:
            return None
        i = bisect.bisect_right(self.fstarts, sb) - 1
        prev = self.funcs[i] if i >= 0 else None
        nxt = self.funcs[i + 1] if i + 1 < len(self.funcs) else None
        if not prev or not nxt:
            return None
        cands = [f for f in self.b.funcs if prev[3] <= f[0] and f[1] <= nxt[2]]
        ta = self.tokens(self.a, sb, se)
        best = None
        for gb, ge in cands:
            tb = self.tokens(self.b, gb, ge)
            sm = difflib.SequenceMatcher(None, [t for _, t in ta], [t for _, t in tb], autojunk=False)
            r = sm.ratio()
            if best is None or r > best[0]:
                best = (r, gb, ge, tb, sm)
        if not best or best[0] < 0.6:
            return None
        r, gb, ge, tb, sm = best
        for tag, a0, a1, b0, b1 in sm.get_opcodes():
            if tag != 'equal':
                continue
            for x, y in zip(ta[a0:a1], tb[b0:b1]):
                (xa, xs), (ya, ys) = x[0], y[0]
                if xa <= rva < xa + xs and xs == ys:
                    return ya + (rva - xa), 'changed function %.0f%%' % (r * 100)
        return None

    def code(self, rva):
        i = bisect.bisect_right(self.fstarts, rva) - 1
        if i < 0:
            return self.fuzzy(rva)
        sb, se, gb, ge = self.funcs[i]
        if not sb <= rva < se:
            return self.fuzzy(rva)
        if rva == sb:
            return gb, 'function'
        ia = list(self.md.disasm_lite(self.a.img[sb:se], sb))
        ib = list(self.md.disasm_lite(self.b.img[gb:ge], gb))
        if len(ia) != len(ib):
            return None
        for x, y in zip(ia, ib):
            if x[0] <= rva < x[0] + x[1]:
                if x[2] != y[2] or x[1] != y[1]:
                    return None
                return y[0] + (rva - x[0]), 'instruction'
        if rva == se:
            return ge, 'function end'
        return None

    def datum(self, rva):
        i = bisect.bisect_left(self.dstarts, rva)
        if i < len(self.data) and self.data[i][0] == rva:
            return self.data[i][1], 'reference'
        sec = self.section(self.a, rva)
        lo = self.data[i - 1] if i > 0 else None
        hi = self.data[i] if i < len(self.data) else None
        if lo and hi and self.section(self.a, lo[0]) == sec == self.section(self.a, hi[0]):
            dlo, dhi = lo[1] - lo[0], hi[1] - hi[0]
            if dlo == dhi:
                return rva + dlo, 'between references'
        return None

    def slot(self, rva):
        """A vtable slot: the GOG slot that holds the mapped function pointer."""
        if rva % 8:
            return None
        ptr = int.from_bytes(self.a.img[rva:rva + 8], 'little')
        if not BASE <= ptr < BASE + self.a.size or self.section(self.a, ptr - BASE) != '.text':
            return None
        t = self.code(ptr - BASE)
        if not t:
            return None
        needle = (BASE + t[0]).to_bytes(8, 'little')
        va, size = self.b.sections['.rdata']
        hits = [m.start() for m in re.finditer(re.escape(needle), self.b.img[va:va + size])
                if m.start() % 8 == (rva - self.a.sections['.rdata'][0]) % 8]
        if len(hits) == 1:
            return va + hits[0], 'vtable slot'
        # several vtables share the function: pick the one whose neighbouring slots also match
        best = []
        for h in hits:
            g = va + h
            score = 0
            for d in (-16, -8, 8, 16):
                p = int.from_bytes(self.a.img[rva + d:rva + d + 8], 'little')
                q = int.from_bytes(self.b.img[g + d:g + d + 8], 'little')
                if BASE <= p < BASE + self.a.size and self.section(self.a, p - BASE) == '.text':
                    c = self.code(p - BASE)
                    score += bool(c and c[0] + BASE == q)
            best.append((score, g))
        best.sort(reverse=True)
        if best and best[0][0] >= 3 and (len(best) == 1 or best[1][0] < best[0][0]):
            return best[0][1], 'vtable slot (neighbours)'
        return None

    def pattern(self, rva):
        """Unique byte pattern of the following instructions; displacements are wildcards."""
        va, size = self.a.sections['.text']
        ins = list(self.md.disasm_lite(self.a.img[rva:rva + 64], rva))
        parts = []
        length = 0
        for addr, sz, mn, ops in ins:
            raw = self.a.img[addr:addr + sz]
            if RIP.search(ops) or (BRANCH.match(mn) and ops.startswith('0x') and sz >= 5):
                parts.append(re.escape(raw[:sz - 4]) + b'.{4}' if not RIP.search(ops) or True else b'')
            else:
                parts.append(re.escape(raw))
            length += sz
            if length >= 24:
                rx = re.compile(b''.join(parts), re.S)
                vb, sb = self.b.sections['.text']
                hits = [m.start() for m in rx.finditer(self.b.img, vb, vb + sb)]
                if len(hits) == 1:
                    return hits[0], 'byte pattern'
                if not hits:
                    return None
        return None

    def map(self, rva):
        sec = self.section(self.a, rva)
        if sec == '.text':
            return self.code(rva) or self.pattern(rva)
        if sec in ('.rdata', '.data', '_RDATA'):
            r = self.datum(rva) if sec != '.rdata' else None
            return r or self.slot(rva) or self.datum(rva)
        return None


HEXNUM = re.compile(r'(?<![0-9A-Za-z_])0x([0-9a-fA-F]{5,9})(?![0-9A-Za-z_])|(?<![0-9A-Za-z_])(14[0-9a-fA-F]{7})(?![0-9A-Za-z_])')


def scan(m, root, out):
    files = [f for pat in ('eu4_*_patch/src/**/*', 'eu4_ai_serial/src/**/*', 'eu4_*_patch/*.ini',
                           'eu4_ai_serial/*.ini')
             for f in glob.glob(os.path.join(root, pat), recursive=True)
             if os.path.isfile(f) and os.path.splitext(f)[1] in ('.cpp', '.hpp', '.inc', '.S', '.h', '.c', '.ini')]
    found = {}
    for f in sorted(files):
        rel = os.path.relpath(f, root).replace('\\', '/')
        for n, line in enumerate(open(f, encoding='utf-8', errors='ignore'), 1):
            code = line.split('//')[0]
            for g1, g2 in HEXNUM.findall(code):
                v = int(g1 or g2, 16)
                if v >= BASE:
                    v -= BASE
                if not 0x1000 <= v < m.a.size:
                    continue
                if m.section(m.a, v) is None:
                    continue
                found.setdefault(v, []).append('%s:%d' % (rel, n))
    res = {}
    for v, uses in sorted(found.items()):
        r = m.map(v)
        res['0x%x' % v] = {'gog': '0x%x' % r[0] if r else None, 'how': r[1] if r else None,
                           'section': m.section(m.a, v), 'uses': uses}
    json.dump(res, open(out, 'w'), indent=1)
    ok = sum(1 for x in res.values() if x['gog'])
    print('addresses', len(res), 'mapped', ok)


def main():
    a = sys.argv[1:]
    m = Mapper(a[0], a[1], a[2])
    if len(a) > 3 and a[3] == '--scan':
        scan(m, a[4], a[5])
        return
    for s in a[3:]:
        v = int(s, 16)
        if v >= BASE:
            v -= BASE
        r = m.map(v)
        print('0x%x -> %s' % (v, ('0x%x (%s)' % r) if r else 'NOT MAPPED'))


if __name__ == '__main__':
    main()
