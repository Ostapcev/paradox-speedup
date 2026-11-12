"""Static checks of include/eu4_gog_table.inc against both executables.

Usage: python check_table.py STEAM_EXE GOG_EXE

1. The verified GOG bytes of every entry are what the GOG exe has there.
2. Every CALL/JMP rel32 inside a verified window points, in the GOG build, to the GOG
   address of its Steam target (when that target is itself a table entry or a matched
   function start). This catches a window that lines up by instruction text but sits in
   the wrong copy of a duplicated function.
"""
import json
import os
import re
import sys

import capstone

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from match_functions import Image  # noqa: E402

ROOT = os.path.dirname(HERE)


def main():
    a, b = Image(sys.argv[1]), Image(sys.argv[2])
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    funcs = json.load(open(os.path.join(ROOT, 'data', 'functions.json')))['functions']
    fmap = {f[0]: f[2] for f in funcs}
    rows = []
    for line in open(os.path.join(ROOT, 'include', 'eu4_gog_table.inc')):
        m = re.match(r'\{0x([0-9a-f]+), 0x([0-9a-f]+), +(\d+), "(.*)"\},', line)
        if m:
            data = bytes(int(x, 16) for x in re.findall(r'\\x([0-9a-f]{2})', m.group(4)))
            rows.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3)), data))
    tmap = {s: g for s, g, _, _ in rows}
    bad = calls = 0
    for s, g, n, data in rows:
        if b.img[g:g + n] != data:
            print('bytes differ', hex(s))
            bad += 1
        ia = list(md.disasm_lite(a.img[s:s + n], s))
        ib = list(md.disasm_lite(b.img[g:g + n], g))
        for x, y in zip(ia, ib):
            if x[2] in ('call', 'jmp') and x[3].startswith('0x') and x[1] == 5:
                ta, tb = int(x[3], 16), int(y[3], 16)
                want = tmap.get(ta, fmap.get(ta))
                if want is None:
                    continue
                calls += 1
                if want != tb:
                    print('call target differs at', hex(x[0]), hex(ta), '->', hex(tb), 'want', hex(want))
                    bad += 1
    print('entries', len(rows), 'calls checked', calls, 'problems', bad)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
