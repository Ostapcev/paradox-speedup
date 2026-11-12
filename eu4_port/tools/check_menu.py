"""Offline check of eu4_menu_patch's check_all() against the Steam and GOG eu4.exe 1.37.5.

Usage: python check_menu.py STEAM_EXE GOG_EXE MENU_CPP

Simulates what the plugin does on each build: every expect(rva::X, {steam}, "what"[, {gog}])
and expect_pointer(rva::S, rva::P, ...), with GOG addresses from eu4_gog_table.inc (plus the
plugin's one explicit override), and lists every rva:: constant that has no GOG address.
"""
import re
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from match_functions import Image  # noqa: E402
from check_plugin import table, find  # noqa: E402

BASE = 0x140000000
OVERRIDE = {0x8165F2: 0x815952}


def main():
    steam, gog = Image(sys.argv[1]), Image(sys.argv[2])
    src = re.sub(r'//[^\n]*', '', open(sys.argv[3], encoding='utf-8').read())
    rows = table()
    ns = re.search(r'namespace rva \{(.*?)\}', src, re.S).group(1)
    rv = {k: int(v, 16) for k, v in re.findall(r'(\w+) = 0x([0-9A-Fa-f]+)', ns)}

    def g(x):
        if x in OVERRIDE:
            return OVERRIDE[x], None
        e = find(rows, x)
        return (e[1] + x - e[0], e) if e else (None, None)

    def blist(t):
        return bytes(int(b, 16) for b in re.findall(r'0x([0-9A-Fa-f]{2})', t))

    named = {m.group(1): blist(m.group(2)) for m in
             re.finditer(r'initializer_list<std::uint8_t> (\w+) = \{([^}]*)\}', src)}
    problems = 0
    for m in re.finditer(r'expect\(rva::(\w+),\s*(\{[^}]*\}|\w+),\s*"(\w+)"(?:,\s*(\{[^}]*\}))?\)', src):
        name, sb, what, gb = m.groups()
        x = rv[name]
        sbytes = named[sb] if not sb.startswith('{') else blist(sb)
        s_ok = steam.img[x:x + len(sbytes)] == sbytes
        ga, e = g(x)
        if ga is None:
            g_ok, how = False, 'NO ADDRESS'
        elif gb:
            gbytes = blist(gb)
            g_ok, how = gog.img[ga:ga + len(gbytes)] == gbytes, 'gog bytes'
        else:
            g_ok = e is not None and (x - e[0]) + len(sbytes) <= e[2]
            how = 'table window %d' % (e[2] if e else -1)
        lobby = name.startswith('lobby_')
        bad = not s_ok or (not g_ok and not lobby)
        problems += bad
        print('%-26s steam %#9x %-5s gog %-10s %-5s %s%s' % (what, x, s_ok, hex(ga) if ga else '-', g_ok, how,
              '  (skipped on GOG)' if lobby else '', ) + ('   <--' if bad else ''))
    for m in re.finditer(r'expect_pointer\(rva::(\w+),\s*rva::(\w+)', src):
        a, b = rv[m.group(1)], rv[m.group(2)]
        s_ok = int.from_bytes(steam.img[a:a + 8], 'little') == BASE + b
        ga, gb_ = g(a)[0], g(b)[0]
        g_ok = ga is not None and gb_ is not None and int.from_bytes(gog.img[ga:ga + 8], 'little') == BASE + gb_
        lobby = 'lobby' in m.group(1)
        bad = not s_ok or (not g_ok and not lobby)
        problems += bad
        print('ptr %-22s steam %-5s gog %-5s %s%s' % (m.group(1), s_ok, g_ok, '(skipped on GOG)' if lobby else '',
              '   <--' if bad else ''))
    for k, x in sorted(rv.items()):
        if g(x)[0] is None and 'lobby' not in k:
            problems += 1
            print('no GOG address for rva::%s %#x   <--' % (k, x))
    # the code sites patched: the window must cover the bytes moved (stubs copy them)
    print('problems', problems)


if __name__ == '__main__':
    main()
