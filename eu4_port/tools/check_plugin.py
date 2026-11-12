"""Check a converted plugin's address uses against both executables, without running EU4.

Usage: python check_plugin.py STEAM_EXE GOG_EXE SOURCE.cpp [...]

For every `eu4build::code_matches(image, X, NAME, sizeof(NAME))` with X and NAME defined in
the same files (constexpr hex constants and byte arrays) it reports whether the check passes
on Steam (the bytes equal the Steam exe) and on GOG (the table has X and covers NAME).
For every `eu4build::rva(X)` it reports whether X is in the table.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from match_functions import Image  # noqa: E402

ROOT = os.path.dirname(HERE)


def table():
    rows = []
    for line in open(os.path.join(ROOT, 'include', 'eu4_gog_table.inc')):
        m = re.match(r'\{0x([0-9a-f]+), 0x([0-9a-f]+), +(\d+),', line)
        if m:
            rows.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))))
    return sorted(rows)


def find(rows, x):
    best = None
    for s, g, n in rows:
        if s <= x and (s == x or x <= s + n):
            best = (s, g, n)
    return best


def main():
    a = Image(sys.argv[1])
    rows = table()
    text = ''
    for f in sys.argv[3:]:
        text += open(f, encoding='utf-8').read() + '\n'
    code = re.sub(r'//[^\n]*', '', text)
    consts = {m.group(1): int(m.group(2), 16)
              for m in re.finditer(r'constexpr std::uintptr_t (\w+) = 0x([0-9a-fA-F]+)', code)}
    carrays = {k: [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)', body)] for k, body in
               re.findall(r'std::uintptr_t (\w+)\[\w*\] = \{([^}]*)\}', code)}
    arrays = {}
    for m in re.finditer(r'(?:constexpr|static const|static constexpr) (?:std::uint8_t (\w+)\[\d*\] = |std::array<std::uint8_t, \d+> (\w+)\s*)\{([^}]*)\}', code):
        arrays[m.group(1) or m.group(2)] = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{1,2})\b', m.group(3)))
    for m in []:
        arrays[m.group(1)] = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{1,2})\b', m.group(2)))

    def value(tok):
        tok = tok.strip()
        if tok in consts:
            return consts[tok]
        if re.fullmatch(r'0x[0-9a-fA-F]+', tok):
            return int(tok, 16)
        m = re.fullmatch(r'(?:\w+::)?(\w+)\s*([+-])\s*(0x[0-9a-fA-F]+|\d+)', tok)
        if m and m.group(1) in consts:
            d = int(m.group(3), 0)
            return consts[m.group(1)] + (d if m.group(2) == '+' else -d)
        m = re.fullmatch(r'(\w+)\[(\d+)\]', tok)
        if m and m.group(1) in carrays and int(m.group(2)) < len(carrays[m.group(1)]):
            return carrays[m.group(1)][int(m.group(2))]
        m = re.fullmatch(r'\w+::(\w+)', tok)
        if m and m.group(1) in consts:
            return consts[m.group(1)]
        return None

    problems = 0
    checks = [(m.group(1), m.group(2), m.group(0)) for m in
              re.finditer(r'code_matches\(\s*\w+,\s*([^,]+),\s*([\w\[\]]+),\s*(?:sizeof\([\w\[\]]+\)|\d+)\)', code)]
    checks += [(m.group(1), m.group(2), m.group(0)) for m in
               re.finditer(r'\bexpect\(\s*image,\s*([^,]+),\s*(\w+),', code)]
    # hex-string signatures: {0xRVA, "48895c24..", "what"}
    for m in re.finditer(r'\{\s*0x([0-9a-fA-F]{5,8}),\s*"([0-9a-f]{4,})"', code):
        arrays['sig_' + m.group(1)] = bytes.fromhex(m.group(2))
        checks.append(('0x' + m.group(1), 'sig_' + m.group(1), m.group(0)))
    # table-driven checks: {site_rva, site_bytes, ...} initializers
    checks += [(m.group(1), m.group(2), m.group(0)) for m in
               re.finditer(r'\{\s*(\w+_rva|0x[0-9a-fA-F]{5,8}),\s*(\w+_bytes)\b', code)]
    for tok, name, whole in checks:
        x = value(tok)
        data = arrays.get(name)
        mlen = re.search(r',\s*(\d+)\)$', whole)
        if data is not None and mlen:
            data = data[:int(mlen.group(1))]
        if x is None or data is None:
            print('?? cannot resolve', whole[:80])
            continue
        steam_ok = a.img[x:x + len(data)] == data
        e = find(rows, x)
        gog_ok = bool(e) and (x - e[0]) + len(data) <= e[2]
        flag = '' if steam_ok and gog_ok else '   <-- ' + ('' if steam_ok else 'STEAM ') + ('' if gog_ok else 'GOG')
        problems += not (steam_ok and gog_ok)
        print('check %-28s %#9x len %2d steam %-5s gog %-5s%s' % (name, x, len(data), steam_ok, gog_ok, flag))
    seen = set()
    for m in re.finditer(r'eu4build::rva\(([^()]+(?:\([^()]*\))?)\)', code):
        tok = m.group(1)
        if tok in seen:
            continue
        seen.add(tok)
        x = value(tok)
        if x is None:
            print('rva   %-40s (runtime value)' % tok)
            continue
        e = find(rows, x)
        if not e:
            problems += 1
        print('rva   %-28s %#9x -> %s' % (tok, x, hex(e[1] + x - e[0]) if e else 'MISSING   <--'))
    print('problems', problems)


if __name__ == '__main__':
    main()
