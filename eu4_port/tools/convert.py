"""Rewrite a plugin's EU4 address uses to go through eu4build (Steam and GOG 1.37.5).

Usage: python convert.py FILE.cpp [...] [--base NAME ...]

- `BASE + X` where X is a *_rva constant, a hex literal of 5+ digits, `arr[i]` of an *_rva(s)
  array, or `X + offset` of those, becomes `BASE + eu4build::rva(X)`.
- `std::memcmp(reinterpret_cast<...>(BASE + eu4build::rva(X)), B, sizeof(B)) != 0` (or `== 0`)
  for code X becomes `!eu4build::code_matches(BASE, X, B, sizeof(B))` (or without `!`).
  Data addresses keep memcmp.
- adds #include "eu4_build.hpp".
The result still needs review: check_plugin.py, and a look at what the script printed as left.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP = json.load(open(os.path.join(ROOT, 'data', 'rva_map.json')))


def section_of(value):
    e = MAP.get('0x%x' % value)
    return e['section'] if e else None


def convert(path, bases):
    s = open(path, encoding='utf-8').read()
    orig = s
    consts = {m.group(1): int(m.group(2), 16)
              for m in re.finditer(r'constexpr std::uintptr_t (\w+) = 0x([0-9a-fA-F]+)', s)}
    b = '|'.join(map(re.escape, bases))
    addr = r'(?:[A-Za-z_]\w*_(?:rva|RVA)s?\[\w+\]|[A-Za-z_]\w*_(?:rva|RVA)|0x[0-9a-fA-F]{5,8})(?:\s*\+\s*(?:0x[0-9a-fA-F]+|\d+))?'
    s = re.sub(r'\b(%s) \+ (%s)(?![\w(])' % (b, addr), lambda m: '%s + eu4build::rva(%s)' % (m.group(1), m.group(2)), s)

    def is_code(tok):
        tok = tok.strip()
        m = re.fullmatch(r'(\w+)(?:\s*\+\s*(0x[0-9a-fA-F]+|\d+))?', tok)
        if not m:
            return None
        v = consts.get(m.group(1))
        if v is None and m.group(1).startswith('0x'):
            v = int(m.group(1), 16)
        if v is None:
            return None
        return section_of(v) == '.text'

    def repl(m):
        base, tok, arr, op = m.group(1), m.group(2), m.group(3), m.group(4)
        if not is_code(tok):
            return m.group(0)
        neg = '!' if op == '!=' else ''
        return '%seu4build::code_matches(%s, %s, %s, sizeof(%s))' % (neg, base, tok, arr, arr)
    s = re.sub(r'std::memcmp\(\s*reinterpret_cast<(?:const )?(?:void|std::uint8_t|unsigned char)\s*\*>\((%s) \+ eu4build::rva\(([^()]+)\)\),\s*(\w+),\s*sizeof\(\3\)\)\s*(!=|==)\s*0' % b, repl, s)
    # general form: memcmp(cast(BASE + eu4build::rva(X) [+-] N), ARR, LEN) [!= 0 | == 0 | truthy]
    def repl2(m):
        base, tok, adj, arr, ln, op = m.groups()
        tok = tok.strip()
        name = re.sub(r'\[\w+\]$', '', tok)
        if not (is_code(tok) or (tok != name and name.endswith(('rva', 'rvas', 'RVA')))):
            return m.group(0)
        where = tok
        if adj:
            a = adj.replace(' ', '')
            where += ' %s %s' % (a[0], a[1:])
        neg = '' if op and '==' in op else '!'
        return '%seu4build::code_matches(%s, %s, %s, %s)' % (neg, base, where, arr.strip(), ln.strip())
    s = re.sub(r'std::memcmp\(\s*reinterpret_cast<(?:const )?(?:void|std::uint8_t|unsigned char)\s*\*>'
               r'\((%s) \+ eu4build::rva\(([\w]+(?:\[\w+\])?)\)(\s*[+-]\s*(?:0x[0-9a-fA-F]+|\d+))?\),'
               r'\s*([\w\[\]]+),\s*(sizeof\([\w\[\]]+\)|\w+)\)(\s*(?:!=|==)\s*0)?' % b, repl2, s)
    if s != orig and 'eu4_build.hpp' not in s:
        # after the includes at the top of the file (a later #include "x.inc" is code)
        incs = [m for m in re.finditer(r'#include [<"][^>"]+[>"]\n', s) if '.inc"' not in m.group(0)]
        pos = incs[-1].end() if incs else 0
        s = s[:pos] + '#include "eu4_build.hpp"\n' + s[pos:]
    open(path, 'w', encoding='utf-8', newline='').write(s)
    left = [(n + 1, l.strip()) for n, l in enumerate(s.split('\n'))
            if re.search(r'\b(%s) \+ (?!eu4build)' % b, l) or re.search(r'memcmp\([^;]*eu4build::rva', l)]
    print('%s: %d rva, %d code_matches; left:' % (path, s.count('eu4build::rva('), s.count('code_matches(')))
    for n, l in left:
        print('   %d: %s' % (n, l[:150]))


def main():
    args = sys.argv[1:]
    bases = ['image']
    if '--base' in args:
        i = args.index('--base')
        bases = args[i + 1:]
        args = args[:i]
    for f in args:
        convert(f, bases)


if __name__ == '__main__':
    main()
